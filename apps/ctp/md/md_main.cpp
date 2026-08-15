
#include <dztrader/core/encoding.h>
#include <dztrader/core/this_process.h>
#include <dztrader/core/path.h>
#include <dztrader/core/json_section.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/named_semaphore.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>

#include "md/md_api.h"

using namespace dztrader;
using namespace ctp;

namespace {
// 信号触发的优雅退出标志, 主循环检查后 break
std::atomic<bool> g_shutdown_requested{false};
// event_sem 指针, 信号处理器通过它唤醒主循环的 event_queue_->wait()
// atomic 保证信号处理器与主线程的读写无 UB (仅退出路径, 不在热路径)
std::atomic<dztrader::shm::NamedSemaphore*> g_event_sem{nullptr};

// 信号处理: SIGTERM/SIGINT 触发优雅退出
// 仅调用异步信号安全操作: atomic store + notify (sem_post 封装)
extern "C" void signal_handler(int sig) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
    if (auto* sem = g_event_sem.load(std::memory_order_relaxed)) {
        sem->notify();  // 唤醒主循环的 event_queue_->wait()
    }
    std::signal(sig, SIG_DFL);  // 恢复默认处理, 二次 Ctrl+C 直接终止
}
}  // namespace

int main(int argc, char* argv[]) {
    // 解析命令行参数
    bool recover = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--recover") {
            recover = true;
        }
    }
    // 网关名 = 可执行文件名 (不含扩展名), 用于 logger 名 / 配置文件名 / SHM 通道命名。
    // 重命名 exe (如 dzmd_ctp_2) 即自动作为新网关, 无需命令行参数。
    const auto& exe_stem = dztrader::this_process::exe_stem();
    const std::string name = exe_stem;

    // flow 目录: $DZTRADER_HOME/flow/<name>/, 隔离每个网关的 CTP 流文件
    std::filesystem::path flow_dir = paths::home() / "flow" / name;
    std::error_code mk_ec;
    std::filesystem::create_directories(flow_dir, mk_ec);
    if (mk_ec) {
        SPDLOG_ERROR("create flow dir failed | path={} error=\"{}\"",
                     flow_dir.string(), dztrader::to_utf8_from_system(mk_ec.message()));
        return 1;
    }

    // 配置文件路径: $DZTRADER_HOME/configs/<name>.json
    auto cfg_path = paths::configs() / (name + ".json");

    // 1. 加载 log section 为 JSON (不依赖 struct LogConfig, 启动期只需 level/flush_on 字符串)
    //    log_config_ 在 MdApi 构造体内从 cfg_path 的 "log" section 自行加载 (失败用默认值自愈)
    nlohmann::json log_json;
    try {
        log_json = dztrader::core::load_json_section<nlohmann::json>(cfg_path, "log");
    } catch (const std::exception& e) {
        SPDLOG_ERROR("log config load failed | error=\"{}\"", e.what());
        return 1;
    }
    // 配置文件缺失/无 log 段时 load_json_section 返回 null, 直接 value() 会抛异常崩溃;
    // 自愈: 缺配置用默认值 + 警告日志; 坏配置(解析失败)仍按现状报错退出
    std::string log_level = "debug";
    std::string log_flush_on = "info";
    if (log_json.is_object()) {
        log_level = log_json.value("level", log_level);
        log_flush_on = log_json.value("flush_on", log_flush_on);
    } else {
        SPDLOG_WARN("log config missing, using defaults | path={}", cfg_path.string());
    }
    // 统一用 LogConfig::parse_level 校验 (与 master main.cpp 一致), 非法回落 info
    auto lvl = dztrader::platform::LogConfig::parse_level(log_level)
                   .value_or(spdlog::level::info);
    auto flush_lvl = dztrader::platform::LogConfig::parse_level(log_flush_on)
                         .value_or(spdlog::level::info);

    dztrader::log::LoggerSetup setup{
        .logger_name = name,
        .log_dir = paths::logs(),
        .level = lvl,
        .flush_level = flush_lvl,
        .max_files = 30,  // 7x24 长期运行每天一个日志文件, 保留 30 天, 超出自动删除最旧文件
    };
    dztrader::log::set_default_logger(setup);

    SPDLOG_INFO("start | name={} flow={}", name, flow_dir.string());

    // 2. md config 已在 MdApi 构造函数体内由 ctp_md_config_.load() 加载 (md_api.cpp), 无需外部加载

    // 3. 打开 event channel (master 已创建, open_only)
    std::shared_ptr<shm::ChannelMeta> event_meta;
    try {
        auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), paths::shm());
        event_meta = std::make_shared<shm::ChannelMeta>(std::move(meta));
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("open event channel failed | error=\"{}\"", e.what());
        return 1;
    }

    // 4. 构造共用信号量 (名字 = reader_name = name, 无 pid 后缀)
    // master 的 make_subscriber_name 用 entry.name (= exe_stem) 构造订阅者名,
    // dzmd_ctp 的 reader_name 必须与之一致, master 的 notify_subscribers 才能 notify 到
    // 该信号量同时被 SPI 线程的 push 通知, 主线程 wait 它即可响应两种事件源
    std::string reader_name = name;
    auto event_sem = std::make_shared<shm::NamedSemaphore>(reader_name);

    // 注册信号处理: SIGTERM/SIGINT 触发优雅退出
    g_event_sem.store(event_sem.get(), std::memory_order_relaxed);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    // 5. 构造 event_queue (SPI 线程 -> 主线程), 共用 event_sem
    auto event_queue = std::make_shared<SpscQueue>(event_sem.get(), 4096);

    // 6. 构造 MdApi (内部创建 event reader/writer + md SingleWriter, 构造时加载 log_config_ 与 md_shm_config_)
    std::unique_ptr<MdApi> api;
    try {
        api = std::make_unique<MdApi>(name, paths::shm(), event_meta,
                                      event_queue, flow_dir, reader_name, cfg_path);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("md api init failed | error=\"{}\"", e.what());
        return 1;
    }
    api->set_recover(recover);
    api->set_external_shutdown_flag(&g_shutdown_requested);

    // 7. 主循环 (异常不退出: catch + 日志 + sleep + 继续)
    while (true) {
        try {
            api->run();
            // run 返回后: 若是信号触发的退出则正常结束, 否则 (收到 DZ_FRAME_REQUEST_SHUTDOWN) 也结束
            if (g_shutdown_requested.load(std::memory_order_relaxed)) {
                SPDLOG_INFO("shutdown requested by signal");
            }
            break;
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("run loop exception, continuing | error=\"{}\"", e.what());
            if (g_shutdown_requested.load(std::memory_order_relaxed)) {
                break;  // 信号期间异常, 直接退出
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    SPDLOG_INFO("stop");
    g_event_sem.store(nullptr, std::memory_order_relaxed);  // 防止信号处理器在析构后访问悬垂指针
    spdlog::shutdown();
    return 0;
}
