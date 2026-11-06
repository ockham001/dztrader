/**
 * @file smoke_test.cpp
 * @brief 集成冒烟测试: master + dzmd_ctp + dztd_ctp + dzweb + stg_demo 真实进程组合验证。
 *
 * 流程: 临时 DZTRADER_HOME -> spawn 真实 dztraderd.exe -> master 拉起行情/交易网关 + dzweb +
 * 常驻策略 stg_demo -> 事件通道帧回路断言 + dzweb /health HTTP 探测 + 策略 UI 帧 E2E
 * (等 Running -> 写 UI_INPUT -> 读 OUTPUT_UI 断言 -> PROCESS_CONTROL 停止 -> Stopped)
 * -> 定向 SHUTDOWN (md/td/webui) -> 各进程 Stopped -> 终止 master -> 清理。
 *
 * 注意:
 * - Windows NamedSemaphore 为机器全局命名空间 ("dz.sem.<name>"), 与并行测试/dev 栈互扰,
 *   故启动前检测 dz.sem.dzevent 占用: 默认 GTEST_SKIP, DZ_SMOKE_STRICT=1 时 FAIL (CI 使用)。
 * - ctest 中本测试 RUN_SERIAL (见 apps/master/CMakeLists.txt)。
 */

#include "child_process.h"
#include "config.h"
#include "tests/td_config_fixture.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <gtest/gtest.h>
#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

namespace dztrader {
namespace {

using namespace std::chrono_literals;
using master::Category;
using master::ChildProcess;
using master::ChildState;
using master::ProcessEntry;

/// Boost.Beast 的命名空间在 boost:: 下; 冒烟测试里只用两三个类型, 用别名缩短书写。
namespace beast = boost::beast;

/// smoke 夹具专用 admin 口令（**不得改回默认口令**，见 SetUp 内注释）。
///
/// 为什么不能用默认的 `88888888`：dzweb 启动时若检出 admin 仍在用系统默认口令
/// （`WebuiConfig::admin_password_is_default`），会经 `force_password_change_if_default()`
/// 置 `users.must_change_password=1`；此后 JWT advice 除白名单路径外对**所有** `/api/*`
/// 回 403 `{"code":"password_change_required"}`，WS 也由 `ws_auth` + `session_guard` 拒连。
/// 登录本身仍回 200（这是 `/health` 与本测试历史断言一直没暴露该问题的原因）。
/// 配置**非默认**口令 = 生产部署的实际形态，强制改密门禁不介入。
///
/// 长度必须 ≥ `webui::kMinPasswordLength`(12)，否则自助改密路径会回 400；本夹具不触发
/// 改密，但仍沿用同一口径避免将来加"改密"用例时踩坑。
constexpr const char* kSmokeAdminPassword = "smoke_test_admin_pw_253";

/// 事件通道/行情进程信号量是否已被占用 (机器全局命名空间)。
/// 存在 = dev 栈、其他测试实例或上次运行遗留的孤儿进程, 冒烟测试会与之互扰。
bool event_channel_semaphore_exists() {
#ifdef _WIN32
    for (const char* name : {"dz.sem.dzevent", "dz.sem.dzmd_ctp"}) {
        HANDLE h = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, name);
        if (h != nullptr) {
            CloseHandle(h);
            return true;
        }
    }
    return false;
#else
    return false;  // POSIX 下暂不检测 (v1 目标平台 Windows)
#endif
}

bool smoke_strict() { return env::get("DZ_SMOKE_STRICT") == "1"; }

/// 按 pid 强杀 (等价于 run-dev -Stop 的 Stop-Process -Force)。
/// 不用 ChildProcess::terminate: 外部强杀后其 async_wait 状态回调不触发, 判定不可靠。
void force_kill_pid(uint32_t pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h == nullptr) {
        std::fprintf(stderr, "--- smoke: OpenProcess(TERMINATE) failed pid=%u err=%lu ---\n", pid,
                     GetLastError());
        return;
    }
    if (!TerminateProcess(h, 1)) {
        std::fprintf(stderr, "--- smoke: TerminateProcess failed pid=%u err=%lu ---\n", pid,
                     GetLastError());
    } else {
        std::fprintf(stderr, "--- smoke: TerminateProcess ok pid=%u ---\n", pid);
    }
    CloseHandle(h);
#else
    ::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

/// 进程是否存活 (句柄 SIGNALED = 已死)
bool process_alive(uint32_t pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) != 0) {
        return false;  // ESRCH: 进程不存在
    }
    // SIGKILL 后的进程在被父进程 waitpid 回收前以僵尸态存在, kill(pid,0) 仍成功;
    // 读 /proc/<pid>/stat 状态位, 'Z' 视为已死亡 (与 Windows 句柄语义对齐)
    std::ifstream statf("/proc/" + std::to_string(pid) + "/stat");
    if (statf) {
        std::string line;
        std::getline(statf, line);
        // stat 格式: <pid> (<comm>) <state> ...; comm 可含空格/括号, 取最后一个 ')' 后第 2 个字符
        const auto rparen = line.rfind(')');
        if (rparen != std::string::npos && rparen + 2 < line.size()) {
            return line[rparen + 2] != 'Z';
        }
    }
    return true;
#endif
}

/// 帧是否含 instance_id (与 ProcessControlFrameTest drain 逻辑一致,
/// 00-general §3: 无中央映射表, 接收方按注册约定解析)
bool frame_has_inst(DzFrameType t) {
    switch (t) {
        case DZ_FRAME_PRELOAD_EVENT_SHM:
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER:
        case DZ_FRAME_SET_EVENT_SHM_CONFIG:
        case DZ_FRAME_RTN_EVENT_SHM_CONFIG:
        case DZ_FRAME_QUERY_FULL_SNAPSHOT:
        case DZ_FRAME_NOTIFY_UI:
        case DZ_FRAME_REQUEST_PROCESS_CONTROL:
        case DZ_FRAME_RTN_PROCESS_STATUS:
        case DZ_FRAME_SET_PROCESS_CONFIG:
        case DZ_FRAME_RTN_PROCESS_CONFIG:
            return false;
        default:
            return true;
    }
}

/// basic 帧 (定长结构体 payload 紧跟 DzFrameHeader, 无 ext 头): 无法按 JSON 解析。
/// 策略/交易链路会在事件通道产生此类帧 (TD_ORDER_REQ=DZ_FRAME_TD_ORDER_REQ /
/// ORDER_REPORT=DZ_FRAME_ORDER_REPORT), 混在 ext 帧里, drain 时只记摘要跳过 payload 解析。
/// 注意: PRELOAD_MD_SHM 不是 basic (ext-inst 头, 契约 SHM 通道配置), 见 drain_available
/// 中的显式分支; 只有 PRELOAD_EVENT_SHM 是 basic。
bool frame_is_basic_struct(DzFrameType t) {
    switch (t) {
        case DZ_FRAME_TD_ORDER_REQ:
        case DZ_FRAME_TD_ORDER_CANCEL_REQ:
        case DZ_FRAME_TD_QUERY_ACCOUNT_STATUS:
        case DZ_FRAME_PRELOAD_EVENT_SHM:
        case DZ_FRAME_ORDER_REPORT:
        case DZ_FRAME_TRADE_REPORT:
        case DZ_FRAME_POSITION_INFO:
        case DZ_FRAME_TRADING_ACCOUNT:
        case DZ_FRAME_TICK:
            return true;
        default:
            return false;
    }
}

/// 文本拆行 (OUTPUT_UI 单帧可多行: md dump)
std::vector<std::string> split_text_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string::npos) {
            if (start < text.size()) {
                lines.push_back(text.substr(start));
            }
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

struct DecodedFrame {
    DzFrameType type{};
    std::string instance_id;  // 空 = 无 instance_id 帧
    nlohmann::json payload;   // 空 payload 帧 -> null
    std::string text;         // 变长文本 payload (OUTPUT_UI)
    bool has_payload{false};
};

/// 失败诊断: 打印 home 下某前缀日志文件的尾部 (master/md 的日志在 TearDown 删除前抢救)
void dump_log_tail(const std::filesystem::path& dir, const std::string& prefix, int lines) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& name = it->path().filename().string();
        if (!name.starts_with(prefix)) {
            continue;
        }
        std::ifstream ifs(it->path());
        if (!ifs) {
            continue;
        }
        std::vector<std::string> tail;
        std::string line;
        while (std::getline(ifs, line)) {
            tail.push_back(std::move(line));
            if (tail.size() > static_cast<size_t>(lines)) {
                tail.erase(tail.begin());
            }
        }
        std::fprintf(stderr, "--- %s (last %d lines) ---\n", name.c_str(), lines);
        for (const auto& l : tail) {
            std::fprintf(stderr, "  %s\n", l.c_str());
        }
    }
}

/// 一套最小 HTTP 响应 (裸 TCP 客户端用): 状态码 + JSON body。
///
/// 为什么不引入 drogon HTTP client: dzweb 的 REST 只用到 GET/POST + Bearer header +
/// JSON body, 加起来不到 100 行; 拉入 drogon 客户端会把一个测试目标绑到框架的
/// 连接池/定时器语义上, 反而更难断言"原始 HTTP 状态行"。drogon 侧行为由 webui 的
/// 单测覆盖, 这里只要端到端"真的能通"。
struct HttpResponse {
    int status = 0;       ///< 0 = 连接/发送/解析失败 (不是 HTTP 状态)
    std::string body;     ///< 原始 body (Content-Length 截取)
    nlohmann::json json;  ///< body 解析结果; 解析失败 = null
    bool json_ok = false;
    std::string error;  ///< status == 0 时的失败原因 (诊断用)

