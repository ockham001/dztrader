/**
 * @file process_supervisor_test.cpp
 * @brief ProcessSupervisor 集成测试。
 */

#include "process_supervisor.h"
#include "test_temp_dir.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/process.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>
#include <vector>

namespace dztrader::master {
namespace {

class ProcessSupervisorTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "process_supervisor_test";
        log_cfg.log_dir = dztrader::test::unique_temp_dir("dz_test_logs");
        log_cfg.enable_console = true;
        log_cfg.level = spdlog::level::debug;
        dztrader::log::set_default_logger(log_cfg);
    }

    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_supervisor_test");
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", tmp_dir_.string());
        dztrader::paths::reset_for_test();  // 清空进程级缓存, 指向本用例 home

        worker_exe_ = dztrader::this_process::exe_dir() /
                      "test_worker"
#ifdef _WIN32
                      ".exe"
#endif
            ;

        // 测试用最小配置文件 (init 配置由参数注入, 文件仅供 parse_master_json 测试用)
        // shm.event 含 page_size_mb=1 (测试用小页面, 降低内存占用)
        cfg_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream ofs(cfg_path_);
        ofs << R"({"log":{}, "master":{}, "shm":{"meta_file_size":1048576,"event":{"page_size_mb":1,"preload_points":{},"check_interval_min":5,"check_pages":1,"check_bytes":0}}})";
    }

    void TearDown() override {
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        // 先释放 SHM 映射，再删除 tmp_dir_。
        // 若先删除文件，~ChannelMetaImpl() 析构 managed_mapped_file/ProcessMutex
        // 时会访问已删除文件，触发 SEH 0xc0000005。
        if (shm_mgr_) shm_mgr_->release_all();
        dztrader::paths::reset_for_test();
        std::filesystem::remove_all(tmp_dir_);
    }

    ProcessEntry make_strategy_entry(const std::string& name,
                                     const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = Category::Strategy;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty() ? std::filesystem::current_path()
                                                            : worker_exe_.parent_path();
        entry.restart = default_restart_policy(Category::Strategy);
        entry.restart.enabled = false;  // 测试场景禁用重启
        return entry;
    }

    ProcessEntry make_internal_entry(const std::string& name,
                                     Category category,
                                     const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = category;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty() ? std::filesystem::current_path()
                                                            : worker_exe_.parent_path();
        entry.restart = default_restart_policy(category);
        entry.restart.enabled = false;  // 测试场景禁用重启
        return entry;
    }

    /// 自动重启测试条目: 与 make_strategy_entry 同构, 仅打开 restart 策略。
    ProcessEntry make_restart_entry(const std::string& name,
                                    const std::vector<std::string>& args,
                                    int max_attempts = 3,
                                    int backoff_sec = 1) {
        auto entry = make_strategy_entry(name, args);
        entry.restart.enabled = true;
        entry.restart.max_attempts = max_attempts;
        entry.restart.backoff_sec = backoff_sec;
        return entry;
    }

    /// 事件通道进程帧: RTN_PROCESS_STATUS (ext payload) 与 SHUTDOWN (ext instance_id)。
    struct ProcessFrame {
        DzFrameType type{};
        std::string target;  ///< SHUTDOWN 的 instance_id; RTN 的 payload.name
        platform::ProcessStatus status{};
    };

    shm::Reader make_event_reader(const std::string& reader_name) {
        return shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                   reader_name);
    }

    /// 排空事件通道, 只保留进程控制相关帧 (其余帧如订阅者更新直接丢弃)。
    static std::vector<ProcessFrame> drain_process_frames(shm::Reader& reader) {
        std::vector<ProcessFrame> frames;
        for (int i = 0; i < 256; ++i) {
            const auto* raw = reader.next_frame();
            if (!raw) {
                break;
            }
            shm::FrameView view(raw);
            if (view.type() == DZ_FRAME_SHUTDOWN) {
                ProcessFrame frame;
                frame.type = view.type();
                frame.target = view.ext_inst_id();
                frames.push_back(std::move(frame));
            } else if (view.type() == DZ_FRAME_RTN_PROCESS_STATUS) {
                if (view.ext_payload_size() == 0) {
                    continue;
                }
                const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                ProcessFrame frame;
                frame.type = view.type();
                frame.status = nlohmann::json::parse(data, data + view.ext_payload_size())
                                   .get<platform::ProcessStatus>();
                frame.target = frame.status.name;
                frames.push_back(std::move(frame));
            }
        }
        return frames;
    }

    static bool has_status(const std::vector<ProcessFrame>& frames,
                           const std::string& name,
                           ChildState state) {
        return std::any_of(frames.begin(), frames.end(), [&](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == name &&
                   frame.status.state == state;
        });
    }

    static bool has_shutdown_for(const std::vector<ProcessFrame>& frames, const std::string& name) {
        return std::any_of(frames.begin(), frames.end(), [&](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_SHUTDOWN && frame.target == name;
        });
    }

    /// 条件等待: 驱动 io_context 直到 pred 成立或超时 (替代固定 sleep)。
    template <typename Pred>
    bool wait_for_io(std::chrono::milliseconds timeout, Pred pred) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred() && std::chrono::steady_clock::now() < deadline) {
            ioc_.restart();
            ioc_.run_for(std::chrono::milliseconds(20));
        }
        return pred();
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path cfg_path_;
    std::optional<std::string> orig_home_;
    std::filesystem::path worker_exe_;
    boost::asio::io_context ioc_;
    ProcessRegistry registry_;
    std::unique_ptr<ShmManager> shm_mgr_;
    OrphanGuard orphan_guard_;
};

