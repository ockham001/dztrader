/**
 * @file process_supervisor_test.cpp
 * @brief ProcessSupervisor 集成测试。
 */

#include "process_supervisor.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <memory>

namespace dztrader::master {
namespace {

/// 构建进程唯一临时目录名（PID + 随机数）：ctest -j 并行时避免多个测试 exe
/// 共用固定目录名（如 dz_supervisor_test）导致 SHM 文件互相占用的冲突。
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

class ProcessSupervisorTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dztrader::log::LoggerSetup log_cfg;
        log_cfg.logger_name = "process_supervisor_test";
        log_cfg.log_dir = std::filesystem::temp_directory_path() / "dz_test_logs";
        log_cfg.enable_console = true;
        log_cfg.level = spdlog::level::debug;
        dztrader::log::set_default_logger(log_cfg);
    }

    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_supervisor_test");
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", tmp_dir_.string());

        worker_exe_ = dztrader::this_process::exe_dir() / "test_worker"
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
        std::filesystem::remove_all(tmp_dir_);
    }

    ProcessEntry make_strategy_entry(const std::string& name,
                                     const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = Category::Strategy;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty()
            ? std::filesystem::current_path()
            : worker_exe_.parent_path();
        entry.restart = default_restart_policy(Category::Strategy);
        entry.restart.enabled = false;  // 测试场景禁用重启
        return entry;
    }

    ProcessEntry make_internal_entry(const std::string& name, Category category,
                                     const std::vector<std::string>& args = {}) {
        ProcessEntry entry;
        entry.name = name;
        entry.category = category;
        entry.exe = worker_exe_;
        entry.args = args;
        entry.start_dir = worker_exe_.parent_path().empty()
            ? std::filesystem::current_path()
            : worker_exe_.parent_path();
        entry.restart = default_restart_policy(category);
        entry.restart.enabled = false;  // 测试场景禁用重启
        return entry;
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
inline ShmGlobalConfig make_default_shm_global() {
    return {.meta_file_size = 1 * 1024 * 1024};
}

// stop_process 在 shutdown 进行中应被拒绝（无副作用）
TEST_F(ProcessSupervisorTest, StopProcessRejectedDuringShutdown) {
    auto entry = make_strategy_entry("longrun", {"longrun", "60"});
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // shutdown 进行中调用 stop_process 应被忽略
    EXPECT_NO_THROW(supervisor.stop_process("longrun"));

    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));

    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// stop_process 找不到子进程时不崩溃
TEST_F(ProcessSupervisorTest, StopProcessNonExistentName) {
    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    EXPECT_NO_THROW(supervisor.stop_process("nonexistent"));

    orphan_guard_.cleanup();
}

// stop_process 对已停止的子进程不崩溃
TEST_F(ProcessSupervisorTest, StopProcessAlreadyStopped) {
    auto entry = make_strategy_entry("quick", {"quick", "1"});
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();

    // 等子进程自然退出
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(3));
    EXPECT_EQ(supervisor.children().size(), 0u);

    // 对已退出的子进程调用 stop_process 不应崩溃
    EXPECT_NO_THROW(supervisor.stop_process("quick"));

    orphan_guard_.cleanup();
}

