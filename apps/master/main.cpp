/**
 * @file main.cpp
 * @brief Master 进程入口：信号处理与进程生命周期管理。
 */

#include "orphan_guard.h"
#include "process_registry.h"
#include "process_supervisor.h"
#include "shm_manager.h"
#include "spdlog/common.h"

#include <dztrader/core/json_section.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/log_config.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <functional>

/// CLI 子命令：清理僵尸子进程后立即退出。
/// 不启动 io_context、不启动子进程。复用 OrphanGuard::startup() + cleanup()。
/// 主进程正在运行时 file_lock 获取失败，CLI 报错退出。
static int run_cleanup_orphans() {
    // 最小日志初始化（不读 dztraderd.json）
    dztrader::log::LoggerSetup log_cfg;
    log_cfg.logger_name = dztrader::this_process::exe_stem();
    log_cfg.log_dir = dztrader::paths::logs();
    log_cfg.level = spdlog::level::info;
    log_cfg.flush_level = spdlog::level::warn;
    dztrader::log::set_default_logger(log_cfg);

    SPDLOG_INFO("cleanup-orphans started | pid={}", dztrader::this_process::pid());

    dztrader::master::OrphanGuard guard;
    try {
        // startup() 内部：
        // 1. 获取 file_lock（失败=主进程在运行，抛 DZ_EC_MASTER_ALREADY_RUNNING）
        // 2. 打开 children.db
        // 3. 读 children 表，ext::exe + exe_path 比对，terminate 僵尸
        // 4. 清空 children 表
        guard.startup();
        // cleanup() 内部：
        // 1. 释放 file_lock
        // 2. 关闭 DB
        // 3. 删除锁文件
        guard.cleanup();
    } catch (const dztrader::Exception& e) {
        SPDLOG_CRITICAL("cleanup-orphans failed | error=\"{}\"", e.what());
        fprintf(stderr, "cleanup-orphans failed: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("cleanup-orphans failed | error=\"{}\"", e.what());
        fprintf(stderr, "cleanup-orphans failed: %s\n", e.what());
        return 1;
    }

    SPDLOG_INFO("cleanup-orphans done | result=ok");
    return 0;
}