    bool ok(int want = 200) const { return status == want; }
};

/// 极简 HTTP/1.1 客户端 (GET/POST + 自定义 header + JSON body)。
///
/// 连接语义: 每次调用独立建连、`Connection: close`、读到 EOF —— 与 `http_get_health`
/// 的单次探测同型, 不维护连接池 (REST 断言之间没有性能要求)。
class HttpClient {
public:
    HttpClient(boost::asio::io_context& ioc, int port)
        : ioc_(ioc),
          port_(port) {}

    /// GET; @param bearer 非空则带 `Authorization: Bearer <token>`
    HttpResponse get(const std::string& path, const std::string& bearer = {}) {
        return request("GET", path, {}, bearer);
    }

    /// POST JSON; @param body 已序列化的 JSON 文本
    HttpResponse post(const std::string& path,
                      const std::string& body,
                      const std::string& bearer = {}) {
        return request("POST", path, body, bearer);
    }

private:
    HttpResponse request(const std::string& method,
                         const std::string& path,
                         const std::string& body,
                         const std::string& bearer) {
        HttpResponse resp;
        boost::system::error_code ec;
        boost::asio::ip::tcp::resolver resolver(ioc_);
        const auto results = resolver.resolve("127.0.0.1", std::to_string(port_), ec);
        if (ec || results.empty()) {
            resp.error = "resolve failed: " + ec.message();
            return resp;
        }
        boost::asio::ip::tcp::socket sock(ioc_);
        sock.connect(*results.begin(), ec);
        if (ec) {
            resp.error = "connect failed: " + ec.message();
            return resp;
        }
        std::string req = method + " " + path + " HTTP/1.1\r\n";
        req += "Host: 127.0.0.1\r\n";
        req += "Connection: close\r\n";
        if (!bearer.empty()) {
            req += "Authorization: Bearer " + bearer + "\r\n";
        }
        if (method == "POST") {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;
        boost::asio::write(sock, boost::asio::buffer(req), ec);
        if (ec) {
            resp.error = "write failed: " + ec.message();
            return resp;
        }
        // 读到 EOF (Connection: close) —— 服务端正常关闭时 read 以 EOF 收尾
        std::string raw;
        boost::system::error_code read_ec;
        char chunk[4096];
        while (true) {
            const std::size_t n = sock.read_some(boost::asio::buffer(chunk), read_ec);
            raw.append(chunk, n);
            if (read_ec) {
                break;  // eof / 连接关闭: body 已读全
            }
        }
        if (raw.empty()) {
            resp.error = "empty response";
            return resp;
        }
        // 状态行: "HTTP/1.1 200 OK"
        const std::size_t line_end = raw.find("\r\n");
        const std::string status_line = raw.substr(0, line_end);
        const std::size_t sp = status_line.find(' ');
        if (sp == std::string::npos) {
            resp.error = "malformed status line: " + status_line;
            return resp;
        }
        resp.status = std::atoi(status_line.c_str() + sp + 1);
        // body = headers 之后; 有 Content-Length 时按它截断 (头部大小写不敏感)
        const std::size_t hdr_end = raw.find("\r\n\r\n");
        if (hdr_end == std::string::npos) {
            resp.error = "no header terminator";
            resp.status = 0;
            return resp;
        }
        resp.body = raw.substr(hdr_end + 4);
        const std::size_t cl_pos = find_header(raw, "content-length");
        if (cl_pos != std::string::npos) {
            const std::size_t value_begin = raw.find(':', cl_pos);
            const std::string_view value(raw.data() + value_begin + 1,
                                         raw.find("\r\n", value_begin) - (value_begin + 1));
            const auto length = static_cast<std::size_t>(std::atoll(std::string(value).c_str()));
            if (length < resp.body.size()) {
                resp.body.resize(length);
            }
        }
        try {
            resp.json = nlohmann::json::parse(resp.body);
            resp.json_ok = true;
        } catch (const std::exception&) {
            // 非 JSON body (例如 /health 的 "ok") 不算错误: 调用方按需检查
        }
        return resp;
    }

    /// 在原始响应里查 header (大小写不敏感), 返回 header 名起始位置; 未找到 = npos。
    static std::size_t find_header(const std::string& raw, std::string_view name) {
        std::size_t pos = 0;
        while ((pos = raw.find("\r\n", pos)) != std::string::npos) {
            pos += 2;
            if (raw.size() - pos < name.size()) {
                return std::string::npos;
            }
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(raw[pos + i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return pos;
            }
        }
        return std::string::npos;
    }

    boost::asio::io_context& ioc_;
    int port_ = 0;
};

/// 最小 WebSocket 客户端 (Boost.Beast, header-only; 已实测与本 target 的
/// `Boost::boost` 传递依赖一起编译+链接通过)。
///
/// 用**同步** API: 冒烟测试的断言节奏是"发一个请求 → 轮询等一条消息", 同步 read 配上
/// `wait_until` 的轮询谓词已经够用, 且不引入 Beast 的异步完成令牌与 io_context 生命
/// 周期的耦合。读一帧只解析 envelope 的 `type` (契约 webui-ws §1/§2), payload 交给
/// 调用方按需取。
class WsClient {
public:
    WsClient(boost::asio::io_context& ioc, int port)
        : ioc_(ioc),
          port_(port) {}

    ~WsClient() { close(); }

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    /// 建连 + 握手。`/ws?token=<jwt>` (契约 webui-ws §1)。
    bool connect(const std::string& token) {
        boost::system::error_code ec;
        boost::asio::ip::tcp::resolver resolver(ioc_);
        const auto results = resolver.resolve("127.0.0.1", std::to_string(port_), ec);
        if (ec || results.empty()) {
            error_ = "resolve failed: " + ec.message();
            return false;
        }
        stream_ = std::make_unique<beast::websocket::stream<beast::tcp_stream>>(ioc_);
        // 注意: expires_after 只对**异步**操作生效。Beast 文档明写
        // "Timeouts are not available when performing blocking calls"
        // (boost/beast/core/basic_stream.hpp 的 Blocking I/O 段), 下面用的是同步
        // connect/handshake/read, 所以本类**没有**真正的超时——失败靠 ctest 的
        // TIMEOUT 兜底(见 apps/master/CMakeLists.txt), 不靠这里的任何时长参数。
        stream_->next_layer().connect(*results.begin(), ec);
        if (ec) {
            error_ = "connect failed: " + ec.message();
            return false;
        }
        // Host 头必须带端口: drogon 的 WS 路由按 path+query 匹配, 但握手校验 Host
        const std::string host = "127.0.0.1:" + std::to_string(port_);
        const std::string target = "/ws?token=" + token;
        stream_->handshake(host, target, ec);
        if (ec) {
            error_ = "handshake failed: " + ec.message();
            return false;
        }
        connected_ = true;
        return true;
    }

    /// 读一条完整消息（**无限期阻塞**直到收到消息或连接关闭）。关闭/错误 = nullopt。
    ///
    /// @param timeout_ms **当前不生效**，保留是为了将来切异步读时直接可用：
    ///        Beast 的同步读不触发 `expires_after`（见 `connect` 内的说明）。调用方
    ///        因此不能靠它做"短超时轮询"——`pump_ws` 的返回值只表示"是否读到了一条"，
    ///        "读不到"会一直阻塞，由 ctest TIMEOUT 兜底为失败（而不是假绿）。
    std::optional<nlohmann::json> read_message(int timeout_ms) {
        (void)timeout_ms;  // 同步读不生效（见上）；保留参数便于将来切异步读
        if (!connected_) {
            return std::nullopt;
        }
        beast::flat_buffer buffer;
        boost::system::error_code ec;
        stream_->read(buffer, ec);
        if (ec) {
            error_ = "read failed: " + ec.message();
            return std::nullopt;
        }
        const std::string text = beast::buffers_to_string(buffer.data());
        try {
            return nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            error_ =
                std::string("non-json ws message: ") + e.what() + " | text=" + text.substr(0, 200);
            return nlohmann::json{{"type", "__unparsable__"}, {"raw", text.substr(0, 200)}};
        }
    }

    void close() {
        if (!stream_) {
            return;
        }
        if (connected_) {
            boost::system::error_code ignored;
            stream_->close(beast::websocket::close_reason(beast::websocket::close_code::normal),
                           ignored);
            connected_ = false;
        }
        boost::system::error_code ignored;
        stream_->next_layer().socket().close(ignored);
        stream_.reset();
    }

    const std::string& error() const { return error_; }

private:
    boost::asio::io_context& ioc_;
    int port_ = 0;
    std::unique_ptr<beast::websocket::stream<beast::tcp_stream>> stream_;
    bool connected_ = false;
    std::string error_;
};

class SmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 冲突检测 (NamedSemaphore 机器全局): 开发机跑着 dev 栈时 Skip 不误报;
        // CI 设 DZ_SMOKE_STRICT=1, 冲突即失败 (防止冒烟被静默跳过造成假绿)
        if (event_channel_semaphore_exists()) {
            if (smoke_strict()) {
                FAIL() << "dz.sem.dzevent already exists (dev stack running?) - DZ_SMOKE_STRICT=1";
                return;
            }
            GTEST_SKIP() << "dz.sem.dzevent already exists (dev stack running?); "
                            "set DZ_SMOKE_STRICT=1 to fail instead of skip";
        }

        home_ = std::filesystem::temp_directory_path() /
                ("dz_smoke_" + std::to_string(this_process::pid()));
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        std::filesystem::create_directories(home_ / "configs", ec);
        ASSERT_FALSE(ec) << "create temp home failed: " << ec.message();
        orig_home_ = env::get("DZTRADER_HOME");
        env::set("DZTRADER_HOME", home_.string());

        // 测试 exe 在 <out>/tests/, master/网关/策略 exe 在 <out>/
        out_dir_ = this_process::exe_dir().parent_path();

        // 最小 dztraderd.json: 注册 dzmd_ctp + dztd_ctp + dzweb + 常驻策略 stg_demo
        // (网关段模板与 ProcessControlFrameTest 一致; td/webui 段见 config.cpp parse_master_json)
        // 不写 configs/dzmd_ctp.json: 顺带回归"配置缺失自愈"路径 (历史崩溃 bug, td 侧
        // TdConfig::load 对缺失文件走 exists 短路, 冒烟由 md 覆盖共享模式即可)
        // 策略: exe 名即策略ID (stg_demo), 必须与 strategy[].name 一致; 常驻策略不传 args
        // (不再有 CLI 模式), md_source 必填且 ready 后 master 才拉起策略
        {
            const auto strategy_exe = out_dir_ /
                                      "stg_demo"
#ifdef _WIN32
                                      ".exe"
#endif
                ;
            nlohmann::json cfg = {
                {"log", nlohmann::json::object()},
                {"master", nlohmann::json::object()},
                {"shm",
                 {{"meta_file_size", 1048576},
                  {"event",
                   {{"page_size_mb", 1},
                    {"preload_points", nlohmann::json::object()},
                    {"check_interval_min", 5},
                    {"check_pages", 1},
                    {"check_bytes", 0}}}}},
                {"md",
                 {{"dzmd_ctp",
                   {{"args", nlohmann::json::array()},
                    {"env", nlohmann::json::object()},
                    {"restart", {{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}},
                    {"display_name", "CTP行情"}}}}},
                {"td",
                 {{"dztd_ctp",
                   {{"args", nlohmann::json::array()},
                    {"env", nlohmann::json::object()},
                    {"restart", {{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}},
                    {"display_name", "CTP交易"}}}}},
                {"webui",
                 {{"args", nlohmann::json::array()},
                  {"restart", {{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}}}},
                {"strategy", nlohmann::json::array({{{"name", "stg_demo"},
                                                     {"exe", strategy_exe.string()},
                                                     {"md_source", "dzmd_ctp"}}})},
            };
            std::ofstream ofs(home_ / "configs" / "dztraderd.json");
            ofs << cfg.dump();
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write dztraderd.json failed";
        }

        // dzweb 配置: 随机高位端口 (避免与 dev 栈/其他实例冲突; 机器全局端口空间)
        webui_port_ = 20000 + (this_process::pid() % 30000);
        // admin 口令**必须是 kSmokeAdminPassword（非系统默认口令）**，理由见该常量注释：
        // 用默认的 88888888 会让 dzweb 启动即置 must_change_password=1，之后所有 /api/*
        // 与 WS 都被 403 password_change_required 拦住（#253 实测）。别改回默认值。
        {
            std::ofstream ofs(home_ / "configs" / "webui.json");
            ofs << "{\"server\":{\"listen\":\"0.0.0.0\",\"port\":" << webui_port_ << "},"
                << "\"log\":{\"level\":\"info\",\"flush_on\":\"info\"},"
                << "\"auth\":{\"jwt_secret\":\"smoke_test_secret\",\"token_ttl_sec\":3600},"
                << "\"admin\":{\"username\":\"admin\",\"password\":\"" << kSmokeAdminPassword
                << "\"},"
                << "\"notify\":{\"cache_size\":100}}";
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write webui.json failed";
        }

        // dztd_ctp 配置: 空账户闭环 — 配置账户 CTP001 但从不连接 (无 auto_login 段,
        // 无 --recover), 会话懒创建于 connect 不会发生; 下单走 no_session 拒单回报。
        // 形态不手写: 走 apps/ctp/tests/td_config_fixture.h（单一真相源 = TdConfig::to_json）。
        // #251 把连接参数移入账户 ext 后，手写旧形态会让 dztd_ctp 启动即 CRITICAL 退出、
        // 冒烟超时；夹具让形状变更在构造它的测试 target 编译期就红。
        {
            dztrader::test::TdConfigSeed seed;
            seed.account_id = "CTP001";
            seed.user_id = "SMOKE001";
            seed.password = "";
            std::ofstream ofs(home_ / "configs" / "dztd_ctp.json");
            ofs << dztrader::test::make_td_config_json(seed).dump();
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write dztd_ctp.json failed";
        }

        // master 可执行文件: 测试 exe 在 <out>/tests/, master 在 <out>/
        master_exe_ = out_dir_ /
                      "dztraderd"
#ifdef _WIN32
                      ".exe"
#endif
            ;

        ProcessEntry entry;
        entry.name = "dztraderd";
        entry.category = Category::GatewayMd;  // 类别不影响 ChildProcess 行为 (仅日志/命名)
        entry.exe = master_exe_;
        master_ = ChildProcess::create(ioc_, std::move(entry));
        boost::system::error_code spawn_ec;
        ASSERT_TRUE(master_->start(spawn_ec))
            << "master spawn failed | exe=" << master_exe_.string() << " error=\""
            << spawn_ec.message() << "\"";
    }

    void TearDown() override {
        // 断言提前失败时策略/网关可能仍在跑: 经事件通道补发定向 SHUTDOWN, 给 master
        // 一个回收窗口, 避免常驻 stg_demo 成为占全局信号量的孤儿进程 (Windows 还会
        // 阻塞临时 home 删除)。成功路径测试已自行停止策略, 不重复付出等待成本。
        if (::testing::Test::HasFailure() && writer_ && master_ && master_->pid() != 0 &&
            process_alive(static_cast<uint32_t>(master_->pid()))) {
            for (const char* target : {"stg_demo", "dzmd_ctp", "dztd_ctp", "dzweb"}) {
                platform::write_ext_inst_raw(*writer_, DZ_FRAME_SHUTDOWN, target);
            }
            writer_->notify_subscribers();
            ioc_.run_for(1500ms);
        }
        reader_.reset();
        writer_.reset();
        // 兜底: 断言提前失败时 master 可能还活着 (A5 的强杀没走到), 按 liveness 清理
        // pid 必须非 0: spawn 失败时 pid()==0, Linux 下 kill(0, SIGKILL) 会
        // 误杀整个进程组 (含 ctest 自身), 只清理真实存活进程
        if (master_ && master_->pid() != 0 &&
            process_alive(static_cast<uint32_t>(master_->pid()))) {
            force_kill_pid(static_cast<uint32_t>(master_->pid()));
            ioc_.run_for(1s);
        }
        master_.reset();
        if (::testing::Test::HasFailure()) {
            std::fprintf(stderr, "--- smoke test: frames received ---\n");
            for (const auto& s : frame_summary_) {
                std::fprintf(stderr, "  %s\n", s.c_str());
            }
            std::fprintf(stderr, "--- smoke test: process logs ---\n");
            dump_log_tail(home_ / "logs", "dztraderd_", 40);
            dump_log_tail(home_ / "logs", "dzmd_ctp_", 40);
        }
        if (orig_home_) {
            env::set("DZTRADER_HOME", *orig_home_);
        } else {
            env::unset("DZTRADER_HOME");
        }
        std::error_code ec;
        if (env::get("DZ_SMOKE_KEEP_HOME") != "1") {
            std::filesystem::remove_all(home_, ec);
        } else {
            std::fprintf(stderr, "--- smoke test: kept temp home for diagnosis: %s ---\n",
                         home_.string().c_str());
        }
    }

    /// 打开事件通道 writer/reader (master 启动后创建通道)。
    /// 5ms 快速轮询: 通道在 master 初始化时创建、早于子进程 spawn, 因此成功打开通常
    /// 早于策略启动; 但"打开即保证早于策略启动"并非硬契约 —— 极端调度下仍可能错过
    /// 一次性的 [ready], 故 ready 断言失败时输出已收 OUTPUT_UI 行供诊断。
    bool open_event_channel() {
        for (int i = 0; i < 4000; ++i) {  // 20s
            try {
                auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), paths::shm());
                writer_ = std::make_unique<shm::MultiWriter>(shm::MultiWriter::create(
                    std::make_shared<shm::ChannelMeta>(std::move(meta)), "smoke_writer"));
                reader_ = std::make_unique<shm::Reader>(shm::Reader::create(
                    shm::channel_name("dzevent"), paths::shm(), "smoke_reader"));
                return true;
            } catch (const std::exception&) {
                ioc_.run_for(5ms);  // 保持 io_context 推进 (子进程 spawn/管道回调)
            }
        }
        return false;
    }

    /// 排空 reader 当前可读的全部帧 (非阻塞), 并记录摘要用于失败诊断
    std::vector<DecodedFrame> drain_available(shm::Reader& reader) {
        std::vector<DecodedFrame> out;
        for (int i = 0; i < 256; ++i) {
            const auto* frame = reader.next_frame();
            if (!frame) {
                break;
            }
            shm::FrameView view(frame);
            DecodedFrame df;
            df.type = view.type();
            std::string summary = std::to_string(static_cast<int>(df.type));
            if (df.type == DZ_FRAME_ACCOUNT_STATUS &&
                view.frame_size() >= sizeof(DzFrameHeader) + sizeof(DzAccountStatus)) {
                // 账户状态 basic 广播帧 (契约 account-status): 定长 payload, 无扩展头,
                // 拷出累积 (帧指针在下次 next_frame 失效)
                DzAccountStatus status;
                std::memcpy(&status, &view.payload<DzAccountStatus>(), sizeof(status));
                account_status_frames_.push_back(status);
                summary += " acct=" + std::string(status.account_id) +
                           " gw=" + std::string(status.gateway_name) +
                           " state=" + std::to_string(static_cast<int>(status.state));
            } else if (df.type == DZ_FRAME_UI_INPUT) {
                // 测试自身写出的 UI_INPUT 文本帧 (广播介质对所有读者可见): 非 JSON,
                // 只记摘要, 不进策略输出断言集合
                df.instance_id = std::string(view.ext_inst_id());
                const auto size = view.ext_inst_payload_size();
                df.text.assign(reinterpret_cast<const char*>(view.ext_inst_payload()), size);
                summary += " inst=" + df.instance_id + " ui_input=" + df.text;
            } else if (df.type == DZ_FRAME_OUTPUT_UI) {
                // 策略上行文本帧 (契约 strategy: 变长 UTF-8 文本, 无固定 schema, 不是 JSON):
                // 只收 instance_id == 裸策略名的帧 (身份边界断言), 拷出累积供 UI E2E 断言
                df.instance_id = std::string(view.ext_inst_id());
                const auto size = view.ext_inst_payload_size();
                df.text.assign(reinterpret_cast<const char*>(view.ext_inst_payload()), size);
                if (df.instance_id != "stg_demo") {
                    ++unexpected_ui_instance_count_;
                    summary += " unexpected_ui_instance=" + df.instance_id;
                } else {
                    ui_frames_.push_back(df.text);
                    for (auto& line : split_text_lines(df.text)) {
                        ui_lines_.push_back(std::move(line));
                    }
                }
                summary += " inst=" + df.instance_id + " text=" + df.text;
            } else if (df.type == DZ_FRAME_PRELOAD_MD_SHM) {
                // 《帧契约：SHM 通道配置》: PRELOAD_MD_SHM 是 ext-inst 帧 (instance_id =
                // 行情通道名), payload = struct DzShmPreload (非 JSON); 与 basic 的
                // PRELOAD_EVENT_SHM 布局不同, 不能按 basic 跳过, 也不能当 JSON 解析。
                df.instance_id = std::string(view.ext_inst_id());
                summary += " preload_md inst=" + df.instance_id;
                if (view.ext_inst_payload_size() >= sizeof(DzShmPreload)) {
                    DzShmPreload params{};
                    std::memcpy(&params, view.ext_inst_payload(), sizeof(params));
                    summary += " pages=" + std::to_string(params.pages) +
                               " bytes=" + std::to_string(params.bytes);
                } else {
                    summary += " short_payload";
                }
            } else if (frame_is_basic_struct(df.type)) {
                summary += " basic";
            } else if (frame_has_inst(df.type)) {
                df.instance_id = std::string(view.ext_inst_id());
                summary += " inst=" + df.instance_id;
                const auto size = view.ext_inst_payload_size();
                if (size > 0) {
                    const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                    try {
                        df.payload = nlohmann::json::parse(data, data + size);
                        df.has_payload = true;
                    } catch (const std::exception&) {
                        summary += " undecodable_json";  // 布局错配/非 JSON: 只诊断不中断
                    }
                }
            } else {
                const auto size = view.ext_payload_size();
                if (size > 0) {
                    const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                    try {
                        df.payload = nlohmann::json::parse(data, data + size);
                        df.has_payload = true;
                    } catch (const std::exception&) {
                        summary += " undecodable_json";
                    }
                }
            }
            if (df.has_payload) {
                summary += " payload=" + df.payload.dump();
            }
            frame_summary_.push_back(std::move(summary));
            out.push_back(std::move(df));
        }
        return out;
    }

    /// 写 UI_INPUT 文本命令给策略 (定向帧, instance_id = 裸策略名)
    bool send_ui_input(std::string_view command, std::string_view strategy = "stg_demo") {
        writer_->refresh_subscribers();  // 防过期订阅者快照漏唤醒策略
        const std::string instance(strategy);
        const bool ok = writer_->write_ext_inst_frame(
            DZ_FRAME_UI_INPUT, instance.c_str(), reinterpret_cast<const std::byte*>(command.data()),
            static_cast<uint32_t>(command.size()));
        if (ok) {
            writer_->notify_subscribers();
        }
        return ok;
    }

    /// OUTPUT_UI 已收文本行中是否存在 "prefix 开头且含 needle" 的行 (只断言稳定锚点)
    bool saw_ui_line(std::string_view prefix, std::string_view needle) const {
        for (const auto& line : ui_lines_) {
            if (line.starts_with(prefix) && line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// wait_until 谓词: 先排空事件通道, 再检查 OUTPUT_UI 行
    bool pump_and_saw_ui_line(std::string_view prefix, std::string_view needle) {
        (void)drain_available(*reader_);
        return saw_ui_line(prefix, needle);
    }

    /// 失败诊断: 已收 OUTPUT_UI 全文
    std::string dump_ui_lines() const {
        std::string out = "--- OUTPUT_UI lines ---\n";
        for (const auto& line : ui_lines_) {
            out += "  " + line + "\n";
        }
        return out;
    }

    /// 最近一条以 prefix 开头的 OUTPUT_UI 行 (用于"最新一条"类断言)
    std::optional<std::string> last_ui_line(std::string_view prefix) const {
        for (auto it = ui_lines_.rbegin(); it != ui_lines_.rend(); ++it) {
            if (it->starts_with(prefix)) {
                return *it;
            }
        }
        return std::nullopt;
    }

    /// 最近一条以 prefix 开头的 OUTPUT_UI 行是否含 needle
    bool last_line_contains(std::string_view prefix, std::string_view needle) const {
        const auto line = last_ui_line(prefix);
        return line.has_value() && line->find(needle) != std::string::npos;
    }

    /// 最近一条 [order] action=order 行的 order_id (nullopt = 未找到/解析失败)
    std::optional<int64_t> last_ui_order_id() const {
        for (auto it = ui_lines_.rbegin(); it != ui_lines_.rend(); ++it) {
            if (!it->starts_with("[order] ") || it->find("action=order") == std::string::npos) {
                continue;
            }
            const auto pos = it->find(" order_id=");
            if (pos == std::string::npos) {
                return std::nullopt;
            }
            std::size_t i = pos + 10;  // " order_id="
            int64_t value = 0;
            bool any = false;
            while (i < it->size() && (*it)[i] >= '0' && (*it)[i] <= '9') {
                value = value * 10 + ((*it)[i] - '0');
                ++i;
                any = true;
            }
            return any ? std::optional<int64_t>(value) : std::nullopt;
        }
        return std::nullopt;
    }

    /// 轮询 + 驱动 io_context 直到谓词满足或超时。
    /// trigger 每 500ms 重发一次 (读者从通道尾部起步, 一次性启动帧可能早于
    /// reader 打开而错过; 用快照/SET 类触发帧周期性重发保证至少一次落在打开之后)
    template <typename Pred, typename Trigger>
    bool wait_until(Pred pred, int timeout_ms, Trigger trigger, int interval_ms = 100) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int ticks = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            if (++ticks % 5 == 0) {
                trigger();
            }
            ioc_.run_for(std::chrono::milliseconds(interval_ms));
        }
        return pred();
    }

    /// dzweb /health 探测: 裸 TCP GET, 200 即健康 (不引入 drogon HTTP 客户端依赖)
    bool http_get_health() {
        boost::system::error_code ec;
        boost::asio::ip::tcp::resolver resolver(ioc_);
        const auto results = resolver.resolve("127.0.0.1", std::to_string(webui_port_), ec);
        if (ec || results.empty()) {
            return false;
        }
        boost::asio::ip::tcp::socket sock(ioc_);
        sock.connect(*results.begin(), ec);
        if (ec) {
            return false;
        }
        const std::string req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        boost::asio::write(sock, boost::asio::buffer(req), ec);
        if (ec) {
            return false;
        }
        std::string resp;
        boost::asio::read_until(sock, boost::asio::dynamic_buffer(resp), "\r\n", ec);
        // read_until 服务器先关连接时以 EOF 结束 (ec 非 0 但 resp 可能已有状态行)
        return resp.starts_with("HTTP/1.1 200") || resp.starts_with("HTTP/1.0 200");
    }

    // ── REST / WS / 交易库 断言辅助 (AC1) ─────────────────────────────────

    /// REST 登录并缓存 token (`POST /api/login`)。成功返回 true 且设置 auth_token_。
    /// 断言口径 (AC1-2): 200 且响应含非空 `token`。
    bool login() {
        HttpClient http(ioc_, webui_port_);
        const nlohmann::json body = {{"username", "admin"}, {"password", kSmokeAdminPassword}};
        const HttpResponse resp = http.post("/api/login", body.dump());
        if (!resp.ok()) {
            fprintf(stderr, "--- smoke: login failed | status=%d error=\"%s\" body=%s ---\n",
                    resp.status, resp.error.c_str(), resp.body.c_str());
            return false;
        }
        if (!resp.json_ok || !resp.json.contains("token") || !resp.json["token"].is_string() ||
            resp.json["token"].get<std::string>().empty()) {
            fprintf(stderr, "--- smoke: login response without token | body=%s ---\n",
                    resp.body.c_str());
            return false;
        }
        auth_token_ = resp.json["token"].get<std::string>();
        return true;
    }

    /// 带 Bearer token 的 GET, 断言 200 且 body 是 JSON (失败信息带 path/status/body)。
    std::optional<nlohmann::json> api_get_json(const std::string& path) {
        HttpClient http(ioc_, webui_port_);
        const HttpResponse resp = http.get(path, auth_token_);
        if (!resp.ok()) {
            fprintf(stderr, "--- smoke: GET %s failed | status=%d error=\"%s\" body=%s ---\n",
                    path.c_str(), resp.status, resp.error.c_str(), resp.body.c_str());
            return std::nullopt;
        }
        if (!resp.json_ok) {
            fprintf(stderr, "--- smoke: GET %s non-json body=%s ---\n", path.c_str(),
                    resp.body.c_str());
            return std::nullopt;
        }
        return resp.json;
    }

    /// AC1-1 建库断言: 库文件存在、`PRAGMA user_version == kTdSchemaVersion`、
    /// schema 声明的全部表齐全, 且库路径与 `paths::trade_db()` 单一真相源一致。
    /// 返回空串 = 通过, 否则为失败描述 (便于 ASSERT_TRUE 输出)。
    std::string check_trade_db() {
        // 单一真相源: 断言用的路径就取自 products 的生产函数, 不手拼 db/trade.db
        const std::filesystem::path db_path = paths::trade_db();
        const std::filesystem::path expected = home_ / "db" / "trade.db";
        if (db_path != expected) {
            return "paths::trade_db() = " + db_path.string() + " != " + expected.string();
        }
        std::error_code ec;
        if (!std::filesystem::exists(db_path, ec) || ec) {
            return "trade db missing: " + db_path.string();
        }
        try {
            SQLite::Database db(db_path.string(), SQLite::OPEN_READONLY);
            const int version = db.execAndGet("PRAGMA user_version").getInt();
            if (version != tdstore::kTdSchemaVersion) {
                return "schema version " + std::to_string(version) +
                       " != " + std::to_string(tdstore::kTdSchemaVersion);
            }
            // 表清单取自 tdstore 的单源声明 (schema_catalog.h 的 tables()), 不在此手抄:
            // 将来新增第六张表时 build_schema_ddl() 会自动建, 手抄清单会静默漏掉它。
            std::set<std::string> present;
            SQLite::Statement stmt(
                db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
            while (stmt.executeStep()) {
                present.insert(stmt.getColumn(0).getString());
            }
            for (const auto& table : tdstore::tables()) {
                if (!present.contains(std::string(table.name))) {
                    return "missing table: " + std::string(table.name);
                }
            }
        } catch (const std::exception& e) {
            return std::string("trade db read failed: ") + e.what();
        }
        return {};
    }

    /// AC1-3 列表信封断言: `available:true` + 五键齐全 + (可选) items 为空数组。
    /// @param expect_empty_items 无在线账户时 orders/trades/positions 的 items 必须为 []；
    ///        trading_accounts 会返回注册表占位行, 传 false 跳过该断言。
    std::string check_list_envelope(const std::string& path, bool expect_empty_items) {
        const auto body = api_get_json(path);
        if (!body.has_value()) {
            return path + ": request failed";
        }
        if (!body->is_object()) {
            return path + ": body is not an object";
        }
        if (body->value("available", false) != true) {
            return path + ": available != true | body=" + body->dump();
        }
        for (const char* key : {"trading_day", "online_accounts", "items", "next_cursor"}) {
            if (!body->contains(key)) {
                return path + ": missing envelope key " + key + " | body=" + body->dump();
            }
        }
        if (!(*body)["items"].is_array()) {
            return path + ": items is not an array";
        }
        if (expect_empty_items && !(*body)["items"].empty()) {
            return path + ": items not empty with no online account | body=" + body->dump();
        }
        // 无在线账户 -> 无当前交易日 (null 而不是错误)
        if (expect_empty_items && !(*body)["trading_day"].is_null()) {
            return path + ": trading_day not null with no online account | body=" + body->dump();
        }
        return {};
    }

    // ── 成员 ────────────────────────────────────────────────────────────

    std::filesystem::path home_;
    std::filesystem::path out_dir_;
    std::filesystem::path master_exe_;
    std::optional<std::string> orig_home_;
    int webui_port_ = 0;
    boost::asio::io_context ioc_;
    std::shared_ptr<ChildProcess> master_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    std::vector<std::string> frame_summary_;
    /// 策略 OUTPUT_UI 帧与拆行后的文本 (UI E2E 断言用)
    std::vector<std::string> ui_frames_;
    std::vector<std::string> ui_lines_;
    /// instance_id != stg_demo 的 OUTPUT_UI 帧数 (身份边界回归)
    std::size_t unexpected_ui_instance_count_ = 0;
    /// 契约 account-status: 排空中捕获的 DZ_FRAME_ACCOUNT_STATUS 账户状态帧 payload (帧指针会失效,
    /// 拷出)
    std::vector<DzAccountStatus> account_status_frames_;
    /// REST 登录后的 JWT (login() 填充; REST 读端点与 WS 建连共用)
    std::string auth_token_;
};

/// 核心冒烟路径: 组合验证 4 条断言 (见设计 §3)
TEST_F(SmokeTest, MasterStartsMdFrameRoundTripAndGracefulShutdown) {
    ASSERT_TRUE(open_event_channel()) << "event channel not created by master within 20s";

    auto send_query_snapshot = [&] {
        // 刷新订阅者快照: writer 创建时的快照可能早于 master 注册 md 订阅者
        // (CI 负载下 master 拉起 md 更慢), 过期快照会漏唤醒 md (真实进程靠
        // DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER 帧刷新)
        writer_->refresh_subscribers();
        if (writer_->write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0)) {
            writer_->notify_subscribers();
        }
    };

    // 1. master 成功拉起真实子进程: 快照驱动查询, master 对每个注册进程回一条
    //    RTN_PROCESS_STATUS (shm_manager_frame.cpp report_full_snapshot)
    const auto saw_running = [&](const std::string& name) {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_PROCESS_STATUS && f.payload.value("name", "") == name &&
                f.payload.value("state", "") == "Running") {
                return true;
            }
        }
        return false;
    };
    const bool md_running =
        wait_until([&] { return saw_running("dzmd_ctp"); }, 30000, send_query_snapshot);
    ASSERT_TRUE(md_running) << "dzmd_ctp did not report Running within 30s";
    const bool td_running =
        wait_until([&] { return saw_running("dztd_ctp"); }, 15000, send_query_snapshot);
    ASSERT_TRUE(td_running) << "dztd_ctp did not report Running within 15s";

