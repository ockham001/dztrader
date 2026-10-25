/**
 * @file alert_e2e_test.cpp
 * @brief #123 端到端：资源越限降级为日志后，**不再产生任何 `NOTIFY_UI`**。
 *
 * 纯函数单测（`thresholds_test`）已经覆盖进入/重记/恢复的完整语义与日志记录内容；
 * 本用例补的是**进程接线与外部承诺**：真实 `dzsysmon` 在内存持续越限的情况下，事件通道上
 * 一条资源类 `NOTIFY_UI` 都不出现，快照也不携带已删除的 `alerts` 字段。
 *
 * **不断言日志文件内容**：日志 sink 的 flush 策略与按日滚动使文件读取脆弱，日志内容由单测
 * 覆盖；这里只验证「不弹窗」这个用户可感知的承诺（ADR 0034）。
 *
 * 做法：起一个真实 `dzsysmon` 进程（临时 DZTRADER_HOME + 真实事件通道）：
 * 1. 配置 `thresholds.memory_used_pct = 0`（必然越限）→ 启动后观察一段时间的事件通道：
 *    必须收到快照，但**收不到** `NOTIFY_UI`；
 * 2. 发 `SET_SYSMON_CONFIG` 改阈值（触发一个真实的状态跃迁）→ 仍然收不到 `NOTIFY_UI`，
 *    且回推的配置里 `thresholds.memory_used_pct` 已生效；
 * 3. `QUERY_FULL_SNAPSHOT` 重推快照 → 快照不含 `alerts` 字段（#125 已删除；发送方义务）。
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
#include <boost/process/v2/process.hpp>
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

using test::e2e_strict;
using test::event_semaphore_busy;
using test::remove_semaphore;
using test::sysmon_executable;
using test::unique_suffix;

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
            {"disk", {{"devices", nlohmann::json::array()}}},
            {"network", {{"hide_virtual", true}}},
            {"history",
             {{"enabled", false},
              {"raw_retention_hours", 24},
              {"max_retention_days", 30},
              {"max_bytes", 0}}},
            {"realtime", {{"push_interval_sec", kPushIntervalSec}}},
            {"thresholds",
             {{"cpu_sustain_used_pct", 90.0},
              {"cpu_sustain_samples", 10},
              {"memory_used_pct", 0.0},
              {"swap_used_pct", 100.0},
              {"volume_free_pct", 0.0},
              {"log_repeat_sec", 600}}},
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
        boost::system::error_code ec;
        child_ = test::spawn_dzsysmon(io_, ec);
        ASSERT_FALSE(ec) << "spawn dzsysmon failed: " << ec.message();
    }

    void stop_child() { test::stop_dzsysmon(child_); }

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

// 契约 sysmon §2.8 / ADR 0034：资源越限只写日志，**不产生任何 NOTIFY_UI**。
//
// 这是本票最重要的外部承诺：弹窗通道必须只剩真故障。内存阈值配成 0（必然越限）仍然
// 一条通知都不该出现——旧实现会在这里发 error+popup 并弹模态框打断盯盘。
TEST_F(AlertE2ETest, ResourceThresholdBreachNeverProducesNotifyUi) {
    start_child();

    // 1) 启动首帧（契约 sysmon §4 触发 1）：快照必须到达且合法——"不发通知"不等于"没在跑"。
    const auto first = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s);
    ASSERT_TRUE(first.has_value()) << "dzsysmon did not push the startup snapshot";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*first), std::nullopt);
    // 启动即越限（阈值 0），但快照不含已删除的 alerts 字段（#125；状态只在日志里）。
    EXPECT_FALSE((*first).contains("alerts")) << "snapshot must not carry the removed alerts field";

    // 2) 内存持续越限跨过多个计量 tick：期间不得出现任何 NOTIFY_UI。
    //    等待窗口显著长于 measure_interval_ms，确保旧实现的"启动即弹"必然被捕获。
    EXPECT_FALSE(wait_for_frame(DZ_FRAME_NOTIFY_UI, 3s).has_value())
        << "resource threshold breach produced a NOTIFY_UI frame (must be log-only)";

    // 3) 触发一次真实的状态跃迁：把内存阈值提到 100%（只有内存全满才越限）。
    //    旧实现在这里会发一条 info 的"恢复"通知；新实现必须安静。
    pending_.clear();
    ASSERT_TRUE(send_set_config({{"thresholds", {{"memory_used_pct", 100.0}}}}));
    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(config.has_value()) << "SET must roll the effective config back to the client";
    EXPECT_DOUBLE_EQ((*config)["thresholds"]["memory_used_pct"].get<double>(), 100.0);
    // 未提及的键保持默认（merge 不是整体替换）。
    EXPECT_EQ((*config)["thresholds"]["log_repeat_sec"], 600);

    EXPECT_FALSE(wait_for_frame(DZ_FRAME_NOTIFY_UI, 3s).has_value())
        << "threshold recovery produced a NOTIFY_UI frame (must be log-only)";

    // 4) 全量快照查询：仍不含已删除的 alerts 字段。
    pending_.clear();
    ASSERT_TRUE(query_full_snapshot());
    const auto after = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 10s);
    ASSERT_TRUE(after.has_value()) << "QUERY_FULL_SNAPSHOT must re-push the snapshot";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*after), std::nullopt);
    EXPECT_FALSE((*after).contains("alerts"));
}

// 旧格式配置（`alerts` 段）必须能启动并迁移为新形状：现网配置文件直接报错会让进程起不来。
// 这里验证的是**进程级**接线（迁移在 load_config 内、写回文件后进程照常运行）；
// 迁移的键级语义（哪些映射、哪些丢弃、部分键、未知键）由 config_test 覆盖。
TEST_F(AlertE2ETest, LegacyAlertsSectionIsMigratedAndProcessStarts) {
    // 用旧形状覆盖启动前写的配置（超集：旧段 + 已删除的 volume_free_bytes/cooldown_sec）。
    nlohmann::json legacy;
    {
        std::ifstream ifs(home_ / "configs" / "dzsysmon.json", std::ios::binary);
        ifs >> legacy;
    }
    ASSERT_TRUE(legacy.contains("thresholds"));
    legacy["alerts"] = {{"cpu_sustain_used_pct", 91.0}, {"cpu_sustain_samples", 7},
                        {"memory_used_pct", 0.0},       {"swap_used_pct", 100.0},
                        {"volume_free_pct", 0.0},       {"volume_free_bytes", 1'073'741'824},
                        {"cooldown_sec", 600}};
    legacy.erase("thresholds");
    {
        std::ofstream ofs(home_ / "configs" / "dzsysmon.json", std::ios::binary | std::ios::trunc);
        ofs << legacy.dump(2);
    }

    start_child();

    // 进程正常启动（迁移不报错、不阻塞）：回推的生效配置是**新形状**且值已搬过来。
    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 15s);
    ASSERT_TRUE(config.has_value()) << "dzsysmon did not come up with a legacy config file";
    EXPECT_FALSE((*config).contains("alerts")) << "legacy section must not survive in RTN payload";
    ASSERT_TRUE((*config).contains("thresholds"));
    EXPECT_DOUBLE_EQ((*config)["thresholds"]["cpu_sustain_used_pct"].get<double>(), 91.0);
    EXPECT_EQ((*config)["thresholds"]["cpu_sustain_samples"], 7);
    // 已删除的键不会出现在新段里。
    EXPECT_FALSE((*config)["thresholds"].contains("volume_free_bytes"));
    EXPECT_FALSE((*config)["thresholds"].contains("cooldown_sec"));

    // 文件也被改写为新形状：旧段消失、新段落盘（下次启动不再重复迁移）。
    nlohmann::json persisted;
    {
        std::ifstream ifs(home_ / "configs" / "dzsysmon.json", std::ios::binary);
        ifs >> persisted;
    }
    EXPECT_FALSE(persisted.contains("alerts")) << "migration must write the new shape back";
    ASSERT_TRUE(persisted.contains("thresholds"));
    EXPECT_DOUBLE_EQ(persisted["thresholds"]["cpu_sustain_used_pct"].get<double>(), 91.0);
    // log section 与其它段必须原样保留（迁移不是重写整份文件）。
    EXPECT_TRUE(persisted.contains("log"));
    EXPECT_TRUE(persisted.contains("sampling"));

    // 迁移后照常推送快照（进程健康），且仍然不发资源类通知。
    EXPECT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 10s).has_value());
    EXPECT_FALSE(wait_for_frame(DZ_FRAME_NOTIFY_UI, 2s).has_value());
}

}  // namespace
}  // namespace dztrader::sysmon
