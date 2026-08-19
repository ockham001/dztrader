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
#include <thread>
#include <memory>

namespace dztrader::master {
namespace {

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
        tmp_dir_ = std::filesystem::temp_directory_path() / "dz_supervisor_test";
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

}  // namespace
}  // namespace dztrader::master