    // 1c-2. 账户状态回路: td 配置了账户 CTP001 但从不连接 (dztd_ctp.json stub)
    //     -> td 启动读完配置应推 DZ_FRAME_ACCOUNT_STATUS Offline 帧 (契约 account-status 触发场景
    //     1)。 DZ_FRAME_ACCOUNT_STATUS 帧由 drain_available 统一捕获 (account_status_frames_);
    //     启动帧可能 早于 reader 打开被错过, 但 QUERY_FULL_SNAPSHOT 触发 td 快照重推 Offline
    //     (force=true), 故用既有 wait_until + 快照触发轮询窗口兜底 (10s 与本文件
    //     其他轮询窗口一致; 负载下 3s 偏紧)。
    const auto saw_account_offline = [&] {
        (void)drain_available(*reader_);  // 排空并捕获 2018 payload 到 account_status_frames_
        for (const auto& s : account_status_frames_) {
            if (std::string(s.account_id) == "CTP001" && s.state == DZ_ACCOUNT_OFFLINE &&
                std::string(s.gateway_name) == "dztd_ctp" && s.date == 0) {
                return true;
            }
        }
        return false;
    };
    const bool account_offline = wait_until(saw_account_offline, 10000, send_query_snapshot);
    ASSERT_TRUE(account_offline)
        << "dztd_ctp did not publish OFFLINE account status for configured CTP001";

