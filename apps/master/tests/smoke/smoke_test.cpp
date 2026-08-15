/**
 * @file smoke_test.cpp
 * @brief 集成冒烟测试: master + dzmd_ctp + dztd_ctp + dzweb 真实进程组合验证。
 *
 * 流程: 临时 DZTRADER_HOME -> spawn 真实 dztraderd.exe -> master 拉起行情/交易网关 + dzweb + 策略 stg_demo
 *       -> 事件通道帧回路断言 + dzweb /health HTTP 探测 + 空账户拒单闭环 (stg_demo order 退出码 3)
 *       -> REQUEST_SHUTDOWN_ALL -> 终止 master -> 清理。
 *
 * 注意:
 * - Windows NamedSemaphore 为机器全局命名空间 ("dz.sem.<name>"), 与并行测试/dev 栈互扰,
 *   故启动前检测 dz.sem.dzevent 占用: 默认 GTEST_SKIP, DZ_SMOKE_STRICT=1 时 FAIL (CI 使用)。
 * - ctest 中本测试 RUN_SERIAL (见 apps/master/CMakeLists.txt)。
 */

#include "child_process.h"
#include "config.h"

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

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
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

bool smoke_strict() {
    return env::get("DZ_SMOKE_STRICT") == "1";
}

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
        case DZ_FRAME_REQUEST_SHUTDOWN_ALL:
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
/// 策略/交易链路会在事件通道产生此类帧 (TD_ORDER_REQ=2100 / TD_ORDER_RPT=2000),
/// 混在 ext 帧里, drain 时只记摘要跳过 payload 解析。
bool frame_is_basic_struct(DzFrameType t) {
    switch (t) {
        case DZ_FRAME_TD_ORDER_REQ:
        case DZ_FRAME_TD_ORDER_CANCEL_REQ:
        case DZ_FRAME_TD_ORDER_RPT:
        case DZ_FRAME_TD_TRADE_RPT:
        case DZ_FRAME_TD_POSITION_INFO:
        case DZ_FRAME_TD_TRADING_ACCOUNT:
        case DZ_FRAME_RTN_MD_TICK:
            return true;
        default:
            return false;
    }
}

struct DecodedFrame {
    DzFrameType type{};
    std::string instance_id;   // 空 = 无 instance_id 帧
    nlohmann::json payload;    // 空 payload 帧 -> null
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

        // 最小 dztraderd.json: 注册 dzmd_ctp + dztd_ctp + dzweb + 策略 stg_demo(info)
        // (网关段模板与 ProcessControlFrameTest 一致; td/webui 段见 config.cpp parse_master_json)
        // 不写 configs/dzmd_ctp.json: 顺带回归"配置缺失自愈"路径 (历史崩溃 bug, td 侧
        // TdConfig::load 对缺失文件走 exists 短路, 冒烟由 md 覆盖共享模式即可)
        // 策略: exe 名即策略ID (stg_demo), 必须与 strategy[].name 一致; info 模式秒退, 断言生命周期
        {
            const auto cli_exe = out_dir_ / "stg_demo"
#ifdef _WIN32
                ".exe"
#endif
                ;
            nlohmann::json cfg = {
                {"log", nlohmann::json::object()},
                {"master", nlohmann::json::object()},
                {"shm", {{"meta_file_size", 1048576},
                         {"event", {{"page_size_mb", 1},
                                    {"preload_points", nlohmann::json::object()},
                                    {"check_interval_min", 5},
                                    {"check_pages", 1},
                                    {"check_bytes", 0}}}}},
                {"md", {{"dzmd_ctp",
                         {{"args", nlohmann::json::array()},
                          {"env", nlohmann::json::object()},
                          {"restart", {{"enabled", false},
                                       {"max_attempts", 0},
                                       {"backoff_sec", 5}}},
                          {"display_name", "CTP行情"}}}}},
                {"td", {{"dztd_ctp",
                         {{"args", nlohmann::json::array()},
                          {"env", nlohmann::json::object()},
                          {"restart", {{"enabled", false},
                                       {"max_attempts", 0},
                                       {"backoff_sec", 5}}},
                          {"display_name", "CTP交易"}}}}},
                {"webui", {{"args", nlohmann::json::array()},
                           {"restart", {{"enabled", false},
                                        {"max_attempts", 0},
                                        {"backoff_sec", 5}}}}},
                {"strategy", nlohmann::json::array(
                                 {{{"name", "stg_demo"},
                                   {"exe", cli_exe.string()},
                                   {"args", nlohmann::json::array({"info"})}}})},
            };
            std::ofstream ofs(home_ / "configs" / "dztraderd.json");
            ofs << cfg.dump();
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write dztraderd.json failed";
        }