// 测试用默认配置
inline ShmGlobalConfig make_default_shm_global() { return {.meta_file_size = 1 * 1024 * 1024}; }

// stop_process 在 shutdown 进行中应被拒绝（无副作用）
TEST_F(ProcessSupervisorTest, StopProcessRejectedDuringShutdown) {
    auto entry = make_strategy_entry("longrun", {"longrun", "60"});
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);  // 证明 spawn 真的发生
    ASSERT_EQ(supervisor.children()[0]->state(), ChildState::Running);

    auto reader = make_event_reader("reject_stop_probe");

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // 关闭批次同步派发: SHUTDOWN + Stopping 均须出现 (证明关闭链路执行过)
    const auto dispatched = drain_process_frames(reader);
    EXPECT_TRUE(has_shutdown_for(dispatched, "longrun"));
    EXPECT_TRUE(has_status(dispatched, "longrun", ChildState::Stopping));

    // shutdown 进行中调用 stop_process 应被忽略: 不得再写任何进程控制帧
    // (shutting_down_ 守卫被删 -> 重复 SHUTDOWN/Stopping 帧, 此处变红)
    supervisor.stop_process("longrun");
    EXPECT_TRUE(drain_process_frames(reader).empty());

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));

    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// stop_process 找不到子进程时不崩溃且不产生状态帧
TEST_F(ProcessSupervisorTest, StopProcessNonExistentName) {
    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    auto reader = make_event_reader("stop_missing_probe");
    EXPECT_NO_THROW(supervisor.stop_process("nonexistent"));
    EXPECT_TRUE(drain_process_frames(reader).empty());
    EXPECT_TRUE(supervisor.children().empty());

    orphan_guard_.cleanup();
}

// stop_process 对已停止的子进程不崩溃且不产生状态帧
TEST_F(ProcessSupervisorTest, StopProcessAlreadyStopped) {
    auto entry = make_strategy_entry("quick", {"quick", "1"});
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // 等子进程自然退出 (exit 0)
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));

    // 已退出进程不在 children_ 中: stop_process 走 not-found 分支, 无状态帧
    auto reader = make_event_reader("stop_stopped_probe");
    EXPECT_NO_THROW(supervisor.stop_process("quick"));
    EXPECT_TRUE(drain_process_frames(reader).empty());

    orphan_guard_.cleanup();
}