    const bool webui_running =
        wait_until([&] { return saw_running("dzweb"); }, 20000, send_query_snapshot);
    ASSERT_TRUE(webui_running) << "dzweb did not report Running within 20s";

    // 1b. 策略生命周期 + UI 帧 E2E: master 按 strategy 段拉起常驻 stg_demo
    //     (md 源 ready 后才启动) -> 等 Running -> 写 UI_INPUT / 读 OUTPUT_UI 断言 ->
    //     PROCESS_CONTROL 停止 -> Stopped 且无 Crashed。策略不再解析 argv/退出码,
    //     旧 CLI 断言已删除。
    bool saw_strategy_crashed = false;
    bool saw_stop_succeeded_event = false;
    bool saw_stopping = false;
    bool saw_terminal_stopped = false;
    const auto drain_strategy_state = [&](const std::string& wanted) {
        for (auto& f : drain_available(*reader_)) {
            if (f.type != DZ_FRAME_RTN_PROCESS_STATUS ||
                f.payload.value("name", "") != "stg_demo") {
                continue;
            }
            const std::string state = f.payload.value("state", "");
            const std::string event = f.payload.value("event", "");
            if (state == "Crashed") {
                saw_strategy_crashed = true;
            }
            if (state == "Stopping") {
                saw_stopping = true;
            }
            if (event == "StopSucceeded") {
                saw_stop_succeeded_event = true;
            }
            // 契约 process: 终态 Stopped 由进程退出触发, event 缺失 (区别于 StopFailed)
            if (state == "Stopped" && !f.payload.contains("event")) {
                saw_terminal_stopped = true;
            }
            if (state == wanted) {
                return true;
            }
        }
        return false;
    };
    const bool strategy_running =
        wait_until([&] { return drain_strategy_state("Running"); }, 30000, send_query_snapshot);
    ASSERT_TRUE(strategy_running) << "stg_demo did not report Running within 30s";
    ASSERT_FALSE(saw_strategy_crashed) << "stg_demo crashed on master spawn";

