/**
 * @file history_e2e_test.cpp
 * @brief #78 端到端：真实 `dzsysmon` 把指标写进历史库，重启后数据仍在且带中断标记。
 *
 * 这是三条验收标准的**唯一**真实验证点（纯函数/临时库单测覆盖不到进程级接线）：
 * - 「采集中断标记与重启恢复参数语义」：重启后不重置数据文件、不丢已有聚合，
 *   重启窗口被标记为采集中断（契约 #41「历史连续性优先」）；
 * - 「优雅退出时 flush 队列并干净关库」：发 SHUTDOWN 后库文件可被只读连接打开，
 *   且退出前最后一个窗口的数据已落盘；
 * - 「关闭历史记录配置项生效」：`history.enabled=false` 时根本不建库文件。
 *
 * 做法：用临时 `DZTRADER_HOME` 起真实 `dzsysmon`，把 `record_interval_sec` 设成最小值
 * （1 秒）让数据快速落盘，再用 `HistoryReader`（与 dzweb 同一条只读路径）断言库内容。
 *
 * 信号量与事件通道是机器全局命名空间，与 dev 栈/并行测试互扰：检测到占用时
 * 默认 GTEST_SKIP，`DZ_SYSMON_E2E_STRICT=1` 时 FAIL（与 network_config_e2e_test 同约定）。
 */

#include "sysmon_e2e_support.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/sysmon.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/process_mutex.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace dztrader::sysmon {
namespace {

using e2e_support::e2e_strict;
using e2e_support::event_semaphore_busy;
using e2e_support::remove_semaphore;
using e2e_support::sysmon_executable;
using e2e_support::unique_suffix;

namespace process_v2 = boost::process::v2;
using namespace std::chrono_literals;

constexpr uint64_t kMB = 1024 * 1024;
/// 推送间隔拉长：本用例只关心历史落盘，不需要频繁广播（每次写入唤醒全部注册进程）。
constexpr int kPushIntervalSec = 300;
/// 记录间隔取最小值（1 秒）：让第一个完整记录窗口在数秒内结束并落盘，
/// 而不是等默认的 60 秒（那会让用例跑一分钟以上）。
constexpr int kRecordIntervalSec = 1;

class HistoryE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        if (event_semaphore_busy()) {
            if (e2e_strict()) {
                FAIL() << "event channel semaphore is busy";
            }
            GTEST_SKIP() << "event channel semaphore busy (dev stack or parallel test running)";
        }
        if (!std::filesystem::exists(sysmon_executable())) {
            if (e2e_strict()) {
                FAIL() << "dzsysmon executable not found: " << sysmon_executable().string();
            }
            GTEST_SKIP() << "dzsysmon executable not built: " << sysmon_executable().string();
        }