// 正向基线 (RestartPolicy 分支①): 崩溃后 1s 退避内被重新拉起 -> Running。
// 与下方 CancelPendingRestartPreventsRevival 同配置, 作为"未取消时会复活"的对照。
TEST_F(ProcessSupervisorTest, RestartAfterCrashSpawnsNewChild) {
    const auto marker = tmp_dir_ / "revive_crash_once.marker";
    registry_.register_strategy(
        make_restart_entry("stg_crash", {"crash_once", marker.string(), "7"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    auto reader = make_event_reader("restart_revive_probe");
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);
    const auto first_pid = static_cast<int>(supervisor.children()[0]->pid());
    ASSERT_NE(first_pid, 0);

    // 首次启动崩溃 (exit 7): 推送 Crashed 并挂起 1s 退避重启
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));
    auto observed = drain_process_frames(reader);
    ASSERT_TRUE(has_status(observed, "stg_crash", ChildState::Crashed));

    // 退避窗口内自动重启: 新 pid 的 Running 出现 (契约: 重启成功推 Running, event 缺失)
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(3000), [&] {
        auto child = supervisor.find_child("stg_crash");
        return child && child->state() == ChildState::Running &&
               static_cast<int>(child->pid()) != first_pid;
    }));
    auto restarted = drain_process_frames(reader);
    observed.insert(observed.end(), restarted.begin(), restarted.end());

    auto revived = supervisor.find_child("stg_crash");
    ASSERT_NE(revived, nullptr);
    EXPECT_EQ(revived->state(), ChildState::Running);
    EXPECT_NE(static_cast<int>(revived->pid()), first_pid);
    EXPECT_TRUE(std::filesystem::exists(marker));  // 首崩已落盘, 重启实例常驻

    // 两次 Running 对应两次 spawn (首启 + 重启), 重启帧 event 缺失 (自发状态变化)
    std::vector<int> running_pids;
    for (const auto& frame : observed) {
        if (frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "stg_crash" &&
            frame.status.state == ChildState::Running) {
            running_pids.push_back(frame.status.pid);
            EXPECT_FALSE(frame.status.event.has_value());
        }
    }
    ASSERT_EQ(running_pids.size(), 2u);
    EXPECT_NE(running_pids[0], running_pids[1]);

    supervisor.shutdown();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// 内部进程重启与网关一致地收到恢复参数: --recover 追加机制不按类别区分
TEST_F(ProcessSupervisorTest, InternalProcessRestartReceivesRecoverArg) {
    const auto marker = tmp_dir_ / "internal_crash_once.marker";
    const auto argv_log = tmp_dir_ / "internal_argv.log";
    auto entry =
        make_internal_entry("dzsysmon", Category::Internal, {"crash_once", marker.string(), "7"});
    entry.restart.enabled = true;
    entry.restart.max_attempts = 2;
    entry.restart.backoff_sec = 1;
    entry.env["DZ_TEST_ARGV_DUMP"] = argv_log.string();
    registry_.register_gateway(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    supervisor.start_all();
    // 内部进程随第二趟启动 (非 md/策略), 首启即 spawn
    ASSERT_EQ(supervisor.children().size(), 1u);
    const auto first_pid = static_cast<int>(supervisor.children()[0]->pid());
    ASSERT_NE(first_pid, 0);

    // 首崩 (exit 7) -> 1s 退避后自动重启 (重启实例读到 marker 后常驻)
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(3000), [&] {
        auto child = supervisor.find_child("dzsysmon");
        return child && child->state() == ChildState::Running &&
               static_cast<int>(child->pid()) != first_pid;
    }));

    // 捕获两次 spawn 的 argv: 首启无 --recover, 重启追加 --recover。
    // master 在 spawn 成功后立即推 Running, 子进程可能尚未执行到 argv 落盘, 必须轮询文件。
    auto read_spawn_args = [&] {
        std::vector<std::string> lines;
        std::ifstream ifs(argv_log);
        std::string line;
        while (std::getline(ifs, line)) {
            lines.push_back(line);
        }
        return lines;
    };
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(3000),
                            [&] { return read_spawn_args().size() >= 2u; }));
    const auto spawn_args = read_spawn_args();
    ASSERT_EQ(spawn_args.size(), 2u);
    EXPECT_EQ(spawn_args[0].find("--recover"), std::string::npos);
    EXPECT_NE(spawn_args[1].find("--recover"), std::string::npos);

    supervisor.shutdown();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// Fix1: 崩溃退避窗口内 cancel_pending_restart 后不得"复活"进程