// Fix1: 崩溃退避窗口内 cancel_pending_restart 后不得"复活"进程
// (Remove/显式停止在进程不运行路径取消挂起的退避重启定时器, 契约"remove 不重启")
TEST_F(ProcessSupervisorTest, CancelPendingRestartPreventsRevival) {
    auto entry = make_strategy_entry("stg_crash", {"longrun", "60"});
    entry.restart.enabled = true;
    entry.restart.max_attempts = 3;
    entry.restart.backoff_sec = 1;  // 1s 退避: 未取消则观察窗口内必被拉起
    registry_.register_strategy(entry);

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();
    ASSERT_EQ(supervisor.children().size(), 1u);

    // 强杀模拟崩溃 (非零退出码) -> on_child_exit 挂 1s 退避重启定时器
    supervisor.children()[0]->terminate();
    ioc_.restart();
    // 轮询等待退出回调完成 (children 清空 = on_child_exit 已执行, 定时器已挂上)
    for (int i = 0; i < 20 && !supervisor.children().empty(); ++i) {
        ioc_.run_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(supervisor.children().size(), 0u);

    // 移除/显式停止路径: 取消挂起的重启定时器
    supervisor.cancel_pending_restart("stg_crash");

    // 越过退避窗口验证未复活 (未取消时 launch_child 会重新拉起, children 变 1;
    // 复活的 worker 续跑 60s, 观察窗口内不会自然退出, 断言稳定)
    ioc_.run_for(std::chrono::seconds(3));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// 整体关闭逆序分批 (纯函数): 策略 -> 交易 -> 行情 -> dzweb, 空批次不产生
TEST(ProcessShutdownBatches, ReverseOrderByCategory) {
    std::vector<std::pair<std::string, Category>> running{
        {"dzweb", Category::WebUI},     {"dzmd_ctp", Category::GatewayMd},
        {"stg_a", Category::Strategy},  {"dztd_ctp", Category::GatewayTd},
        {"stg_b", Category::Strategy},
    };
    auto batches = build_shutdown_batches(running);
    ASSERT_EQ(batches.size(), 4u);
    EXPECT_EQ(batches[0], (std::vector<std::string>{"stg_a", "stg_b"}));
    EXPECT_EQ(batches[1], (std::vector<std::string>{"dztd_ctp"}));
    EXPECT_EQ(batches[2], (std::vector<std::string>{"dzmd_ctp"}));
    EXPECT_EQ(batches[3], (std::vector<std::string>{"dzweb"}));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // worker 不读事件通道 -> REQUEST_SHUTDOWN 无效, 依赖批超时强制终止
    // (single_stop_timeout_sec 默认 3s), 全部退出后 children 清空
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(10));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// 整体关闭多类别逐批推进: 四类各一进程, 依赖每批超时强制终止后进入下一批,
// 全部退出后 children 清空 (验证批次间 timer 重建与推进链路)
TEST_F(ProcessSupervisorTest, ShutdownStopsAllCategoriesSequentially) {
    registry_.register_strategy(make_strategy_entry("stg_a", {"longrun", "60"}));
    registry_.register_gateway(make_internal_entry("dztd_ctp", Category::GatewayTd, {"longrun", "60"}));
    registry_.register_gateway(make_internal_entry("dzmd_ctp", Category::GatewayMd, {"longrun", "60"}));
    registry_.register_gateway(make_internal_entry("dzweb", Category::WebUI, {"longrun", "60"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    // single_stop_timeout_sec=1: 每批 1s 超时, 4 批最坏 4s, 测试可控
    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_, /*single_stop_timeout_sec=*/1);
    supervisor.start_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    supervisor.shutdown();
    EXPECT_TRUE(supervisor.is_shutting_down());

    // worker 不读事件通道 -> REQUEST_SHUTDOWN 无效, 依赖各批超时强制终止
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(12));
    EXPECT_EQ(supervisor.children().size(), 0u);

    orphan_guard_.cleanup();
}

// GatewayMd 进程退出时 master 代发 NOTIFY_MD_STOPPED (md 停止后果接线, 自检补测)
TEST_F(ProcessSupervisorTest, MdExitBroadcastsNotifyStopped) {
    registry_.register_gateway(make_internal_entry("dzmd_ctp", Category::GatewayMd, {"quick", "1"}));

    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    orphan_guard_.startup();

    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    supervisor.start_all();

    // 独立 reader 探测事件通道 (须在子进程退出前注册)
    shm::Reader reader = shm::Reader::create(
        shm::channel_name("dzevent"), dztrader::paths::shm(), "notify_stopped_probe");

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
    shm::MultiWriter writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "md_sim");
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
    shm::MultiWriter writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "md_sim");
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
