/**
 * @file config_e2e_test.cpp
 * @brief #81 端到端：运行期配置的六段全量生效 / 间隔即时重排 / 组关闭 / history 运行期开关。
 *
 * 单测已覆盖快照构造（snapshot_builder_test）与配置边界（platform sysmon_test）的纯逻辑；
 * 本文件补的是**进程接线**，这些语义由 `SysmonApp::apply_sysmon_config` 的副作用顺序决定，
 * 纯函数测试观测不到：
 * 1. 一次 patch 覆盖六个配置段 → RTN 回推全量生效值、未提及字段保持默认，且成功后立即推送
 *    的快照已体现新配置（契约 sysmon §3.1.1、§4 触发 4）；
 * 2. `realtime.push_interval_sec` 运行期修改后**立即**按新间隔重排推送定时器，不等旧周期结束
 *    （契约 sysmon §3.1.1 第 2 步）；
 * 3. `sampling.groups.<key>=false` 时对应可用性为 `disabled_by_config`、数值为 null，且实时
 *    快照仍按周期到达（关闭一类指标不影响其它链路，契约 sysmon §3.1.1）；
 * 4. `history.enabled` 运行期 false→true 会建库（且**不**写 restart 标记——进程没有重启）、
 *    true→false 会 flush 并关库，两者都不影响实时快照推送（契约 sysmon §3.1.1 第 5 步）。
 *
 * 做法：起一个真实 `dzsysmon` 进程（临时 DZTRADER_HOME + 真实事件通道），用独立 reader
 * 观察帧序；每个断言都以「条件等待 + 超时即失败」表达，不用固定 sleep 当同步手段。
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
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

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
#include <functional>
#include <limits>
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
/// 启动配置把推送间隔拉得很长：这样「SET 之后短窗口内出现快照」只能归因于
/// §4 触发 4 的立即推送，而不是周期推送蒙混过关。
constexpr int kLongPushIntervalSec = 300;

class ConfigE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        if (event_semaphore_busy()) {
            // 机器全局信号量被 dev 栈/其它测试实例占用：真实进程会互相唤醒错帧。
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

        home_ = std::filesystem::temp_directory_path() / ("dz_sysmon_cfg_e2e_" + unique_suffix());
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ / "configs");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();

        write_config();
        create_event_channel();
    }

    void TearDown() override {
        // 先停子进程，再释放通道映射，最后删目录（Windows 上已映射文件删不掉）
        stop_child();
        reader_.reset();
        meta_.reset();
        writer_.reset();
        // 命名信号量是机器全局资源：不 unlink 会在 /dev/shm 里随每次运行累积。
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

    /// 最小合法 dzsysmon 配置：只参数化本文件用到的启动值，其余取契约默认值。
    void write_config(bool history_enabled = true,
                      int push_interval_sec = kLongPushIntervalSec,
                      int measure_interval_ms = 1000) {
        nlohmann::json cfg = {
            {"log", {{"level", "debug"}, {"flush_on", "info"}}},
            {"sampling",
             {{"measure_interval_ms", measure_interval_ms},
              {"record_interval_sec", 60},
              {"phase_offset_sec", 3},
              {"groups",
               {{"cpu", true},
                {"memory", true},
                {"swap", true},
                {"volumes", true},
                {"disk_io", true},
                {"network", true},
                {"temperature", true},
                {"cpu_frequency", true}}}}},
            {"disk",
             {{"watch_paths", nlohmann::json::array()}, {"devices", nlohmann::json::array()}}},
            {"network", {{"hide_virtual", true}}},
            {"history",
             {{"enabled", history_enabled},
              {"raw_retention_hours", 24},
              {"max_retention_days", 30},
              {"max_bytes", 0}}},
            {"realtime", {{"push_interval_sec", push_interval_sec}}},
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
        // reader 必须先注册：MultiWriter 构造时同步订阅者缓存，否则 notify 唤不醒本 reader。
        ASSERT_TRUE(meta_->add_reader(reader_name_, 0));
        // 生产链路由 master 的 register_subscriber(entry.name) 注册子进程；本用例自建通道，
        // 所以必须自己把 dzsysmon 登记为订阅者，否则 SET_SYSMON_CONFIG 到不了目标进程。
        ASSERT_TRUE(meta_->add_reader(platform::SYSMON_INSTANCE_NAME, 0));
        reader_ = std::make_unique<shm::Reader>(shm::Reader::create(meta_, reader_name_));
        writer_ = std::make_unique<shm::MultiWriter>(
            shm::MultiWriter::create(meta_, "dzcfg_e2e_" + unique_suffix()));
    }

    void start_child() {
        boost::system::error_code ec;
        child_ = test::spawn_dzsysmon(io_, ec);
        ASSERT_FALSE(ec) << "spawn dzsysmon failed: " << ec.message();
    }

    void stop_child() { test::stop_dzsysmon(child_); }

    /// 写一帧 SET_SYSMON_CONFIG（定向 dzsysmon）并唤醒订阅者。
    bool send_set_config(const nlohmann::json& patch) {
        // 每次发送前刷新订阅者缓存：dzsysmon 可能在上一帧之后才注册完成。
        (void)writer_->refresh_subscribers();
        const bool ok = platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_SYSMON_CONFIG,
                                                          platform::SYSMON_INSTANCE_NAME, patch);
        if (!ok) {
            return false;
        }
        writer_->notify_subscribers();
        return true;
    }

    /// 从帧视图按帧头类型取 payload（快照无 instance_id，配置帧有）。
    static nlohmann::json payload_of(const shm::FrameView& view) {
        const bool broadcast = view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT;
        const auto size = broadcast ? view.ext_payload_size() : view.ext_inst_payload_size();
        const auto* data = broadcast ? view.ext_payload() : view.ext_inst_payload();
        return nlohmann::json::parse(reinterpret_cast<const char*>(data),
                                     reinterpret_cast<const char*>(data) + size);
    }

    /// 在窗口内等指定类型的帧，**保留**窗口内读到的其它 JSON 帧到 `pending_`。
    ///
    /// 保留是必需的：一次 SET 会先写 RTN_SYSMON_CONFIG 再写 RTN_SYSMON_SNAPSHOT，
    /// 若等待快照时直接丢弃配置帧，后面就再也读不到它（reader 是单向游标）。
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
                // 只缓存本用例关心的 JSON 帧：通道上还有其它二进制/控制帧，parse 会抛。
                if (view.type() == DZ_FRAME_RTN_SYSMON_CONFIG ||
                    view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT) {
                    pending_.emplace_back(view.type(), payload_of(view));
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    /// 等第一帧满足 `predicate` 的快照（条件等待 + 超时即失败）。
    ///
    /// 不匹配的快照直接丢弃：旧状态的快照对「最新状态」的断言没有价值，留着只会让
    /// 后续等待命中过期数据。RTN_SYSMON_CONFIG 仍保留在 `pending_`（后续用例要读）。
    std::optional<nlohmann::json> wait_for_snapshot(
        const std::function<bool(const nlohmann::json&)>& predicate,
        std::chrono::milliseconds timeout) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->first == DZ_FRAME_RTN_SYSMON_SNAPSHOT) {
                if (predicate(it->second)) {
                    auto payload = it->second;
                    pending_.erase(it);
                    return payload;
                }
                it = pending_.erase(it);
                continue;
            }
            ++it;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            while (const auto* raw = reader_->next_frame()) {
                const shm::FrameView view(raw);
                if (view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT) {
                    auto payload = payload_of(view);
                    if (predicate(payload)) {
                        return payload;
                    }
                    continue;
                }
                if (view.type() == DZ_FRAME_RTN_SYSMON_CONFIG) {
                    pending_.emplace_back(view.type(), payload_of(view));
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    std::filesystem::path history_db() const {
        return home_ / "db" / std::string(sysmonstore::kHistoryFileName);
    }

    /// 条件等待文件出现（不用固定 sleep 当同步手段）。
    static bool wait_for_file(const std::filesystem::path& path,
                              std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(path)) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::filesystem::exists(path);
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::string reader_name_ = "dzmon_cfg_e2e_reader_" + unique_suffix();
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    boost::asio::io_context io_;
    std::unique_ptr<process_v2::process> child_;
    /// 已读到但本次等待不需要的帧（按到达顺序保留，供后续 wait_for_frame 消费）。
    std::vector<std::pair<DzFrameType, nlohmann::json>> pending_;
};

// 契约 sysmon §3.1.1：一次 patch 覆盖六个段，全部生效；未提及字段保持默认；
// 成功后立即推送的快照已体现新配置（§4 触发 4）。
TEST_F(ConfigE2ETest, SetConfigAppliesAllSectionsAndPushesImmediateSnapshot) {
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value())
        << "dzsysmon did not push the startup snapshot";
    pending_.clear();  // 丢弃启动期残留帧（旧值会满足后续断言）

    const std::string watch_path = home_.string();
    const auto patch = nlohmann::json{
        {"sampling",
         {{"measure_interval_ms", 500},
          {"record_interval_sec", 2},
          {"phase_offset_sec", 1},
          {"groups", {{"network", false}}}}},
        {"disk",
         {{"watch_paths", nlohmann::json::array({watch_path})},
          {"devices", nlohmann::json::array({"sda"})}}},
        {"network", {{"hide_virtual", false}}},
        {"history", {{"enabled", false}}},
        {"realtime", {{"push_interval_sec", 2}}},
        {"alerts", {{"memory_used_pct", 42.0}}},
    };
    ASSERT_TRUE(send_set_config(patch));

    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(config.has_value()) << "SET must roll the effective config back to the client";
    // 六个段的新值全部生效。
    EXPECT_EQ((*config)["sampling"]["measure_interval_ms"], 500);
    EXPECT_EQ((*config)["sampling"]["record_interval_sec"], 2);
    EXPECT_EQ((*config)["sampling"]["phase_offset_sec"], 1);
    EXPECT_EQ((*config)["sampling"]["groups"]["network"], false);
    EXPECT_EQ((*config)["disk"]["watch_paths"], nlohmann::json::array({watch_path}));
    EXPECT_EQ((*config)["disk"]["devices"], nlohmann::json::array({"sda"}));
    EXPECT_EQ((*config)["network"]["hide_virtual"], false);
    EXPECT_EQ((*config)["history"]["enabled"], false);
    EXPECT_EQ((*config)["realtime"]["push_interval_sec"], 2);
    EXPECT_DOUBLE_EQ((*config)["alerts"]["memory_used_pct"].get<double>(), 42.0);
    // 未提及字段保持默认（patch 是 merge，不是整体替换）。
    EXPECT_EQ((*config)["sampling"]["groups"]["cpu"], true);
    EXPECT_EQ((*config)["alerts"]["cpu_sustain_samples"], 10);
    EXPECT_EQ((*config)["history"]["raw_retention_hours"], 24);
    EXPECT_EQ((*config)["alerts"]["volume_free_bytes"].get<uint64_t>(), 1'073'741'824ULL);

    // SET 成功后立即推送的快照必须已体现新配置。推送间隔为 2s（刚从 300s 重排），
    // 因此 1.5s 内到达的快照只能是 §4 触发 4 的立即推送，不是周期推送。
    const auto snapshot = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 1500ms);
    ASSERT_TRUE(snapshot.has_value())
        << "SET_SYSMON_CONFIG did not trigger an immediate snapshot push (contract sysmon §4 "
           "trigger 4)";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*snapshot), std::nullopt);
    EXPECT_EQ((*snapshot)["sampling"]["measure_interval_ms"], 500);
    // disk.devices 的过滤在快照构造阶段完成：本次立即推送即可体现新范围，
    // 已采集到的设备中只有精确匹配 `sda` 的才会出现在 disk_io[]。
    const auto& disk_io = (*snapshot)["disk_io"];
    if (disk_io.is_array()) {
        for (const auto& disk : disk_io) {
            EXPECT_EQ(disk["device"], "sda");
        }
    }
}

// 契约 sysmon §3.1.1 第 2 步：推送间隔运行期修改后立即重排，不等旧周期（300s）结束。
TEST_F(ConfigE2ETest, PushIntervalChangeReschedulesTimerImmediately) {
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value());
    pending_.clear();

    // 启动间隔 300s：SET 之后 5s 内出现的任何快照都只能来自重排/立即推送。
    ASSERT_TRUE(send_set_config({{"realtime", {{"push_interval_sec", 1}}}}));

    const auto immediate = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 5s);
    ASSERT_TRUE(immediate.has_value())
        << "SET did not trigger an immediate snapshot push (contract sysmon §4 trigger 4)";

    // 第二帧必须是重排后的 1s 周期推送：若定时器仍等旧 300s，这里 2.5s 内不可能有帧。
    const auto periodic = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 2500ms);
    ASSERT_TRUE(periodic.has_value())
        << "push timer was not rescheduled to the new interval immediately (contract sysmon "
           "§3.1.1 step 2)";
    // 用快照自带的生成时刻判断两帧间隔（而不是测试线程的读取时刻——两帧可能已同时缓冲，
    // 读到的先后顺序不与生成时刻成正比）。新间隔 1s：间隔应在一个周期左右。
    const auto interval_ms =
        (*periodic)["timestamp"].get<int64_t>() - (*immediate)["timestamp"].get<int64_t>();
    EXPECT_GE(interval_ms, 400);
    EXPECT_LE(interval_ms, 2500);
    EXPECT_EQ(platform::SysmonSnapshot::validate(*periodic), std::nullopt);
}

// 契约 sysmon §3.1.1：组关闭后可用性为 disabled_by_config、数值为 null；
// 实时链路其余部分照常（快照继续按周期到达）。
TEST_F(ConfigE2ETest, DisabledGroupKeepsRealtimeStreamAndReportsDisabledByConfig) {
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value());
    pending_.clear();

    // 计量 200ms、推送 1s：关闭 memory 组后最多一个推送周期即可在周期快照上看到效果。
    ASSERT_TRUE(send_set_config(
        {{"sampling", {{"measure_interval_ms", 200}, {"groups", {{"memory", false}}}}},
         {"realtime", {{"push_interval_sec", 1}}}}));

    const auto snapshot = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            const auto& metrics = payload.at("availability").at("metrics");
            return metrics.contains("memory") &&
                   metrics.at("memory").value("available", true) == false;
        },
        5s);
    ASSERT_TRUE(snapshot.has_value())
        << "no snapshot reported memory as disabled after SET groups.memory=false";
    const auto& memory_entry = (*snapshot)["availability"]["metrics"]["memory"];
    EXPECT_EQ(memory_entry.value("available", true), false);
    EXPECT_EQ(memory_entry.value("reason", std::string()), "disabled_by_config");
    EXPECT_TRUE((*snapshot)["memory"].is_null());
    EXPECT_EQ(platform::SysmonSnapshot::validate(*snapshot), std::nullopt);

    // 实时链路正常：关闭一类指标后快照仍按周期（1s）继续推送。
    const auto next = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 2500ms);
    ASSERT_TRUE(next.has_value()) << "realtime snapshot stream stopped after disabling a group";
    EXPECT_EQ(next->at("availability").at("metrics").at("memory").value("available", true), false);
}

// 契约 sysmon §3.1.1 第 5 步：`history.enabled` 运行期切换。
// - false→true：建库（且不写 restart 标记——进程没有重启）；
// - true→false：flush 并关库，实时快照推送不受影响（SET 后立即推送仍会到达）。
TEST_F(ConfigE2ETest, HistoryEnableDisableAtRuntimeKeepsRealtime) {
    // 启动时历史关闭（本用例的起点，见 history_e2e 的启动期用例互补）。
    write_config(/*history_enabled=*/false);
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value());
    pending_.clear();
    ASSERT_FALSE(std::filesystem::exists(history_db()))
        << "history.enabled=false must not create the history database";

    // ---- 运行期开启：建库、RTN 回推 true，且不得写 restart 标记 ----
    ASSERT_TRUE(
        send_set_config({{"history", {{"enabled", true}}},
                         {"sampling", {{"measure_interval_ms", 200}, {"record_interval_sec", 1}}},
                         {"realtime", {{"push_interval_sec", kLongPushIntervalSec}}}}));
    const auto enabled = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(enabled.has_value()) << "SET history.enabled=true did not roll back the config";
    EXPECT_EQ((*enabled)["history"]["enabled"], true);
    EXPECT_EQ((*enabled)["sampling"]["record_interval_sec"], 1);
    EXPECT_EQ((*enabled)["sampling"]["measure_interval_ms"], 200);
    // 消费掉本次 SET 的立即快照，保证后续等待不会命中"开启前"的残留帧。
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 5s).has_value());

    ASSERT_TRUE(wait_for_file(history_db(), 10s))
        << "history.enabled=true at runtime did not create the history database";
    {
        auto opened = sysmonstore::HistoryReader::open(history_db());
        ASSERT_EQ(opened.first, sysmonstore::HistoryAvailability::Ok);
        const auto marks = opened.second->query_marks(0, std::numeric_limits<int64_t>::max(), 100);
        // 运行期开启历史不是进程重启：写 restart 标记会把这段存储伪造成采集中断。
        EXPECT_TRUE(std::none_of(marks.begin(), marks.end(),
                                 [](const sysmonstore::HistoryMark& mark) {
                                     return mark.kind == sysmonstore::kMarkRestart;
                                 }))
            << "runtime history enable must not write a restart mark";
    }

    // ---- 运行期关闭：RTN 回推 false，库文件保留（不删除已有数据），实时推送照常 ----
    ASSERT_TRUE(send_set_config({{"history", {{"enabled", false}}}}));
    const auto disabled = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(disabled.has_value()) << "SET history.enabled=false did not roll back the config";
    EXPECT_EQ((*disabled)["history"]["enabled"], false);
    // 关闭历史不影响实时链路：SET 成功后仍会立即推送一帧快照（推送间隔是 300s，
    // 因此 5s 内到达只能来自触发 4）。
    const auto snapshot = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 5s);
    ASSERT_TRUE(snapshot.has_value())
        << "realtime snapshot push stopped after disabling history at runtime";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*snapshot), std::nullopt);
    EXPECT_TRUE(std::filesystem::exists(history_db()))
        << "disabling history must not delete the existing history database";
}

}  // namespace
}  // namespace dztrader::sysmon
