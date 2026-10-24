/**
 * @file alert_e2e_test.cpp
 * @brief #80 端到端：阈值告警的触发 / 恢复经 `NOTIFY_UI` 与快照 `alerts[]` 对外可见。
 *
 * 纯函数单测（alerts_test）已经覆盖判定、冷却与恢复的语义；本用例补的是**进程接线**：
 * `dzsysmon` 真的把状态机结果发成 `DZ_FRAME_NOTIFY_UI`（触发 error+popup、恢复 info+不弹），
 * 并把活动告警投影进快照 `alerts[]`（启动即越限 / 配置改阈值后恢复）。
 *
 * 做法：起一个真实 `dzsysmon` 进程（临时 DZTRADER_HOME + 真实事件通道）：
 * 1. 配置 `alerts.memory_used_pct = 0`（必然越限）→ 启动首帧快照应携带 memory_used 活动告警，
 *    并收到一条 error+popup 的 NOTIFY_UI；
 * 2. 发 `SET_SYSMON_CONFIG` 把阈值改成 100% → 下一个计量 tick 恢复，收到 info 通知；
 * 3. `QUERY_FULL_SNAPSHOT` 触发重推快照 → `alerts[]` 应为空（活动状态已清除）。
 *
 * 信号量与事件通道是机器全局命名空间，与 dev 栈/并行测试互扰：检测到占用时默认
 * GTEST_SKIP，`DZ_SYSMON_E2E_STRICT=1` 时 FAIL（与既有 sysmon e2e 同约定）。该用例 RUN_SERIAL。
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

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
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
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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
/// 周期推送间隔设得远大于本用例等待窗口：测试期间出现的快照只能来自启动/查询触发，
/// 避免周期推送干扰帧序断言。
constexpr int kPushIntervalSec = 300;

class AlertE2ETest : public ::testing::Test {
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

        home_ = std::filesystem::temp_directory_path() / ("dz_sysmon_alert_e2e_" + unique_suffix());
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ / "configs");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();

        write_config();
        create_event_channel();
    }

    void TearDown() override {
        stop_child();
        reader_.reset();
        meta_.reset();
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

    /// 最小合法 dzsysmon 配置：内存阈值设 0（必然告警），其余告警阈值调到不触发，
    /// 避免依赖本机交换/卷剩余空间的真实状态；历史关闭、推送间隔拉长。
    void write_config() {
        nlohmann::json cfg = {
            {"log", {{"level", "debug"}, {"flush_on", "info"}}},
            {"sampling",
             {{"measure_interval_ms", 1000},
              {"record_interval_sec", 60},
              {"phase_offset_sec", 3},
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
             {{"enabled", false},
              {"raw_retention_hours", 24},
              {"max_retention_days", 30},
              {"max_bytes", 0}}},
            {"realtime", {{"push_interval_sec", kPushIntervalSec}}},
            {"alerts",
             {{"cpu_sustain_used_pct", 90.0},
              {"cpu_sustain_samples", 10},
              {"memory_used_pct", 0.0},
              {"swap_used_pct", 100.0},
              {"volume_free_pct", 0.0},
              {"volume_free_bytes", 0},
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
            shm::MultiWriter::create(meta_, "dzweb_alert_e2e_" + unique_suffix()));
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

    void stop_child() {
        if (!child_) {
            return;
        }
        boost::system::error_code ec;
        child_->terminate(ec);
        child_->wait(ec);
        child_.reset();
    }

    /// 写一帧 SET_SYSMON_CONFIG（定向 dzsysmon）并唤醒订阅者。
    bool send_set_config(const nlohmann::json& patch) {
        (void)writer_->refresh_subscribers();
        if (!platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_SYSMON_CONFIG,
                                               platform::SYSMON_INSTANCE_NAME, patch)) {
            return false;
        }
        writer_->notify_subscribers();
        return true;
    }

    /// 广播 QUERY_FULL_SNAPSHOT（无 instance_id，空 payload）触发一次完整上报。
    bool query_full_snapshot() {
        (void)writer_->refresh_subscribers();
        if (!writer_->write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0)) {
            return false;
        }
        writer_->notify_subscribers();
        return true;
    }

    /// 从帧视图取 JSON payload（快照 / NOTIFY_UI 为无 instance_id 帧，配置帧带 instance_id）。
    static nlohmann::json payload_of(const shm::FrameView& view) {
        const bool broadcast =
            view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT || view.type() == DZ_FRAME_NOTIFY_UI;
        const auto size = broadcast ? view.ext_payload_size() : view.ext_inst_payload_size();
        const auto* data = broadcast ? view.ext_payload() : view.ext_inst_payload();
        return nlohmann::json::parse(reinterpret_cast<const char*>(data),
                                     reinterpret_cast<const char*>(data) + size);
    }

    /// 在窗口内等指定类型的帧，**保留**窗口内读到的其它相关类型帧到 `pending_`。
    std::optional<nlohmann::json> wait_for_frame(DzFrameType type,
                                                 std::chrono::milliseconds timeout) {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->first == type) {
                const auto payload = it->second;
                pending_.erase(it);
                return payload;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            while (const auto* raw = reader_->next_frame()) {
                const shm::FrameView view(raw);
                if (view.type() == type) {
                    return payload_of(view);
                }
                if (view.type() == DZ_FRAME_RTN_SYSMON_CONFIG ||
                    view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT ||
                    view.type() == DZ_FRAME_NOTIFY_UI) {
                    pending_.emplace_back(view.type(), payload_of(view));
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::string reader_name_ = "dzmon_alert_e2e_" + unique_suffix();
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    boost::asio::io_context io_;
    std::unique_ptr<process_v2::process> child_;
    std::vector<std::pair<DzFrameType, nlohmann::json>> pending_;
};

// 契约 sysmon §2.8：启动即越限 → 首帧快照携带活动告警 + 一条 error+popup 通知；
// 改阈值恢复 → info 通知 + 重推快照的活动告警清空。
TEST_F(AlertE2ETest, ThresholdAlertTriggersNotificationAndClearsAfterRecovery) {
    start_child();

    // 1) 启动首帧（触发 1）：活动告警已进 alerts[]（init 阶段评估过一次）。
    const auto first = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s);
    ASSERT_TRUE(first.has_value()) << "dzsysmon did not push the startup snapshot";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*first), std::nullopt);
    const auto& startup_alerts = (*first)["alerts"];
    ASSERT_TRUE(startup_alerts.is_array());
    const bool startup_has_memory_alert =
        std::any_of(startup_alerts.begin(), startup_alerts.end(), [](const nlohmann::json& alert) {
            return alert.value("kind", "") == platform::SYSMON_ALERT_MEMORY_USED;
        });
    EXPECT_TRUE(startup_has_memory_alert)
        << "startup snapshot must expose the active memory alert (contract sysmon §2.8)";

    // 2) 触发通知：error + popup=true（只有 error+popup 会打断用户，契约 notify-ui）。
    const auto trigger = wait_for_frame(DZ_FRAME_NOTIFY_UI, 10s);
    ASSERT_TRUE(trigger.has_value()) << "no NOTIFY_UI frame for the threshold alert";
    EXPECT_EQ((*trigger)["source"], platform::SYSMON_INSTANCE_NAME);
    EXPECT_EQ((*trigger)["level"], "error");
    EXPECT_EQ((*trigger)["popup"], true);
    const std::string trigger_message = (*trigger)["message"].get<std::string>();
    EXPECT_NE(trigger_message.find("内存"), std::string::npos);

    // 3) 把内存阈值提到 100%（只有内存全满才越限）→ 下一计量 tick 恢复。
    //    丢弃启动阶段残留的 RTN_SYSMON_CONFIG（启动推送的旧值 0 会被下面的等待命中）。
    pending_.clear();
    ASSERT_TRUE(send_set_config({{"alerts", {{"memory_used_pct", 100.0}}}}));
    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(config.has_value()) << "SET must roll the effective config back to the client";
    EXPECT_DOUBLE_EQ((*config)["alerts"]["memory_used_pct"].get<double>(), 100.0);

    const auto recovery = wait_for_frame(DZ_FRAME_NOTIFY_UI, 10s);
    ASSERT_TRUE(recovery.has_value()) << "no recovery NOTIFY_UI after the threshold was raised";
    EXPECT_EQ((*recovery)["level"], "info");
    EXPECT_EQ((*recovery)["popup"], false);
    const std::string recovery_message = (*recovery)["message"].get<std::string>();
    EXPECT_NE(recovery_message.find("内存"), std::string::npos);

    // 4) 全量快照查询：活动告警已清除（恢复不是只发通知、状态却不清）。
    //    丢弃 SET 成功后立即推送的旧快照（它早于恢复 tick，仍带活动告警）。
    pending_.clear();
    ASSERT_TRUE(query_full_snapshot());
    const auto after_recovery = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 10s);
    ASSERT_TRUE(after_recovery.has_value()) << "QUERY_FULL_SNAPSHOT must re-push the snapshot";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*after_recovery), std::nullopt);
    EXPECT_TRUE((*after_recovery)["alerts"].empty())
        << "recovered alert must leave the active alert set (contract sysmon §2.8)";
}

}  // namespace
}  // namespace dztrader::sysmon