        // dzweb 配置: 随机高位端口 (避免与 dev 栈/其他实例冲突; 机器全局端口空间)
        webui_port_ = 20000 + (this_process::pid() % 30000);
        {
            std::ofstream ofs(home_ / "configs" / "webui.json");
            ofs << "{\"server\":{\"listen\":\"0.0.0.0\",\"port\":" << webui_port_ << "},"
                << "\"log\":{\"level\":\"info\",\"flush_on\":\"info\"},"
                << "\"auth\":{\"jwt_secret\":\"smoke_test_secret\",\"token_ttl_sec\":3600},"
                << "\"admin\":{\"username\":\"admin\",\"password\":\"88888888\"},"
                << "\"notify\":{\"cache_size\":100}}";
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write webui.json failed";
        }

        // dztd_ctp 配置: 空账户闭环 — 配置账户 CTP001 但从不连接 (无 auto_login 段,
        // 无 --recover), 会话懒创建于 connect 不会发生; 下单走 no_session 拒单回报。
        {
            nlohmann::json td_cfg = {
                {"td",
                 {{"accounts",
                   nlohmann::json::array(
                       {{{"account_id", "CTP001"},
                         {"broker", {{"name", "smoke-empty-account"},
                                     {"broker_id", "9999"},
                                     {"user_id", "SMOKE001"},
                                     {"password", ""},
                                     {"product_info", ""},
                                     {"frontends", nlohmann::json::array()}}},
                         {"enabled", true}}})}}},
            };
            std::ofstream ofs(home_ / "configs" / "dztd_ctp.json");
            ofs << td_cfg.dump();
            ASSERT_TRUE(static_cast<bool>(ofs)) << "write dztd_ctp.json failed";
        }