    // 1b-1. 就绪横幅: 启动后一次性的 [ready] 输出 (master 托管的常驻策略已进入事件循环)
    ASSERT_TRUE(wait_until([&] { return pump_and_saw_ui_line("[ready] ", "strategy=stg_demo"); },
                           5000, [] {}))
        << "stg_demo did not emit [ready] banner\n"
        << dump_ui_lines();

    // 1b-2. UI 帧回路: Running 可能早于策略事件 reader 打开, 首帧有理论丢失窗口,
    //       故用可重发的 info 命令握手, 收到回执后再发一次性命令 (下同)
    bool info_ok = false;
    for (int i = 0; i < 60 && !info_ok; ++i) {
        ASSERT_TRUE(send_ui_input("info")) << "write UI_INPUT(info) failed";
        info_ok = wait_until([&] { return pump_and_saw_ui_line("[info] ", "strategy=stg_demo"); },
                             500, [] {});
    }
    ASSERT_TRUE(info_ok) << "stg_demo did not answer info within 30s";
    EXPECT_TRUE(saw_ui_line("[info] ", "md_source=dzmd_ctp")) << "info missing md_source";

    // 1b-3. md 命令: 多合约增量订阅 + list 回显
    ASSERT_TRUE(send_ui_input("md add IF2606 IF2609")) << "write UI_INPUT(md add) failed";
    ASSERT_TRUE(wait_until(
        [&] {
            (void)drain_available(*reader_);
            const auto added = last_ui_line("[md] action=add");
            return added.has_value() && added->find("IF2606") != std::string::npos &&
                   added->find("IF2609") != std::string::npos;
        },
        5000, [] {}))
        << "stg_demo did not answer md add with both instruments";
    ASSERT_TRUE(send_ui_input("md list")) << "write UI_INPUT(md list) failed";
    ASSERT_TRUE(wait_until(
        [&] {
            (void)drain_available(*reader_);
            // 只认最新一条 list 行: 之前的 [md] action=add 行也含合约名, 不能作证
            const auto listed = last_ui_line("[md] action=list");
            return listed.has_value() && listed->find("IF2606") != std::string::npos &&
                   listed->find("IF2609") != std::string::npos;
        },
        5000, [] {}))
        << "stg_demo md list missing subscribed instruments";