// (Remove/显式停止在进程不运行路径取消挂起的退避重启定时器, 契约"remove 不重启")
TEST_F(ProcessSupervisorTest, CancelPendingRestartPreventsRevival) {
    const auto marker = tmp_dir_ / "cancel_crash_once.marker";
    // 与 RestartAfterCrashSpawnsNewChild 同配置: 未取消时 1s 退避内必被拉起
    registry_.register_strategy(
        make_restart_entry("stg_crash", {"crash_once", marker.string(), "7"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    auto reader = make_event_reader("cancel_restart_probe");
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // 崩溃退出 -> on_child_exit 挂 1s 退避重启定时器
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));
    auto observed = drain_process_frames(reader);
    ASSERT_TRUE(has_status(observed, "stg_crash", ChildState::Crashed));

    // 移除/显式停止路径: 取消挂起的重启定时器
    supervisor.cancel_pending_restart("stg_crash");

    // 越过退避窗口验证未复活 (对照组在 RestartAfterCrashSpawnsNewChild 观察到 children 变 1)
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    auto after = drain_process_frames(reader);
    observed.insert(observed.end(), after.begin(), after.end());

    EXPECT_EQ(supervisor.children().size(), 0u);
    const auto running_frames =
        std::count_if(observed.begin(), observed.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "stg_crash" &&
                   frame.status.state == ChildState::Running;
        });
    EXPECT_EQ(running_frames, 1);  // 只有首启那次 Running, 无复活

    orphan_guard_.cleanup();
}

// RestartPolicy 分支②: 达到 max_attempts 停止重启, 状态保持 Crashed。
// worker 4 参数模式立即以 7 退出 -> 每次 spawn 都崩溃。
TEST_F(ProcessSupervisorTest, MaxRestartAttemptsStopsRetryLoop) {
    registry_.register_strategy(
        make_restart_entry("stg_crash", {"stg_crash", "0", "7"}, /*max_attempts=*/1));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    auto reader = make_event_reader("max_attempts_probe");

    std::vector<ProcessFrame> observed;
    auto collect = [&] {
        auto frames = drain_process_frames(reader);
        observed.insert(observed.end(), frames.begin(), frames.end());
    };
    auto running_count = [&] {
        return std::count_if(observed.begin(), observed.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "stg_crash" &&
                   frame.status.state == ChildState::Running;
        });
    };

    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // 首崩 -> 第 1 次重启 (第 2 次 Running) -> 再崩即达上限
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(6000), [&] {
        collect();
        return running_count() >= 2;
    }));
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(3000),
                            [&] { return supervisor.children().empty(); }));

    // 越过任何后续退避窗口: 不得再重启
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    collect();

    EXPECT_EQ(running_count(), 2);
    EXPECT_TRUE(supervisor.children().empty());
    EXPECT_TRUE(has_status(observed, "stg_crash", ChildState::Crashed));

    orphan_guard_.cleanup();
}

// RestartPolicy 分支③: 重启 spawn 失败终止循环 (不再重试, 状态 Crashed)。
TEST_F(ProcessSupervisorTest, RestartSpawnFailureStopsRetryLoop) {
    // 首次 spawn 用临时副本, 崩溃后删除副本 -> 重启 spawn 必失败
    const auto copied_exe = tmp_dir_ / "test_worker_crash_spawn";
    std::filesystem::copy_file(worker_exe_, copied_exe);
    auto entry = make_restart_entry("stg_crash", {"stg_crash", "0", "7"});
    entry.exe = copied_exe;
    entry.start_dir = copied_exe.parent_path();
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    auto reader = make_event_reader("spawn_fail_probe");
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // 首崩 -> 退避重启已排定
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));
    auto observed = drain_process_frames(reader);
    ASSERT_TRUE(has_status(observed, "stg_crash", ChildState::Crashed));

    // 退避窗口内删除 exe: 重启 spawn 失败 -> Crashed(spawn child failed) 并终止循环
    std::filesystem::remove(copied_exe);
    ASSERT_FALSE(std::filesystem::exists(copied_exe));

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    auto after = drain_process_frames(reader);
    observed.insert(observed.end(), after.begin(), after.end());

    const bool saw_spawn_failure =
        std::any_of(observed.begin(), observed.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "stg_crash" &&
                   frame.status.state == ChildState::Crashed &&
                   frame.status.message.find("spawn child failed") != std::string::npos;
        });
    EXPECT_TRUE(saw_spawn_failure);
    EXPECT_TRUE(supervisor.children().empty());
    // spawn 失败不会产生 Running: 全程只有首启一次
    const auto running_frames =
        std::count_if(observed.begin(), observed.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "stg_crash" &&
                   frame.status.state == ChildState::Running;
        });
    EXPECT_EQ(running_frames, 1);

    orphan_guard_.cleanup();
}

