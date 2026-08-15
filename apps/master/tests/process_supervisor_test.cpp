/**
 * @file process_supervisor_test.cpp
 * @brief ProcessSupervisor 集成测试。
 */

#include "process_supervisor.h"

#include <dztrader/core/env.h>
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

}  // namespace
}  // namespace dztrader::master