    // 1b-4. 非法命令 -> [error] 行且进程不崩溃
    ASSERT_TRUE(send_ui_input("bogus")) << "write UI_INPUT(bogus) failed";
    ASSERT_TRUE(
        wait_until([&] { return pump_and_saw_ui_line("[error] ", "cmd=bogus"); }, 5000, [] {}))
        << "stg_demo did not answer invalid command with [error]";

    // 1b-5. 空账户拒单闭环 (UI 驱动): order -> [order] order_id -> td no_session 推
    //       REJECTED 回报 (strategy_id 定向 stg_demo) -> 策略输出 [report] status=REJECTED(拒单)
    // 注意: 本断言只在临时 home 的 TD 库为空 (账户无 seq 水位) 时成立; 真实环境有历史
    // 水位时 no_session 拒单 seq=0 会被 ingest 吞掉 —— 见 #13, 修复后本条应改为
    // 「预置历史水位」场景 (修复前保留空库前提, 不做假绿覆盖)。
    ASSERT_TRUE(send_ui_input("order CTP001 IF2606 100.0 1")) << "write UI_INPUT(order) failed";
    ASSERT_TRUE(
        wait_until([&] { return pump_and_saw_ui_line("[order] ", "action=order"); }, 10000, [] {}))
        << "stg_demo did not output order_id for order command";
    const auto order_id = last_ui_order_id();
    ASSERT_TRUE(order_id.has_value()) << "cannot parse order_id from [order] output";
    ASSERT_TRUE(wait_until(
        [&] { return pump_and_saw_ui_line("[report] ", "status=REJECTED(拒单)"); }, 15000, [] {}))
        << "stg_demo did not output REJECTED order report";

    // 1b-6. cancel / account 有明确输出 (无连接账户: td 对撤单静默丢弃, 只断言请求回执)
    ASSERT_TRUE(send_ui_input("cancel CTP001 " + std::to_string(*order_id)))
        << "write UI_INPUT(cancel) failed";
    ASSERT_TRUE(
        wait_until([&] { return pump_and_saw_ui_line("[order] ", "action=cancel"); }, 5000, [] {}))
        << "stg_demo did not answer cancel";
    // 先排空并清掉查询前已收的输出行 (td 启动期/快照重推的 Offline 状态行不能当作
    // account 查询链路的证据), 再断言查询后的新增行
    (void)drain_available(*reader_);
    ui_lines_.clear();
    ASSERT_TRUE(send_ui_input("account CTP001")) << "write UI_INPUT(account) failed";
    ASSERT_TRUE(
        wait_until([&] { return pump_and_saw_ui_line("[account] ", "action=query"); }, 5000, [] {}))
        << "stg_demo did not answer account";
    ASSERT_TRUE(wait_until([&] { return pump_and_saw_ui_line("[account] ", "action=status"); },
                           5000, [] {}))
        << "stg_demo did not output account status for configured CTP001";
    ASSERT_TRUE(last_ui_line("[account] action=status").has_value()) << dump_ui_lines();
    EXPECT_TRUE(last_line_contains("[account] action=status", "account=CTP001")) << dump_ui_lines();

    // 1b-7. 常驻性: 完成上述交互后策略仍处于 Running (未被交互/异常终止)
    ASSERT_TRUE(
        wait_until([&] { return drain_strategy_state("Running"); }, 5000, send_query_snapshot))
        << "stg_demo not Running before Stop";

    // 1b-8. PROCESS_CONTROL 停止策略: master 发 SHUTDOWN -> 策略 on_stop 输出 [stop] 且
    //       exit 0 -> master 推 Stopped (不能是 Crashed)
    auto send_stop_strategy = [&] {
        platform::ProcessControlReq req{
            .action = platform::ProcessAction::Stop, .target = "stg_demo", .config = std::nullopt};
        platform::write_ext_json(*writer_, DZ_FRAME_REQUEST_PROCESS_CONTROL, req);
        writer_->notify_subscribers();
    };
    send_stop_strategy();
    const bool strategy_stopped =
        wait_until([&] { return drain_strategy_state("Stopped"); }, 15000, send_query_snapshot);
    ASSERT_TRUE(strategy_stopped) << "stg_demo did not reach Stopped within 15s after Stop";
    // 只有"经 master 正常停止"才同时具备: StopSucceeded 事件 + Stopping 中间态 +
    // 无 event 的终态 Stopped (StopFailed 也回 state=Stopped, 不能只断言 Stopped)
    EXPECT_TRUE(saw_stop_succeeded_event) << "no StopSucceeded event in RTN_PROCESS_STATUS";
    EXPECT_TRUE(saw_stopping) << "no Stopping intermediate state in RTN_PROCESS_STATUS";
    EXPECT_TRUE(saw_terminal_stopped) << "no terminal Stopped (event-less) status";
    EXPECT_FALSE(saw_strategy_crashed) << "stg_demo crashed on Stop (must exit 0)";
    (void)drain_available(*reader_);
    EXPECT_TRUE(saw_ui_line("[stop] ", "reason=shutdown"))
        << "stg_demo did not output [stop] on shutdown";
    EXPECT_EQ(unexpected_ui_instance_count_, 0U)
        << "OUTPUT_UI frame with instance_id != stg_demo observed (identity boundary)";