// RestartPolicy 分支④: 正常退出 (exit 0) 不重启。
TEST_F(ProcessSupervisorTest, NormalExitNotRestarted) {
    registry_.register_strategy(make_restart_entry("quick", {"quick", "1"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    auto reader = make_event_reader("normal_exit_probe");
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // exit 0 -> 推 Stopped, 不安排重启
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));
    auto observed = drain_process_frames(reader);
    ASSERT_TRUE(has_status(observed, "quick", ChildState::Stopped));

    // 越过 1s 退避窗口: 正常退出不得重启
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    auto after = drain_process_frames(reader);
    observed.insert(observed.end(), after.begin(), after.end());

    EXPECT_TRUE(supervisor.children().empty());
    const auto running_frames =
        std::count_if(observed.begin(), observed.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "quick" &&
                   frame.status.state == ChildState::Running;
        });
    EXPECT_EQ(running_frames, 1);
    EXPECT_FALSE(has_status(observed, "quick", ChildState::Crashed));

    orphan_guard_.cleanup();
}

// 停止运行中子进程: 定向 SHUTDOWN -> Stopping -> 超时强杀 -> Crashed, children 清空。
// 覆盖 stop_single_child / on_single_stop_timeout 的派发与状态链。
TEST_F(ProcessSupervisorTest, StopRunningChildForceKilledOnTimeout) {
    registry_.register_strategy(make_strategy_entry("longrun", {"longrun", "60"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);
    ASSERT_EQ(supervisor.children()[0]->state(), ChildState::Running);
    const auto pid = static_cast<int>(supervisor.children()[0]->pid());
    ASSERT_NE(pid, 0);

    auto reader = make_event_reader("stop_running_probe");
    supervisor.stop_process("longrun");

    // 同步派发: 定向 SHUTDOWN + Stopping (pid 仍为运行中 pid, event 缺失=自发)
    const auto dispatched = drain_process_frames(reader);
    EXPECT_TRUE(has_shutdown_for(dispatched, "longrun"));
    const auto stopping =
        std::find_if(dispatched.begin(), dispatched.end(), [](const ProcessFrame& frame) {
            return frame.type == DZ_FRAME_RTN_PROCESS_STATUS && frame.status.name == "longrun" &&
                   frame.status.state == ChildState::Stopping;
        });
    ASSERT_NE(stopping, dispatched.end());
    EXPECT_EQ(stopping->status.pid, pid);
    EXPECT_FALSE(stopping->status.event.has_value());

    // worker 不读事件通道 -> SHUTDOWN 无效, 1s 超时强杀 (worker 自身 60s 才退出)
    ASSERT_TRUE(wait_for_io(std::chrono::milliseconds(5000),
                            [&] { return supervisor.children().empty(); }));

    const auto exited = drain_process_frames(reader);
    const auto crashed = std::find_if(exited.rbegin(), exited.rend(), [](const ProcessFrame& f) {
        return f.type == DZ_FRAME_RTN_PROCESS_STATUS && f.status.name == "longrun";
    });
    ASSERT_NE(crashed, exited.rend());
    EXPECT_EQ(crashed->status.state, ChildState::Crashed);  // 强杀 exit_code != 0
    EXPECT_NE(crashed->status.message.find("exit_code="), std::string::npos);
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// 整体关闭逆序分批 (纯函数): 策略 -> 交易 -> 行情 -> 内部进程 -> dzweb, 空批次不产生
TEST(ProcessShutdownBatches, ReverseOrderByCategory) {
    std::vector<std::pair<std::string, Category>> running{
        {"dzweb", Category::WebUI},    {"dzmd_ctp", Category::GatewayMd},
        {"stg_a", Category::Strategy}, {"dztd_ctp", Category::GatewayTd},
        {"stg_b", Category::Strategy}, {"dzsysmon", Category::Internal},
    };
    auto batches = build_shutdown_batches(running);
    ASSERT_EQ(batches.size(), 5u);
    EXPECT_EQ(batches[0], (std::vector<std::string>{"stg_a", "stg_b"}));
    EXPECT_EQ(batches[1], (std::vector<std::string>{"dztd_ctp"}));
    EXPECT_EQ(batches[2], (std::vector<std::string>{"dzmd_ctp"}));
    EXPECT_EQ(batches[3], (std::vector<std::string>{"dzsysmon"}));
    EXPECT_EQ(batches[4], (std::vector<std::string>{"dzweb"}));
}

// 内部进程批次位于行情网关之后、UI 之前 (md -> internal -> webui)
TEST(ProcessShutdownBatches, InternalBatchBetweenMdAndWebui) {
    auto batches = build_shutdown_batches({
        {"dzweb", Category::WebUI},
        {"dzsysmon", Category::Internal},
        {"dzmd_ctp", Category::GatewayMd},
    });
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], (std::vector<std::string>{"dzmd_ctp"}));
    EXPECT_EQ(batches[1], (std::vector<std::string>{"dzsysmon"}));
    EXPECT_EQ(batches[2], (std::vector<std::string>{"dzweb"}));
}

// 单一类别只产生一个批次; 空输入产生零批次
TEST(ProcessShutdownBatches, SingleCategoryAndEmpty) {
    auto one = build_shutdown_batches({{"dzmd_ctp", Category::GatewayMd}});
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0], (std::vector<std::string>{"dzmd_ctp"}));

    EXPECT_TRUE(build_shutdown_batches({}).empty());
}