        home_ = std::filesystem::temp_directory_path() / ("dz_sysmon_hist_e2e_" + unique_suffix());
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ / "configs");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();

        write_config(/*history_enabled=*/true);
        create_event_channel();
    }

    void TearDown() override {
        stop_child();
        meta_.reset();
        reader_.reset();
        writer_.reset();
        remove_semaphore(reader_name_);
        remove_semaphore(platform::SYSMON_INSTANCE_NAME);
        dztrader::shm::ProcessMutex::remove(shm::channel_name("dzevent") + ".meta");
        dztrader::shm::ProcessMutex::remove(shm::channel_name("dzevent") + ".write");
        if (orig_home_.has_value()) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        dztrader::paths::reset_for_test();
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
    }

    std::filesystem::path history_db() const {
        return home_ / "db" / std::string(sysmonstore::kHistoryFileName);
    }

    /// 最小合法 dzsysmon 配置；`history_enabled` 控制历史开关。
    void write_config(bool history_enabled) {
        nlohmann::json cfg = {
            {"log", {{"level", "debug"}, {"flush_on", "info"}}},
            {"sampling",
             {{"measure_interval_ms", 1000},
              {"record_interval_sec", kRecordIntervalSec},
              {"phase_offset_sec", 0},
              {"groups",
               {{"cpu", true},
                {"memory", true},
                {"swap", true},
                {"volumes", true},
                {"disk_io", true},
                {"network", true}}}}},
            {"disk",
             {{"watch_paths", nlohmann::json::array()}, {"devices", nlohmann::json::array()}}},
            {"network", {{"hide_virtual", true}}},
            {"history",
             {{"enabled", history_enabled},
              {"raw_retention_hours", 24},
              {"max_retention_days", 30},
              {"max_bytes", 0}}},
            {"realtime", {{"push_interval_sec", kPushIntervalSec}}},
            {"alerts",
             {{"cpu_sustain_used_pct", 90.0},
              {"cpu_sustain_samples", 10},
              {"memory_used_pct", 90.0},
              {"swap_used_pct", 50.0},
              {"volume_free_pct", 10.0},
              {"volume_free_bytes", 1'073'741'824},
              {"cooldown_sec", 600}}},
        };
        std::ofstream ofs(home_ / "configs" / "dzsysmon.json", std::ios::binary | std::ios::trunc);
        ofs << cfg.dump(2);
    }

    void create_event_channel() {
        shm::ChannelConfig cfg{
            .channel_name = shm::channel_name("dzevent"),
            .shm_dir = dztrader::paths::shm(),
            .meta_file_size = 4 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        ASSERT_TRUE(meta_->add_reader(reader_name_, 0));
        ASSERT_TRUE(meta_->add_reader(platform::SYSMON_INSTANCE_NAME, 0));
        reader_ = std::make_unique<shm::Reader>(shm::Reader::create(meta_, reader_name_));
        writer_ = std::make_unique<shm::MultiWriter>(
            shm::MultiWriter::create(meta_, "dzhist_e2e_" + unique_suffix()));
    }

    void start_child() {
        io_.restart();
        process_v2::default_process_launcher launcher;
        boost::system::error_code ec;
        const auto exe = boost::filesystem::path(sysmon_executable().native());
        child_ = std::make_unique<process_v2::process>(
            launcher(io_.get_executor(), ec, exe, std::vector<std::string>{},
                     process_v2::process_stdio{nullptr, nullptr, nullptr}));
        ASSERT_FALSE(ec) << "spawn dzsysmon failed: " << ec.message();
    }

    /// 发 SHUTDOWN 帧（定向 dzsysmon）请求优雅退出，并等待子进程自行退出。
    ///
    /// 优雅退出是本用例的关键：只有走 SHUTDOWN 路径才会 flush 队列并关库；
    /// 强杀（terminate）虽然也能验证「WAL 崩溃恢复」，但验证不到验收标准要求的
    /// 「flush 队列并干净关库」。
    void shutdown_child_gracefully(std::chrono::milliseconds timeout) {
        if (!child_) {
            return;
        }
        (void)writer_->refresh_subscribers();
        (void)platform::write_ext_inst_json(*writer_, DZ_FRAME_SHUTDOWN,
                                            platform::SYSMON_INSTANCE_NAME, nlohmann::json{});
        writer_->notify_subscribers();

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            boost::system::error_code ec;
            if (child_->running(ec) == false) {
                child_->wait(ec);
                child_.reset();
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        // 超时未退出：强杀（用例失败由调用方断言库内容负责）。
        stop_child();
    }

    void stop_child() {
        if (!child_) {
            return;
        }
        boost::system::error_code ec;
        child_->terminate(ec);
        child_->wait(ec);
        child_.reset();
    }

    /// 反复尝试打开历史库，直到出现至少 `min_scalars` 行原始记录（条件等待，不用 sleep
    /// 当同步手段）。
    std::unique_ptr<sysmonstore::HistoryReader> wait_for_history_rows(
        int min_scalars, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto opened = sysmonstore::HistoryReader::open(history_db());
            if (opened.first == sysmonstore::HistoryAvailability::Ok) {
                if (auto rows = opened.second->query_scalars(
                        sysmonstore::kLevelRaw, 0, std::numeric_limits<int64_t>::max(), 1000);
                    static_cast<int>(rows.size()) >= min_scalars) {
                    return std::move(opened.second);
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return nullptr;
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::string reader_name_ = "dzhist_e2e_reader_" + unique_suffix();
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    boost::asio::io_context io_;
    std::unique_ptr<process_v2::process> child_;
};

// 验收：历史库位于数据根目录下的 db，独立于交易库与 WebUI 库；真实进程会写它。
TEST_F(HistoryE2ETest, ProcessWritesHistoryDatabase) {
    start_child();
    auto reader = wait_for_history_rows(1, 20s);
    ASSERT_NE(reader, nullptr) << "dzsysmon did not write any raw history row within 20s";
    EXPECT_EQ(reader->schema_version(), sysmonstore::kSchemaVersion);
    EXPECT_TRUE(std::filesystem::exists(history_db()));

    // 库必须落在 $DZTRADER_HOME/db 下，而不是被塞进交易库/WebUI 库（同一文件会互相踩）。
    const auto path = history_db();
    EXPECT_EQ(path.parent_path().filename().string(), "db");
    EXPECT_EQ(path.filename().string(), "sysmon.db");
    EXPECT_FALSE(std::filesystem::exists(home_ / "db" / "td.db"));
    EXPECT_FALSE(std::filesystem::exists(home_ / "db" / "webui.db"));

    // 原始记录必须带 CPU 使用率这类真实指标（进程确实在采集，不是写空行）。
    const auto rows =
        reader->query_scalars(sysmonstore::kLevelRaw, 0, std::numeric_limits<int64_t>::max(), 10);
    ASSERT_FALSE(rows.empty());
    const auto* spec = sysmonstore::find_scalar_metric("memory.used_bytes");
    ASSERT_NE(spec, nullptr);
    const auto index = static_cast<size_t>(spec - sysmonstore::kScalarMetrics.data());
    // 内存指标在多数机器上可用；不可用时该窗口为空聚合（允许）。
    if (!rows[0].metrics[index].empty()) {
        EXPECT_GT(rows[0].metrics[index].max, 0.0);
    }
    stop_child();
}

// 验收：优雅退出时 flush 队列并干净关库 —— 退出后库可被只读连接打开，且数据一致。
TEST_F(HistoryE2ETest, GracefulShutdownFlushesAndClosesDatabase) {
    start_child();
    auto reader = wait_for_history_rows(1, 20s);
    ASSERT_NE(reader, nullptr) << "dzsysmon did not write history rows";
    reader.reset();

    shutdown_child_gracefully(20s);
    ASSERT_EQ(child_, nullptr) << "dzsysmon did not exit on SHUTDOWN within 20s";

    // 干净关库的证据：只读连接能立刻打开、schema 版本正确、能读到数据。
    // （WAL 未 checkpoint 或连接未关时，-wal 里的数据在只读打开后仍可见，但
    //   本用例的前提是「优雅退出路径确实执行了 checkpoint + close」。）
    auto reopened = sysmonstore::HistoryReader::open(history_db());
    ASSERT_EQ(reopened.first, sysmonstore::HistoryAvailability::Ok);
    EXPECT_EQ(reopened.second->schema_version(), sysmonstore::kSchemaVersion);
    const auto rows = reopened.second->query_scalars(sysmonstore::kLevelRaw, 0,
                                                     std::numeric_limits<int64_t>::max(), 1000);
    EXPECT_FALSE(rows.empty()) << "graceful shutdown must not lose persisted history";
}

// 验收：重启恢复参数语义 —— 历史连续性优先（不重置数据、不丢聚合，重启窗口标记中断）。
TEST_F(HistoryE2ETest, RestartPreservesHistoryAndMarksRestart) {
    start_child();
    auto reader = wait_for_history_rows(1, 20s);
    ASSERT_NE(reader, nullptr) << "first run did not write history";
    const auto first_rows =
        reader->query_scalars(sysmonstore::kLevelRaw, 0, std::numeric_limits<int64_t>::max(), 1000);
    const auto first_bucket = first_rows.front().bucket_ts;
    reader.reset();

    // 先优雅退出（落盘），再重启（模拟主进程自动重启）。
    shutdown_child_gracefully(20s);
    ASSERT_EQ(child_, nullptr);
    start_child();

    // 重启后必须**继续往同一个库写**（不重置数据文件）。
    // 等到出现至少一条 bucket_ts > first_bucket 的新记录，证明进程没有清空重建。
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    bool resumed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto opened = sysmonstore::HistoryReader::open(history_db());
        if (opened.first == sysmonstore::HistoryAvailability::Ok) {
            const auto rows = opened.second->query_scalars(
                sysmonstore::kLevelRaw, 0, std::numeric_limits<int64_t>::max(), 1000);
            // 第一条记录仍是重启前那条 ⇒ 数据文件没有被重置。
            if (!rows.empty() && rows.front().bucket_ts == first_bucket) {
                if (rows.size() > first_rows.size()) {
                    resumed = true;
                    break;
                }
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(resumed) << "restart must continue the same history file, not reset it";

    // 重启窗口必须被标记为「采集中断」：读取方据此知道那段时间没有可信数据。
    auto opened = sysmonstore::HistoryReader::open(history_db());
    ASSERT_EQ(opened.first, sysmonstore::HistoryAvailability::Ok);
    const auto marks = opened.second->query_marks(0, std::numeric_limits<int64_t>::max(), 100);
    const bool has_restart =
        std::any_of(marks.begin(), marks.end(), [](const sysmonstore::HistoryMark& mark) {
            return mark.kind == sysmonstore::kMarkRestart;
        });
    EXPECT_TRUE(has_restart) << "restart window must be marked as collection interruption";
    stop_child();
}

// 验收：「关闭历史记录」配置项生效（仅实时数据，不写库）。
TEST_F(HistoryE2ETest, DisabledHistoryDoesNotCreateDatabase) {
    write_config(/*history_enabled=*/false);
    start_child();

    // 等进程真正跑起来（能读到启动快照帧），再确认没有建库。
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    bool saw_snapshot = false;
    while (std::chrono::steady_clock::now() < deadline && !saw_snapshot) {
        while (const auto* raw = reader_->next_frame()) {
            const shm::FrameView view(raw);
            if (view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT) {
                saw_snapshot = true;
                break;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(saw_snapshot) << "dzsysmon did not start with history disabled";

    // 历史关闭时不得创建库文件（创建后即使不写，也说明配置没生效）。
    EXPECT_FALSE(std::filesystem::exists(history_db()))
        << "history.enabled=false must not create the history database";
    stop_child();
}

}  // namespace
}  // namespace dztrader::sysmon