    // 2. 帧回路: SET_LOG_CONFIG 空 patch -> RTN_LOG_CONFIG (契约 log 回环, 配置缺失自愈路径)
    auto send_set_log_config = [&] {
        writer_->refresh_subscribers();  // 同上: 防过期订阅者快照漏唤醒
        platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_LOG_CONFIG, "dzmd_ctp",
                                          nlohmann::json::object());
    };
    const bool got_rtn = wait_until(
        [&] {
            for (auto& f : drain_available(*reader_)) {
                if (f.type == DZ_FRAME_RTN_LOG_CONFIG && f.instance_id == "dzmd_ctp") {
                    return true;
                }
            }
            return false;
        },
        10000, send_set_log_config);
    ASSERT_TRUE(got_rtn) << "no RTN_LOG_CONFIG from dzmd_ctp within 10s";

    // 2b. td 帧回路: SET_LOG_CONFIG -> RTN_LOG_CONFIG (td 配置缺失自愈路径 + 帧回路)
    auto send_set_log_config_td = [&] {
        writer_->refresh_subscribers();  // 防过期订阅者快照漏唤醒
        platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_LOG_CONFIG, "dztd_ctp",
                                          nlohmann::json::object());
    };
    const bool got_rtn_td = wait_until(
        [&] {
            for (auto& f : drain_available(*reader_)) {
                if (f.type == DZ_FRAME_RTN_LOG_CONFIG && f.instance_id == "dztd_ctp") {
                    return true;
                }
            }
            return false;
        },
        10000, send_set_log_config_td);
    ASSERT_TRUE(got_rtn_td) << "no RTN_LOG_CONFIG from dztd_ctp within 10s";

    // 3. QUERY_FULL_SNAPSHOT -> md 按契约上报 (至少 RTN_LOG_CONFIG)
    const bool got_snapshot = wait_until(
        [&] {
            for (auto& f : drain_available(*reader_)) {
                if (f.instance_id == "dzmd_ctp" &&
                    (f.type == DZ_FRAME_RTN_LOG_CONFIG || f.type == DZ_FRAME_RTN_MD_SHM_CONFIG)) {
                    return true;
                }
            }
            return false;
        },
        10000, send_query_snapshot);
    ASSERT_TRUE(got_snapshot) << "no snapshot RTN from dzmd_ctp within 10s";

    // 3b. dzweb HTTP 健康: GET /health -> 200 (drogon 服务器真实对外可用)
    const bool healthy = wait_until([&] { return http_get_health(); }, 30000, [] {});
    ASSERT_TRUE(healthy) << "dzweb /health not 200 within 30s (port=" << webui_port_ << ")";

    // ── 3c. AC1-1 建库与文件路径 (master 启动期地基, ADR 0041) ──────────────
    // master 在任何子进程之前建库/播种 order_id (main.cpp §6.5), 因此走到这里库必然已在;
    // 但仍用 wait_until 轮询: TearDown 之外没有别的时序保证, 保持"所有等待都走同一原语"。
    ASSERT_TRUE(wait_until([&] { return check_trade_db().empty(); }, 10000, [] {}))
        << "trade db assertion failed: " << check_trade_db();

    // ── 3d. AC1-2 REST 登录 ─────────────────────────────────────────────
    ASSERT_TRUE(wait_until([&] { return login(); }, 10000, [] {}))
        << "POST /api/login did not return 200 with token within 10s";
    ASSERT_FALSE(auth_token_.empty());

    // ── 3e. AC1-3 REST 交易数据 (Bearer token) ───────────────────────────
    // 契约 rest §2.9.2 信封: available/trading_day/online_accounts/items/next_cursor。
    // 无在线账户 = 空列表语义 (不是错误): trading_day 为 null、orders/trades/positions
    // 的 items 为空数组。
    // trading_accounts **不套用 items 空断言**: 契约 rest §2.9.4 规定无记录账户仍返回
    // 占位行 (has_data:false), 而注册表里的 CTP001 会给出一行 —— 断言 items 空会恒假。
    for (const char* path : {"/api/td/orders", "/api/td/trades", "/api/td/positions"}) {
        ASSERT_TRUE(check_list_envelope(path, /*expect_empty_items=*/true).empty())
            << path << ": " << check_list_envelope(path, true);
    }
    ASSERT_TRUE(
        check_list_envelope("/api/td/trading_accounts", /*expect_empty_items=*/false).empty())
        << "/api/td/trading_accounts: " << check_list_envelope("/api/td/trading_accounts", false);

    // stats: 计数器口径 (无在线账户 -> 全 0 但仍 ready, 不是 not_ready)
    {
        const auto stats = api_get_json("/api/td/stats");
        ASSERT_TRUE(stats.has_value()) << "GET /api/td/stats failed";
        ASSERT_TRUE(stats->value("available", false))
            << "stats unavailable | body=" << stats->dump();
        ASSERT_TRUE(stats->contains("stats")) << "stats body missing counters: " << stats->dump();
    }

    // ── 3f. AC1-4 WS 建连: snapshot -> td_recent -> data_changed{td} ─────
    // 全链路证据 (REST -> SHM 帧 -> td -> RTN_TD_CONFIG -> dzweb -> WS 广播):
    // 先连 WS, 再经 REST 新增账户, 断言收到 data_changed{scope:"td"}。
    // 这条是唯一的跨进程全链路验证, 不依赖真实 CTP 柜台 (账户配置存在即可, 不需登录柜台)。
    {
        WsClient ws(ioc_, webui_port_);
        ASSERT_TRUE(ws.connect(auth_token_)) << "ws connect failed: " << ws.error();
        std::vector<nlohmann::json> ws_messages;
        // 从 WS 读一条 (短超时); 超时 = 暂无新消息, 返回 false 交给 wait_until 继续轮询。
        const auto pump_ws = [&]() {
            const auto msg = ws.read_message(500);
            if (!msg.has_value()) {
                return false;
            }
            ws_messages.push_back(*msg);
            return true;
        };
        const auto has_type = [&](std::string_view type) {
            for (const auto& m : ws_messages) {
                if (m.value("type", "") == type) {
                    return true;
                }
            }
            return false;
        };
        // 建连后定向发送的两条 (契约 webui-ws §1/§2.5): 先 snapshot, 再 td_recent。
        ASSERT_TRUE(wait_until([&] { return pump_ws() && has_type("snapshot"); }, 15000, [] {}))
            << "WS did not deliver snapshot within 15s | ws=" << ws.error();
        ASSERT_TRUE(wait_until([&] { return pump_ws() && has_type("td_recent"); }, 15000, [] {}))
            << "WS did not deliver td_recent within 15s | ws=" << ws.error();
        // td_recent 形状: {"type":"td_recent","payload":{"events":[...]}}, 无事件时为空数组
        for (const auto& m : ws_messages) {
            if (m.value("type", "") == "td_recent") {
                ASSERT_TRUE(m.contains("payload") && m["payload"].contains("events"))
                    << "td_recent missing payload.events | msg=" << m.dump();
                ASSERT_TRUE(m["payload"]["events"].is_array())
                    << "td_recent.payload.events not array | msg=" << m.dump();
                break;
            }
        }

        // REST 新增一个账户 -> td 回 RTN_TD_CONFIG -> dzweb reconcile -> 广播 data_changed{td}
        {
            HttpClient http(ioc_, webui_port_);
            const nlohmann::json body = {{"gateway", "dztd_ctp"},
                                         {"account",
                                          {{"account_id", "SMOKE002"},
                                           {"enabled", true},
                                           {"risk_control_enabled", false},
                                           {"currency_id", "CNY"},
                                           {"ext",
                                            {{"broker_id", "9999"},
                                             {"user_id", "SMOKE002"},
                                             {"password", ""},
                                             {"product_info", ""},
                                             {"frontends", nlohmann::json::array()},
                                             {"auth_code", ""},
                                             {"app_id", ""},
                                             {"flow_dir", ""}}}}}};
            const HttpResponse resp = http.post("/api/td/accounts", body.dump(), auth_token_);
            ASSERT_TRUE(resp.ok()) << "POST /api/td/accounts failed | status=" << resp.status
                                   << " body=" << resp.body << " error=" << resp.error;
        }
        const auto saw_td_scope = [&] {
            for (const auto& m : ws_messages) {
                if (m.value("type", "") == "data_changed" && m.contains("payload") &&
                    m["payload"].value("scope", "") == "td") {
                    return true;
                }
            }
            return false;
        };
        ASSERT_TRUE(wait_until(
            [&] {
                while (pump_ws()) {
                    if (saw_td_scope()) {
                        return true;
                    }
                }
                return saw_td_scope();
            },
            20000, [] {}))
            << "WS did not broadcast data_changed{scope:\"td\"} within 20s after account add | ws="
            << ws.error();
        ws.close();
    }

    // 4. 定向 SHUTDOWN -> 全部子进程退出, master 推 Stopped (优雅退出路径)
    //    与 master 整体关闭同原语 (逆序逐批定向发送, 广播全员关闭帧不采用):
    //    触发帧 = 定向 SHUTDOWN (dzmd_ctp/dztd_ctp/dzweb) + QUERY_FULL_SNAPSHOT:
    //    快照让 master 重推每个注册进程的当前状态 (已退出进程回 Stopped),
    //    防止某进程的 Stopped 帧被前一断言窗口 drain 消费掉后其等待永不满足
    //    (md/dzweb/td 几乎同时退出, 帧会在同一窗口到达)
    auto send_shutdown_and_snapshot = [&] {
        writer_->refresh_subscribers();  // 防过期订阅者快照漏唤醒
        for (const char* target : {"dzmd_ctp", "dztd_ctp", "dzweb"}) {
            platform::write_ext_inst_raw(*writer_, DZ_FRAME_SHUTDOWN, target);
        }
        if (writer_->write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0)) {
            writer_->notify_subscribers();
        }
    };
    const auto saw_stopped = [&](const std::string& name) {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_PROCESS_STATUS && f.payload.value("name", "") == name &&
                f.payload.value("state", "") == "Stopped") {
                return true;
            }
        }
        return false;
    };
    const bool md_stopped =
        wait_until([&] { return saw_stopped("dzmd_ctp"); }, 15000, send_shutdown_and_snapshot);
    ASSERT_TRUE(md_stopped) << "dzmd_ctp did not stop within 15s after directed SHUTDOWN";
    const bool td_stopped =
        wait_until([&] { return saw_stopped("dztd_ctp"); }, 15000, send_shutdown_and_snapshot);
    ASSERT_TRUE(td_stopped) << "dztd_ctp did not stop within 15s after directed SHUTDOWN";
    const bool webui_stopped =
        wait_until([&] { return saw_stopped("dzweb"); }, 15000, send_shutdown_and_snapshot);
    ASSERT_TRUE(webui_stopped) << "dzweb did not stop within 15s after directed SHUTDOWN";

    // 5. 终止 master (master 常驻无自退出协议, 与 run-dev -Stop 同策略: 强杀)
    //    双轨判定: ChildProcess 状态回调 + 进程级 liveness (async_wait 在外部强杀后不触发)
    const auto master_pid = static_cast<uint32_t>(master_->pid());
    std::fprintf(stderr, "--- smoke: master pid=%u ---\n", master_pid);
    force_kill_pid(master_pid);
    bool master_dead = false;
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (master_->state() == ChildState::Stopped || !process_alive(master_pid)) {
                master_dead = true;
                break;
            }
            ioc_.run_for(100ms);
        }
    }
    EXPECT_TRUE(master_dead) << "master did not exit within 5s after terminate";
}