// 整体关闭事件驱动: 两个策略 (同一批次) 均退出后完成, 不依赖广播帧
TEST_F(ProcessSupervisorTest, ShutdownStopsAllChildrenSequentially) {
    registry_.register_strategy(make_strategy_entry("long_a", {"longrun", "60"}));
    registry_.register_strategy(make_strategy_entry("long_b", {"longrun", "60"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 2u);  // 证明 spawn 真的发生

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // worker 不读事件通道 -> SHUTDOWN 无效, 依赖批超时强制终止
    // (single_stop_timeout_sec 默认 3s), 全部退出后 children 清空
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(10));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// 整体关闭多类别逐批推进: 五类各一进程 (含内部进程), 依赖每批超时强制终止后进入下一批,
// 全部退出后 children 清空 (验证批次间 timer 重建与推进链路)
TEST_F(ProcessSupervisorTest, ShutdownStopsAllCategoriesSequentially) {
    registry_.register_strategy(make_strategy_entry("stg_a", {"longrun", "60"}));
    registry_.register_gateway(
        make_internal_entry("dztd_ctp", Category::GatewayTd, {"longrun", "60"}));
    registry_.register_gateway(
        make_internal_entry("dzmd_ctp", Category::GatewayMd, {"longrun", "60"}));
    registry_.register_gateway(
        make_internal_entry("dzsysmon", Category::Internal, {"longrun", "60"}));
    registry_.register_gateway(make_internal_entry("dzweb", Category::WebUI, {"longrun", "60"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    // single_stop_timeout_sec=1: 每批 1s 超时, 5 批最坏 5s, 测试可控
    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_,
                                 /*single_stop_timeout_sec=*/1);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 5u);  // 证明五类进程均 spawn

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // worker 不读事件通道 -> SHUTDOWN 无效, 依赖各批超时强制终止
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(15));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// GatewayMd 进程退出时 master 代发 NOTIFY_MD_STOPPED (md 停止后果接线, 自检补测)
TEST_F(ProcessSupervisorTest, MdExitBroadcastsNotifyStopped) {
    registry_.register_gateway(
        make_internal_entry("dzmd_ctp", Category::GatewayMd, {"quick", "1"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();

    // 独立 reader 探测事件通道 (须在子进程退出前注册)
    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "notify_stopped_probe");

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));

    bool saw_stopped = false;
    for (int i = 0; i < 64; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_NOTIFY_MD_STOPPED &&
            std::string_view(view.ext_inst_id()) == "dzmd_ctp") {
            saw_stopped = true;
        }
    }
    EXPECT_TRUE(saw_stopped);

    orphan_guard_.cleanup();
}

// ── 单行情源编排 (契约 4.4/4.6): pending / 晚到 STARTED 补启动 ──

// start_process 对绑定源未 ready 的策略: 进 pending (Starting + message), 不 spawn
TEST_F(ProcessSupervisorTest, StrategyStartPendingWhenMdNotReady) {
    // 策略绑定一个未配置/未 ready 的源 "missing_md"
    ProcessEntry entry;
    entry.name = "stg_pending";
    entry.category = Category::Strategy;
    entry.exe = worker_exe_;
    entry.start_dir = worker_exe_.parent_path();
    entry.restart = default_restart_policy(Category::Strategy);
    entry.restart.enabled = false;
    entry.md_source = "missing_md";
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    shm_mgr_->set_supervisor(&supervisor);

    // 运行期 start: 源未 ready -> pending, 不 spawn
    EXPECT_FALSE(supervisor.start_process("stg_pending"));
    EXPECT_EQ(supervisor.find_child("stg_pending"), nullptr);
    std::string msg;
    EXPECT_TRUE(supervisor.is_pending_strategy("stg_pending", msg));
    EXPECT_EQ(msg, "waiting for md source missing_md");

    orphan_guard_.cleanup();
}

// on_md_channel_ready (晚到 STARTED) 启动该源全部 pending 策略并清空 pending
TEST_F(ProcessSupervisorTest, OnMdChannelReadyStartsPendingStrategies) {
    for (const auto& name : {"stg_a", "stg_b"}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = Category::Strategy;
        entry.exe = worker_exe_;
        entry.start_dir = worker_exe_.parent_path();
        entry.restart = default_restart_policy(Category::Strategy);
        entry.restart.enabled = false;
        entry.md_source = "dzmd_ctp";
        registry_.register_strategy(entry);
    }

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    shm_mgr_->set_supervisor(&supervisor);

    // 源未 ready: start 全部进 pending
    EXPECT_FALSE(supervisor.start_process("stg_a"));
    EXPECT_FALSE(supervisor.start_process("stg_b"));
    std::string msg;
    EXPECT_TRUE(supervisor.is_pending_strategy("stg_a", msg));
    EXPECT_TRUE(supervisor.is_pending_strategy("stg_b", msg));

    // 建通道 + 置 ready (模拟 master 编排), 触发 on_md_channel_ready 补启动
    shm_mgr_->create_md_channel("dzmd_ctp");
    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    shm::MultiWriter writer =
        shm::MultiWriter::create(std::make_shared<shm::ChannelMeta>(std::move(meta)), "md_sim");
    platform::write_ext_inst_raw(writer, DZ_FRAME_NOTIFY_MD_STARTED, "dzmd_ctp");
    shm_mgr_->drain_event_channel();

    // pending 已清空, 两个策略已 spawn
    EXPECT_FALSE(supervisor.is_pending_strategy("stg_a", msg));
    EXPECT_FALSE(supervisor.is_pending_strategy("stg_b", msg));
    EXPECT_NE(supervisor.find_child("stg_a"), nullptr);
    EXPECT_NE(supervisor.find_child("stg_b"), nullptr);

    // 清理
    supervisor.shutdown();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    orphan_guard_.cleanup();
}

// 策略 spawn 前预注册 md 读者 (契约 4.3): on_md_channel_ready 后 readers 表含 stg.<name>
TEST_F(ProcessSupervisorTest, StrategyReaderPreRegisteredOnMdChannel) {
    ProcessEntry entry;
    entry.name = "stg_reader";
    entry.category = Category::Strategy;
    entry.exe = worker_exe_;
    entry.start_dir = worker_exe_.parent_path();
    entry.restart = default_restart_policy(Category::Strategy);
    entry.restart.enabled = false;
    entry.md_source = "dzmd_ctp";
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    shm_mgr_->set_supervisor(&supervisor);

    // pending -> ready -> spawn
    EXPECT_FALSE(supervisor.start_process("stg_reader"));
    shm_mgr_->create_md_channel("dzmd_ctp");
    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    shm::MultiWriter writer =
        shm::MultiWriter::create(std::make_shared<shm::ChannelMeta>(std::move(meta)), "md_sim");
    platform::write_ext_inst_raw(writer, DZ_FRAME_NOTIFY_MD_STARTED, "dzmd_ctp");
    shm_mgr_->drain_event_channel();

    // md 通道 readers 表已含 stg.stg_reader (master 预注册)
    auto md_meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = md_meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.stg_reader") != names.end());

    // 清理
    supervisor.shutdown();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    orphan_guard_.cleanup();
}

}  // namespace
}  // namespace dztrader::master