        // master 可执行文件: 测试 exe 在 <out>/tests/, master 在 <out>/
        master_exe_ = out_dir_ / "dztraderd"
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
            << "master spawn failed | exe=" << master_exe_.string()
            << " error=\"" << spawn_ec.message() << "\"";
    }

    void TearDown() override {
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

    /// 打开事件通道 writer/reader (master 启动后创建通道, 轮询等待)
    bool open_event_channel() {
        for (int i = 0; i < 100; ++i) {
            try {
                auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), paths::shm());
                writer_ = std::make_unique<shm::MultiWriter>(shm::MultiWriter::create(
                    std::make_shared<shm::ChannelMeta>(std::move(meta)), "smoke_writer"));
                reader_ = std::make_unique<shm::Reader>(shm::Reader::create(
                    shm::channel_name("dzevent"), paths::shm(), "smoke_reader"));
                return true;
            } catch (const std::exception&) {
                ioc_.run_for(200ms);
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
            if (frame_is_basic_struct(df.type)) {
                summary += " basic";
            } else if (frame_has_inst(df.type)) {
                df.instance_id = std::string(view.ext_inst_id());
                summary += " inst=" + df.instance_id;
                const auto size = view.ext_inst_payload_size();
                if (size > 0) {
                    const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                    df.payload = nlohmann::json::parse(data, data + size);
                    df.has_payload = true;
                }
            } else {
                const auto size = view.ext_payload_size();
                if (size > 0) {
                    const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                    df.payload = nlohmann::json::parse(data, data + size);
                    df.has_payload = true;
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

    /// 轮询 + 驱动 io_context 直到谓词满足或超时。
    /// trigger 每 500ms 重发一次 (读者从通道尾部起步, 一次性启动帧可能早于
    /// reader 打开而错过; 用快照/SET 类触发帧周期性重发保证至少一次落在打开之后)
    template <typename Pred, typename Trigger>
    bool wait_until(Pred pred, int timeout_ms, Trigger trigger, int interval_ms = 100) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
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
};

/// 核心冒烟路径: 组合验证 4 条断言 (见设计 §3)
TEST_F(SmokeTest, MasterStartsMdFrameRoundTripAndGracefulShutdown) {
    ASSERT_TRUE(open_event_channel()) << "event channel not created by master within 20s";

    auto send_query_snapshot = [&] {
        // 刷新订阅者快照: writer 创建时的快照可能早于 master 注册 md 订阅者
        // (CI 负载下 master 拉起 md 更慢), 过期快照会漏唤醒 md (真实进程靠 21 帧刷新)
        writer_->refresh_subscribers();
        if (writer_->write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0)) {
            writer_->notify_subscribers();
        }
    };

    // 1. master 成功拉起真实子进程: 快照驱动查询, master 对每个注册进程回一条
    //    RTN_PROCESS_STATUS (shm_manager_frame.cpp report_full_snapshot)
    const auto saw_running = [&](const std::string& name) {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_PROCESS_STATUS &&
                f.payload.value("name", "") == name && f.payload.value("state", "") == "Running") {
                return true;
            }
        }
        return false;
    };
    const bool md_running = wait_until([&] { return saw_running("dzmd_ctp"); }, 30000,
                                       send_query_snapshot);
    ASSERT_TRUE(md_running) << "dzmd_ctp did not report Running within 30s";
    const bool td_running = wait_until([&] { return saw_running("dztd_ctp"); }, 15000,
                                       send_query_snapshot);
    ASSERT_TRUE(td_running) << "dztd_ctp did not report Running within 15s";
    const bool webui_running = wait_until([&] { return saw_running("dzweb"); }, 20000,
                                          send_query_snapshot);
    ASSERT_TRUE(webui_running) << "dzweb did not report Running within 20s";

    // 1b. 策略生命周期: master 按 strategy 段拉起 stg_demo(info) -> 策略秒退 0 -> master 推
    //     Stopped。live 帧一次性可能早于 reader 打开, 但快照对已退出进程重推 Stopped
    //     (find_child 为空), 故快照驱动断言必然满足; 启动失败会推 Crashed (live 帧落在
    //     reader 打开之后可捕获), 同时断言无 Crashed 以证明策略真实拉起并干净退出。
    bool saw_cli_crashed = false;
    const auto saw_cli_exited_clean = [&] {
        for (auto& f : drain_available(*reader_)) {
            if (f.type != DZ_FRAME_RTN_PROCESS_STATUS ||
                f.payload.value("name", "") != "stg_demo") {
                continue;
            }
            const std::string state = f.payload.value("state", "");
            if (state == "Crashed") {
                saw_cli_crashed = true;
            } else if (state == "Stopped") {
                return true;
            }
        }
        return false;
    };
    const bool cli_stopped = wait_until(saw_cli_exited_clean, 30000, send_query_snapshot);
    ASSERT_TRUE(cli_stopped) << "stg_demo (info) did not report Stopped within 30s";
    ASSERT_FALSE(saw_cli_crashed) << "stg_demo (info) crashed on master spawn";

    // 1c. 空账户拒单闭环: 手动拉起 stg_demo order 模式, td 未配置账户 -> 会话懒创建不存在
    //     -> no_session 分支推 REJECTED 回报 -> 策略收到回报按设计退出 3 (机器可断言)。
    //     无需 master 注册订阅者: 回报帧已落在通道内, 策略每 500ms 无条件排空一次
    //     (唤醒缺失兜底), 不依赖被 notify 唤醒。
    const auto cli_exe_path = out_dir_ / "stg_demo"
#ifdef _WIN32
        ".exe"
#endif
        ;
    ProcessEntry cli_entry;
    cli_entry.name = "stg_demo";
    cli_entry.category = Category::Strategy;
    cli_entry.exe = cli_exe_path;
    cli_entry.args = {"order", "CTP001", "IF2606", "100.0", "1"};
    auto cli = ChildProcess::create(ioc_, std::move(cli_entry));
    boost::system::error_code cli_spawn_ec;
    ASSERT_TRUE(cli->start(cli_spawn_ec))
        << "stg_demo spawn failed | exe=" << cli_exe_path.string()
        << " error=\"" << cli_spawn_ec.message() << "\"";
    int cli_exit = -1;
    bool cli_exited = false;
    cli->async_wait([&](boost::system::error_code, int code) {
        cli_exit = code;
        cli_exited = true;
    });
    const auto cli_deadline = std::chrono::steady_clock::now() + 15s;
    while (!cli_exited && std::chrono::steady_clock::now() < cli_deadline) {
        ioc_.run_for(100ms);
    }
    EXPECT_TRUE(cli_exited) << "stg_demo order mode did not exit within 15s";
    EXPECT_EQ(cli_exit, 3) << "stg_demo order mode should exit 3 (REJECTED) in empty-account setup";

    // 2. 帧回路: SET_LOG_CONFIG 空 patch -> RTN_LOG_CONFIG (契约 01 回环, 配置缺失自愈路径)
    auto send_set_log_config = [&] {
        writer_->refresh_subscribers();  // 同上: 防过期订阅者快照漏唤醒
        platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_LOG_CONFIG, "dzmd_ctp",
                                          nlohmann::json::object());
    };
    const bool got_rtn = wait_until([&] {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_LOG_CONFIG && f.instance_id == "dzmd_ctp") {
                return true;
            }
        }
        return false;
    }, 10000, send_set_log_config);
    ASSERT_TRUE(got_rtn) << "no RTN_LOG_CONFIG from dzmd_ctp within 10s";

    // 2b. td 帧回路: SET_LOG_CONFIG -> RTN_LOG_CONFIG (td 配置缺失自愈路径 + 帧回路)
    auto send_set_log_config_td = [&] {
        writer_->refresh_subscribers();  // 防过期订阅者快照漏唤醒
        platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_LOG_CONFIG, "dztd_ctp",
                                          nlohmann::json::object());
    };
    const bool got_rtn_td = wait_until([&] {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_LOG_CONFIG && f.instance_id == "dztd_ctp") {
                return true;
            }
        }
        return false;
    }, 10000, send_set_log_config_td);
    ASSERT_TRUE(got_rtn_td) << "no RTN_LOG_CONFIG from dztd_ctp within 10s";

    // 3. QUERY_FULL_SNAPSHOT -> md 按契约上报 (至少 RTN_LOG_CONFIG)
    const bool got_snapshot = wait_until([&] {
        for (auto& f : drain_available(*reader_)) {
            if (f.instance_id == "dzmd_ctp" &&
                (f.type == DZ_FRAME_RTN_LOG_CONFIG || f.type == DZ_FRAME_RTN_MD_SHM_CONFIG)) {
                return true;
            }
        }
        return false;
    }, 10000, send_query_snapshot);
    ASSERT_TRUE(got_snapshot) << "no snapshot RTN from dzmd_ctp within 10s";

    // 3b. dzweb HTTP 健康: GET /health -> 200 (drogon 服务器真实对外可用)
    const bool healthy = wait_until([&] { return http_get_health(); }, 30000, [] {});
    ASSERT_TRUE(healthy) << "dzweb /health not 200 within 30s (port=" << webui_port_ << ")";

    // 4. REQUEST_SHUTDOWN_ALL -> 全部子进程退出, master 推 Stopped (优雅退出路径)
    //    触发帧 = SHUTDOWN_ALL + QUERY_FULL_SNAPSHOT: 快照让 master 重推每个注册进程的
    //    当前状态 (已退出进程回 Stopped), 防止某进程的 Stopped 帧被前一断言窗口
    //    drain 消费掉后其等待永不满足 (md/dzweb/td 几乎同时退出, 帧会在同一窗口到达)
    auto send_shutdown_and_snapshot = [&] {
        writer_->refresh_subscribers();  // 防过期订阅者快照漏唤醒
        if (writer_->write_ext_frame(DZ_FRAME_REQUEST_SHUTDOWN_ALL, nullptr, 0)) {
            writer_->notify_subscribers();
        }
        if (writer_->write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0)) {
            writer_->notify_subscribers();
        }
    };
    const auto saw_stopped = [&](const std::string& name) {
        for (auto& f : drain_available(*reader_)) {
            if (f.type == DZ_FRAME_RTN_PROCESS_STATUS &&
                f.payload.value("name", "") == name && f.payload.value("state", "") == "Stopped") {
                return true;
            }
        }
        return false;
    };
    const bool md_stopped = wait_until([&] { return saw_stopped("dzmd_ctp"); }, 15000,
                                       send_shutdown_and_snapshot);
    ASSERT_TRUE(md_stopped) << "dzmd_ctp did not stop within 15s after REQUEST_SHUTDOWN_ALL";
    const bool td_stopped = wait_until([&] { return saw_stopped("dztd_ctp"); }, 15000,
                                       send_shutdown_and_snapshot);
    ASSERT_TRUE(td_stopped) << "dztd_ctp did not stop within 15s after REQUEST_SHUTDOWN_ALL";
    const bool webui_stopped = wait_until([&] { return saw_stopped("dzweb"); }, 15000,
                                          send_shutdown_and_snapshot);
    ASSERT_TRUE(webui_stopped) << "dzweb did not stop within 15s after REQUEST_SHUTDOWN_ALL";

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

}  // namespace
}  // namespace dztrader