/// 降级路径 E2E（AC1-5，契约 rest §2.9.8）：交易库不存在时读端点必须回
/// `200 {"available":false,"reason":"not_found"}`，**不是** 5xx、不是空 items。
///
/// 为什么单起一个 fixture：本用例需要一个**没有 `db/trade.db`** 的 home，而主 fixture 的
/// master 启动期必然建库（ADR 0041），两者的前提互斥。这里只起 dzweb（不起 master）——
/// dzweb 找不到事件通道时进 api-only 模式（`main.cpp` 的 shm init failed 分支），REST 照常
/// 服务，正是"库缺失但服务在跑"的最小复现。
///
/// 注意：不经过 master 也就没有 NamedSemaphore 争用，本用例不参与 `DZ_SMOKE_STRICT`
/// 的信号量冲突判定（它本来就不会与其他栈互扰），但仍然 RUN_SERIAL（同为进程级测试）。
class SmokeDegradedTest : public ::testing::Test {
protected:
    void SetUp() override {
        home_ = std::filesystem::temp_directory_path() /
                ("dz_smoke_degraded_" + std::to_string(this_process::pid()));
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        std::filesystem::create_directories(home_ / "configs", ec);
        ASSERT_FALSE(ec) << "create temp home failed: " << ec.message();
        orig_home_ = env::get("DZTRADER_HOME");
        env::set("DZTRADER_HOME", home_.string());

        out_dir_ = this_process::exe_dir().parent_path();
        port_ = 20000 + ((this_process::pid() + 7) % 30000);

        // 刻意**不建 db/ 目录**：读端点必须走 not_found 降级。
        // admin 口令同样必须非默认（否则 403 password_change_required 挡住 REST），
        // 理由见 SmokeTest 的 kSmokeAdminPassword 注释。
        {
            std::ofstream ofs(home_ / "configs" / "webui.json");
            ofs << "{\"server\":{\"listen\":\"127.0.0.1\",\"port\":" << port_ << "},"
                << "\"log\":{\"level\":\"warn\",\"flush_on\":\"warn\"},"
                << "\"auth\":{\"jwt_secret\":\"smoke_degraded_secret\",\"token_ttl_sec\":3600},"
                << "\"admin\":{\"username\":\"admin\",\"password\":\"" << kSmokeAdminPassword
                << "\"},"
                << "\"notify\":{\"cache_size\":10}}";
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write webui.json failed";
        }

        const auto webui_exe = out_dir_ /
                               "dzweb"
#ifdef _WIN32
                               ".exe"
#endif
            ;
        ProcessEntry entry;
        entry.name = "dzweb";
        entry.category = Category::WebUI;  // 类别不影响 ChildProcess 行为 (仅日志/命名)
        entry.exe = webui_exe;
        webui_ = ChildProcess::create(ioc_, std::move(entry));
        boost::system::error_code spawn_ec;
        ASSERT_TRUE(webui_->start(spawn_ec)) << "dzweb spawn failed | exe=" << webui_exe.string()
                                             << " error=\"" << spawn_ec.message() << "\"";
    }

    void TearDown() override {
        if (webui_ && webui_->pid() != 0 && process_alive(static_cast<uint32_t>(webui_->pid()))) {
            force_kill_pid(static_cast<uint32_t>(webui_->pid()));
            ioc_.run_for(1s);
        }
        webui_.reset();
        if (::testing::Test::HasFailure()) {
            dump_log_tail(home_ / "logs", "dzweb_", 40);
        }
        if (orig_home_) {
            env::set("DZTRADER_HOME", *orig_home_);
        } else {
            env::unset("DZTRADER_HOME");
        }
        std::error_code ec;
        if (env::get("DZ_SMOKE_KEEP_HOME") != "1") {
            std::filesystem::remove_all(home_, ec);
        }
    }

    /// 轮询 /health 直到 dzweb 起来 (drogon 真实对外可用)
    bool wait_health(int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            HttpClient http(ioc_, port_);
            if (http.get("/health").ok()) {
                return true;
            }
            ioc_.run_for(100ms);
        }
        return false;
    }

    std::filesystem::path home_;
    std::filesystem::path out_dir_;
    std::optional<std::string> orig_home_;
    int port_ = 0;
    boost::asio::io_context ioc_;
    std::shared_ptr<ChildProcess> webui_;
};

TEST_F(SmokeDegradedTest, TradeReadEndpointsDegradeToAvailableFalse) {
    ASSERT_TRUE(wait_health(30000)) << "dzweb /health not 200 within 30s (port=" << port_ << ")";
    // 库不存在的前置条件必须成立, 否则本用例测的就不是降级路径
    std::error_code ec;
    ASSERT_FALSE(std::filesystem::exists(home_ / "db" / "trade.db", ec))
        << "precondition violated: trade.db unexpectedly present";

    HttpClient http(ioc_, port_);
    const nlohmann::json login_body = {{"username", "admin"}, {"password", kSmokeAdminPassword}};
    HttpResponse login = http.post("/api/login", login_body.dump());
    ASSERT_TRUE(login.ok()) << "login failed | status=" << login.status << " body=" << login.body;
    ASSERT_TRUE(login.json_ok && login.json.contains("token"));
    const std::string token = login.json["token"].get<std::string>();

    // 契约 rest §2.9.8: 五个读端点统一降级体, 只有 available + reason 两键。
    // stats 例外（§2.9.5: 计数器未就绪 = not_ready），因此不在此列表。
    for (const char* path :
         {"/api/td/orders", "/api/td/trades", "/api/td/positions", "/api/td/trading_accounts"}) {
        const HttpResponse resp = http.get(path, token);
        ASSERT_TRUE(resp.ok()) << path
                               << ": expected HTTP 200 degraded body, got status=" << resp.status
                               << " body=" << resp.body << " error=" << resp.error;
        ASSERT_TRUE(resp.json_ok) << path << ": non-json body=" << resp.body;
        const auto& body = resp.json;
        // 只有 available 与 reason 两个键 (契约 §2.9.8: 不含 items/trading_day/...)
        ASSERT_EQ(body.size(), 2U)
            << path << ": degraded body must have exactly 2 keys | body=" << body.dump();
        ASSERT_FALSE(body.value("available", true))
            << path << ": available should be false | body=" << body.dump();
        ASSERT_TRUE(body.contains("reason")) << path << ": missing reason | body=" << body.dump();
        // 库文件不存在 -> not_found (不是 schema_mismatch/open_failed)
        ASSERT_EQ(body["reason"].get<std::string>(), "not_found")
            << path << ": unexpected reason | body=" << body.dump();
        ASSERT_FALSE(body.contains("items")) << path << ": degraded body must not carry items";
        ASSERT_FALSE(body.contains("next_cursor"))
            << path << ": degraded body must not carry next_cursor";
    }
}

}  // namespace
}  // namespace dztrader