int main(int argc, char* argv[]) {
    // CLI 子命令：单独清理僵尸进程（不启动主进程）
    if (argc > 1 && std::string(argv[1]) == "--cleanup-orphans") {
        return run_cleanup_orphans();
    }

    // 1. 确定配置文件路径
    //    默认: $DZTRADER_HOME/configs/dztraderd.json
    //    覆盖: argv[1]
    std::filesystem::path config_path;
    if (argc > 1) {
        config_path = argv[1];
    } else {
        config_path = dztrader::paths::configs() / "dztraderd.json";
    }

    // 配置不存在则自动生成(日志尚未初始化,用 stderr)
    if (!std::filesystem::exists(config_path)) {
        fprintf(stderr, "配置文件不存在,自动生成默认配置: %s\n", config_path.string().c_str());
        try {
            dztrader::master::generate_default_config(config_path);
        } catch (const std::exception& e) {
            fprintf(stderr, "生成配置失败: %s\n", e.what());
            return 1;
        }
    }

    // 2. 先解析配置以获取日志级别
    dztrader::master::Config cfg;
    try {
        cfg = dztrader::master::parse_master_json(config_path);
    } catch (const std::exception& e) {
        // 日志尚未初始化，使用 spdlog 默认 logger 输出到 stderr
        SPDLOG_ERROR("config parse failed | error=\"{}\"", e.what());
        return 1;
    }

    // 3. 加载 log section 为 JSON (不依赖 struct LogConfig, 启动期只需 level/flush_on 字符串)
    nlohmann::json log_json;
    try {
        log_json = dztrader::core::load_json_section<nlohmann::json>(config_path, "log");
    } catch (const std::exception&) {
        // 文件损坏用空 object, 后续用默认值
        log_json = nlohmann::json::object();
    }
    // 配置存在但无 log 段/文件不可读时 load_json_section 返回 null,
    // 直接 value() 会抛异常崩溃; 补 null 路径: 缺配置用默认值 + 警告
    std::string log_level = "debug";
    std::string log_flush_on = "info";
    if (log_json.is_object()) {
        log_level = log_json.value("level", log_level);
        log_flush_on = log_json.value("flush_on", log_flush_on);
    } else {
        SPDLOG_WARN("log config missing, using defaults | path={}", config_path.string());
    }

    // 4. 用 log_level/log_flush_on 填充 LoggerSetup -> set_default_logger
    //    统一用 LogConfig::parse_level 校验 (契约 01-log), 非法回落 info
    dztrader::log::LoggerSetup log_setup;
    log_setup.logger_name = dztrader::this_process::exe_stem();
    log_setup.log_dir = dztrader::paths::logs();
    log_setup.level = dztrader::platform::LogConfig::parse_level(log_level)
                          .value_or(spdlog::level::info);
    log_setup.flush_level = dztrader::platform::LogConfig::parse_level(log_flush_on)
                                .value_or(spdlog::level::info);
    dztrader::log::set_default_logger(log_setup);
    SPDLOG_INFO("master starting | pid={}", dztrader::this_process::pid());

    // 5. OrphanGuard：获取 file_lock + 清理残留子进程
    dztrader::master::OrphanGuard orphan_guard;
    try {
        orphan_guard.startup();
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("orphan guard startup failed | error=\"{}\"", e.what());
        return 1;
    }

    // 6. ProcessRegistry：加载 dztraderd.json + 扫描 gateways/
    dztrader::master::ProcessRegistry registry;
    try {
        registry.load(config_path);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("process registry load failed | path={} error=\"{}\"", config_path.string(),
                     e.what());
        return 1;
    }

    // 7. ShmManager：创建事件通道 + md 通道 (构造即初始化, 失败抛异常)
    //    log_config_ 在 ShmManager 构造体内从 cfg_path 的 "log" section 自行加载
    std::unique_ptr<dztrader::master::ShmManager> shm_mgr;
    try {
        shm_mgr = std::make_unique<dztrader::master::ShmManager>(
            cfg.shm_global, config_path);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("shm manager init failed | error=\"{}\"", e.what());
        return 1;
    }

    // 8. 创建 io_context 和 ProcessSupervisor
    //    single_stop_timeout_sec 从 dztraderd.json [master] 段读取 (默认 3),
    //    控制 on_single_stop_timeout 强制 kill 的超时阈值 (失败路径 C)
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    dztrader::master::ProcessSupervisor supervisor(ioc, registry, *shm_mgr, orphan_guard,
                                                    cfg.master.single_stop_timeout_sec);

    // 注入 supervisor 到 ShmManager，使 PROCESS_CONTROL 帧能调用 start_process/stop_process
    shm_mgr->set_supervisor(&supervisor);

    // 启动 shm 周期任务 (60s PageCleaner + event channel 信号量监听 + event 通道维护任务)
    shm_mgr->start_periodic_tasks(ioc);
    shm_mgr->start_event_shm_maintenance(ioc);

    // 9. 并行启动所有子进程（单个失败不阻止其他进程）
    supervisor.start_all();

    // 启动完成后推送完整快照 (与 mdctp 的 report_full_snapshot 对称):
    // master 自己的 config + 所有子进程的 ProcessStatus (含刚拉起的 Starting 状态)
    shm_mgr->report_full_snapshot();

    // 10. 注册信号处理（SIGINT, SIGTERM）
    //    首次信号：优雅关闭。二次信号：强制退出。
    //    boost::asio::signal_set::async_wait 是 one-shot — 首次回调触发后 signal_set 解除武装,
    //    第二次 Ctrl+C 会落到 SIG_DFL, 绕过 release_all()/orphan_guard.cleanup()。
    //    故在首次信号分支末尾重新 async_wait 同一 handler (递归重武装), 让二次信号进入 force-exit 分支。
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    std::function<void(const boost::system::error_code&, int)> signal_handler;
    signal_handler = [&supervisor, &shm_mgr, &orphan_guard, &ioc, &signals,
                      &signal_handler](const boost::system::error_code& ec, int sig) {
        if (ec) { return; }
        try {
            if (supervisor.is_shutting_down()) {
                SPDLOG_CRITICAL("second signal received, force terminating children | sig={}", sig);
                // 强制终止所有子进程 (避免孤儿)
                try {
                    supervisor.force_terminate_all();
                } catch (const std::exception& e) {
                    SPDLOG_CRITICAL("force_terminate_all error | error=\"{}\"", e.what());
                }
                shm_mgr->release_all();
                orphan_guard.cleanup();
                ioc.stop();
                return;
            }

            SPDLOG_INFO("signal received | sig={}", sig);

            auto cleanup_timer =
                std::make_shared<boost::asio::steady_timer>(ioc, std::chrono::seconds(10));

            supervisor.set_shutdown_callback([cleanup_timer, &shm_mgr, &orphan_guard, &ioc]() {
                try {
                    cleanup_timer->cancel();
                    shm_mgr->release_all();
                    orphan_guard.cleanup();
                    ioc.stop();
                } catch (const std::exception& e) {
                    SPDLOG_CRITICAL("shutdown callback error | error=\"{}\"", e.what());
                    ioc.stop();
                }
            });

            supervisor.shutdown();

            cleanup_timer->async_wait(
                [&shm_mgr, &orphan_guard, &ioc](const boost::system::error_code& ec) {
                    if (!ec) {
                        try {
                            SPDLOG_WARN("hard timeout, forced exit");
                            shm_mgr->release_all();
                            orphan_guard.cleanup();
                        } catch (const std::exception& e) {
                            SPDLOG_CRITICAL("hard timeout cleanup error | error=\"{}\"", e.what());
                        }
                        ioc.stop();
                    }
                });

            // 重新武装 signal_set, 使第二次 Ctrl+C 触发上面的强制退出分支。
            signals.async_wait(signal_handler);
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("signal handler error | sig={} error=\"{}\"", sig, e.what());
        }
    };
    signals.async_wait(signal_handler);

    // 11. 运行事件循环
    //    异常不退出：数据进程不可断，宁可持续运行也不终止
    while (true) {
        try {
            ioc.run();
            break;
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("uncaught exception in event loop | error=\"{}\"", e.what());
        }
    }

    SPDLOG_INFO("master exited");
    return 0;
}
