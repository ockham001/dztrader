/**
 * @file clock_e2e_test.cpp
 * @brief #299 端到端：时钟探测接进 dzsysmon——启动即探、时刻表定时探测、失败态、不阻塞
 *        采集、关闭停表、运行期重排。
 *
 * 做法：起**测试专用可执行文件**（CMake 目标 `dzsysmon_clock_e2e_runner`，产出
 * `tests/dzsysmon` = 同一份进程主体 + 假 NTP 传输，见 `clock_e2e_runner.cpp` /
 * `clock_fake_transport.h`），用真实事件通道观察帧序与快照内容。网络部分是注入替身，
 * 全程不访问外网（注入接缝见 `clock_probe.h` 的 `NtpTransport`）。
 *
 * 断言全部用「条件等待 + 超时即失败」表达，不用固定 sleep 当同步手段。
 * 信号量与事件通道是机器全局命名空间，与 dev 栈/并行测试互扰：检测到占用时默认
 * GTEST_SKIP，`DZ_SYSMON_E2E_STRICT=1` 时 FAIL（与既有 sysmon e2e 同约定）。该用例 RUN_SERIAL。
 *
 * 定时探测用例要等到「下一个整分钟」。为了让用例在支持 POSIX TZ 秒级偏移的平台上只等
 * 约 8 秒，SetUp 把子进程的 `TZ` 设成「本地秒数 = 52」的时区（留出两次 SET 帧的余量）；
 * 若平台不支持秒级偏移（TZ 解析失败），测试与子进程回退到同一套本地时区语义，目标时刻
 * 仍然正确，只是要等最多 60 秒——正确性不依赖该优化。
 */

#include "sysmon_e2e_support.h"

#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
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

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
using test::unique_suffix;

namespace process_v2 = boost::process::v2;
using namespace std::chrono_literals;

constexpr uint64_t kMB = 1024 * 1024;
/// 进程身份 = `dzsysmon`（快照校验要求 `source == dzsysmon`，契约 sysmon §5）。
/// 测试专用可执行文件在 tests/ 下输出为同名 `dzsysmon`（见 apps/sysmon/CMakeLists.txt）。
constexpr const char* kInstanceName = "dzsysmon";
/// 假传输的确定性偏差：服务端时间比本机快 50ms → `offset_ms` ≈ −50（本地慢）。
constexpr double kFakeOffsetMs = -50.0;
/// 允许的回调调度抖动（CI 负载下 io 线程可能延迟数毫秒；偏差 = −50 + 实际往返/2）。
constexpr double kOffsetToleranceMs = 15.0;

/// 平台 tzset：让 SetUp 里改写的 TZ 立即生效（与子进程同一套 CRT/glibc 语义）。
void platform_tzset() {
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif
}

/// 当前进程时区（TZ）下的本地日历字段。
std::tm local_calendar(std::chrono::system_clock::time_point wall) {
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(wall);
    std::tm fields{};
#ifdef _WIN32
    localtime_s(&fields, &timestamp);
#else
    localtime_r(&timestamp, &fields);
#endif
    return fields;
}

/// 「下一个整分钟」的 `HH:MM` 与该边界对应的单调时钟时刻。
///
/// 要求边界至少 @p min_seconds_ahead 秒后（保证两次 SET 帧写完前不会到点）；不满足时
/// 越过该边界看下一个。无论 TZ 是否支持秒级偏移，目标时刻与子进程的本地日历语义一致。
std::pair<std::string, std::chrono::steady_clock::time_point> next_local_minute(
    int min_seconds_ahead) {
    auto now_wall = std::chrono::system_clock::now();
    auto now_steady = std::chrono::steady_clock::now();
    for (;;) {
        const std::tm fields = local_calendar(now_wall);
        const int seconds_to_boundary = 60 - fields.tm_sec;
        if (seconds_to_boundary >= min_seconds_ahead) {
            const auto boundary_wall = now_wall + std::chrono::seconds(seconds_to_boundary);
            const std::tm target = local_calendar(boundary_wall);
            return {std::format("{:02d}:{:02d}", target.tm_hour, target.tm_min),
                    now_steady + std::chrono::seconds(seconds_to_boundary)};
        }
        now_wall += std::chrono::seconds(seconds_to_boundary + 1);
        now_steady += std::chrono::seconds(seconds_to_boundary + 1);
    }
}

class ClockE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        if (event_semaphore_busy()) {
            if (e2e_strict()) {
                FAIL() << "event channel semaphore is busy";
            }
            GTEST_SKIP() << "event channel semaphore busy (dev stack or parallel test running)";
        }
        if (!std::filesystem::exists(runner_executable())) {
            if (e2e_strict()) {
                FAIL() << "clock e2e runner not found: " << runner_executable().string();
            }
            GTEST_SKIP() << "clock e2e runner not built: " << runner_executable().string();
        }

        home_ = std::filesystem::temp_directory_path() / ("dz_sysmon_clock_e2e_" + unique_suffix());
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ / "configs");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();

        // 缩短「等到下一个整分钟」的时间（见文件头：不支持时只是慢，语义仍正确）。
        configure_short_minute_wait();
        create_event_channel();
    }

    void TearDown() override {
        stop_child();
        reader_.reset();
        meta_.reset();
        writer_.reset();
        remove_semaphore(reader_name_);
        remove_semaphore(kInstanceName);
        dztrader::shm::ProcessMutex::remove(shm::channel_name("dzevent") + ".meta");
        dztrader::shm::ProcessMutex::remove(shm::channel_name("dzevent") + ".write");
        restore_env("TZ", orig_tz_);
        restore_env("DZTRADER_HOME", orig_home_);
        platform_tzset();
        dztrader::paths::reset_for_test();
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
    }

    static std::filesystem::path runner_executable() {
        auto exe = dztrader::this_process::exe_dir() / kInstanceName;
#ifdef _WIN32
        exe += ".exe";
#endif
        return exe;
    }

    /// 把 TZ 设成「本地秒数 = 52」：下一个整分钟约 8 秒后到达（POSIX TZ 支持秒级偏移；
    /// 留出两次 SET 帧写完的余量）。
    void configure_short_minute_wait() {
        const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        const int utc_second = static_cast<int>(((now_seconds % 60) + 60) % 60);
        const int offset_seconds = (52 - utc_second + 60) % 60;
        const std::string timezone = offset_seconds == 0
                                         ? std::string("UTC0")
                                         : std::format("UTC-0:00:{:02d}", offset_seconds);
        orig_tz_ = dztrader::env::get("TZ");
        dztrader::env::set("TZ", timezone);
        platform_tzset();
    }

    /// 最小合法配置：clock 段由调用方参数化，其余取契约默认值。
    void write_config(const std::vector<std::string>& clock_servers,
                      const std::vector<std::string>& probe_times,
                      bool clock_enabled = true,
                      int push_interval_sec = 300,
                      int measure_interval_ms = 1000) {
        nlohmann::json groups = {
            {"cpu", true},         {"memory", true},        {"swap", true},
            {"volumes", true},     {"disk_io", true},       {"network", true},
            {"temperature", true}, {"cpu_frequency", true}, {"clock", true},
        };
        nlohmann::json cfg = {
            {"log", {{"level", "debug"}, {"flush_on", "info"}}},
            {"sampling",
             {{"measure_interval_ms", measure_interval_ms},
              {"record_interval_sec", 60},
              {"phase_offset_sec", 3},
              {"groups", groups}}},
            {"disk", {{"devices", nlohmann::json::array()}}},
            {"network", {{"hide_virtual", true}}},
            {"history",
             {{"enabled", false},
              {"raw_retention_hours", 24},
              {"max_retention_days", 30},
              {"max_bytes", 0}}},
            {"realtime", {{"push_interval_sec", push_interval_sec}}},
            {"thresholds",
             {{"cpu_sustain_used_pct", 90.0},
              {"cpu_sustain_samples", 10},
              {"memory_used_pct", 90.0},
              {"swap_used_pct", 50.0},
              {"volume_free_pct", 10.0},
              {"log_repeat_sec", 600}}},
            {"clock",
             {{"enabled", clock_enabled},
              {"servers", clock_servers},
              {"probe_times", probe_times},
              {"probe_timeout_ms", 10000},
              {"manual_min_interval_sec", 10},
              {"offset_warn_ms", 100},
              {"offset_error_ms", 1000},
              {"bound_alarm_ms", 200}}},
        };
        std::ofstream ofs(home_ / "configs" / (std::string(kInstanceName) + ".json"),
                          std::ios::binary | std::ios::trunc);
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
        // 生产链路由 master 注册子进程；本用例自建通道，必须自己把 runner 登记为订阅者。
        ASSERT_TRUE(meta_->add_reader(kInstanceName, 0));
        reader_ = std::make_unique<shm::Reader>(shm::Reader::create(meta_, reader_name_));
        writer_ = std::make_unique<shm::MultiWriter>(
            shm::MultiWriter::create(meta_, "dzmon_clock_e2e_writer_" + unique_suffix()));
    }

    void start_child() {
        boost::system::error_code ec;
        child_ = test::spawn_process(io_, runner_executable(), ec);
        ASSERT_FALSE(ec) << "spawn clock e2e runner failed: " << ec.message();
    }

    void stop_child() { test::stop_dzsysmon(child_); }

    bool send_set_config(const nlohmann::json& patch) {
        (void)writer_->refresh_subscribers();
        if (!platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_SYSMON_CONFIG, kInstanceName,
                                               patch)) {
            return false;
        }
        writer_->notify_subscribers();
        return true;
    }

    static nlohmann::json payload_of(const shm::FrameView& view) {
        const bool broadcast =
            view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT || view.type() == DZ_FRAME_NOTIFY_UI;
        const auto size = broadcast ? view.ext_payload_size() : view.ext_inst_payload_size();
        const auto* data = broadcast ? view.ext_payload() : view.ext_inst_payload();
        return nlohmann::json::parse(reinterpret_cast<const char*>(data),
                                     reinterpret_cast<const char*>(data) + size);
    }

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
                if (view.type() == DZ_FRAME_RTN_SYSMON_CONFIG ||
                    view.type() == DZ_FRAME_NOTIFY_UI) {
                    pending_.emplace_back(view.type(), payload_of(view));
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    /// 读整个进程日志（spdlog `flush_on=info`，每条 info/warn 都已落盘）。
    ///
    /// 日志按天滚动（`DailyFileSink`：`<name>_<YYYY-MM-DD>.log`），因此读目录下全部
    /// `*.log`——用例可能跨本地日期边界（TZ 秒级偏移只改秒，不改日期；但仍不假设文件名）。
    std::string read_log() const {
        const auto dir = home_ / "logs";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            return {};
        }
        std::string content;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".log") {
                continue;
            }
            std::ifstream ifs(entry.path(), std::ios::binary);
            if (!ifs) {
                continue;
            }
            content.append(
                std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()));
        }
        return content;
    }

    /// 统计日志中某段文本出现的次数（用例用它数「每轮一条」与「在飞拒绝」两类日志）。
    std::size_t log_occurrences(std::string_view needle) const {
        const std::string log = read_log();
        std::size_t count = 0;
        for (std::size_t pos = log.find(needle); pos != std::string::npos;
             pos = log.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    }

    /// 统计「探测完成」日志行数（每轮恰好一条；`clock probe skipped` 不计入）。
    std::size_t probe_finished_log_lines() const { return log_occurrences("clock probe finished"); }

    static void restore_env(const char* key, const std::optional<std::string>& value) {
        if (value.has_value()) {
            dztrader::env::set(key, *value);
        } else {
            dztrader::env::unset(key);
        }
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_tz_;
    std::string reader_name_ = "dzmon_clock_e2e_reader_" + unique_suffix();
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    boost::asio::io_context io_;
    std::unique_ptr<process_v2::process> child_;
    std::vector<std::pair<DzFrameType, nlohmann::json>> pending_;
};

// AC1 / AC2 数据面：启动后立即探测一次，结果（聚合值 + 每台明细）进快照并立即推送。
// AC5：每轮一条日志，字段齐全（偏差 / 误差界 / 来源 / 成功台数）。
TEST_F(ClockE2ETest, StartupProbeFillsClockDomainAndPushesSnapshot) {
    // 两台服务器：一台成功、一台超时 → 聚合值来自成功台，明细保留全部（部分失败可诊断）。
    write_config({"fake-ok", "fake-timeout"}, {});
    start_child();

    const auto startup = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s);
    ASSERT_TRUE(startup.has_value()) << "runner did not push the startup snapshot";
    // 帧序：启动首帧在首轮探测**之前**推送——clock 域是「尚未探测」域内空态。
    ASSERT_FALSE((*startup)["clock"].is_null());
    EXPECT_EQ((*startup)["clock"]["last_probe_unix_ms"].get<int64_t>(), 0)
        << "startup snapshot must precede the first probe result";
    EXPECT_TRUE((*startup)["clock"]["offset_ms"].is_null());
    EXPECT_TRUE((*startup)["clock"]["servers"].empty());
    EXPECT_EQ(
        (*startup)["availability"]["metrics"].at("clock.offset").value("reason", std::string()),
        "no_reference");
    EXPECT_EQ(platform::SysmonSnapshot::validate(*startup), std::nullopt);

    // 首轮探测完成 → 立即推送的快照携带真实结果。
    const auto probed = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > 0;
        },
        10s);
    ASSERT_TRUE(probed.has_value()) << "startup probe result did not reach the snapshot";
    const auto& clock = (*probed)["clock"];
    ASSERT_FALSE(clock["offset_ms"].is_null());
    ASSERT_FALSE(clock["bound_ms"].is_null());
    EXPECT_NEAR(clock["offset_ms"].get<double>(), kFakeOffsetMs, kOffsetToleranceMs);
    EXPECT_GT(clock["bound_ms"].get<double>(), 0.0);
    EXPECT_EQ(clock["source"], "fake-ok");
    EXPECT_EQ(clock["source_stratum"], 2);
    // 每台明细：成功台带四项测量，超时台带 error=timeout（失败样本不产出测量值）。
    const auto& servers = clock["servers"];
    ASSERT_EQ(servers.size(), 2U);
    EXPECT_EQ(servers[0]["host"], "fake-ok");
    EXPECT_EQ(servers[0]["ok"], true);
    EXPECT_TRUE(servers[0]["error"].is_null());
    EXPECT_EQ(servers[1]["host"], "fake-timeout");
    EXPECT_EQ(servers[1]["ok"], false);
    EXPECT_EQ(servers[1]["error"], "timeout");
    EXPECT_TRUE(servers[1]["offset_ms"].is_null());
    // 同步服务检测结果随本轮进快照（daemons 内容因机器而异，只断言形状）。
    EXPECT_TRUE(clock["daemons"].is_array());
    for (const auto& daemon : clock["daemons"]) {
        EXPECT_TRUE(daemon["name"].is_string());
        EXPECT_TRUE(daemon["running"].is_boolean());
    }
    EXPECT_TRUE(clock["kernel_synced"].is_null() || clock["kernel_synced"].is_boolean());
    EXPECT_EQ((*probed)["availability"]["metrics"].at("clock.offset").value("available", false),
              true);
    EXPECT_EQ(
        (*probed)["availability"]["metrics"].at("clock.sync_service").value("available", false),
        true);
    EXPECT_EQ(platform::SysmonSnapshot::validate(*probed), std::nullopt);

    // 每轮一条日志：字段齐全（offset_ms / bound_ms / source / servers_ok=1/2）。
    const auto log = read_log();
    EXPECT_EQ(probe_finished_log_lines(), 1U) << "each probe round must emit exactly one log line";
    EXPECT_NE(log.find("clock probe finished | offset_ms="), std::string::npos);
    EXPECT_NE(log.find("servers_ok=1/2"), std::string::npos);
    EXPECT_NE(log.find("source=fake-ok"), std::string::npos);
}

// AC3：探测全部失败时域为不可用空态（不是 0、不是陈旧值），且快照仍按周期推送。
TEST_F(ClockE2ETest, FailedProbeKeepsUnavailableDomainAndSnapshotStream) {
    write_config({"fake-timeout"}, {}, /*clock_enabled=*/true, /*push_interval_sec=*/1,
                 /*measure_interval_ms=*/200);
    start_child();

    const auto failed = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > 0;
        },
        10s);
    ASSERT_TRUE(failed.has_value()) << "probe failure did not reach the snapshot";
    const auto& clock = (*failed)["clock"];
    EXPECT_TRUE(clock["offset_ms"].is_null()) << "unavailable must not be reported as 0";
    EXPECT_TRUE(clock["bound_ms"].is_null());
    EXPECT_TRUE(clock["source_stratum"].is_null());
    EXPECT_EQ(clock["source"], "");
    ASSERT_EQ(clock["servers"].size(), 1U);
    EXPECT_EQ(clock["servers"][0]["ok"], false);
    EXPECT_EQ(clock["servers"][0]["error"], "timeout");
    EXPECT_EQ(
        (*failed)["availability"]["metrics"].at("clock.offset").value("reason", std::string()),
        "no_reference");
    EXPECT_EQ(platform::SysmonSnapshot::validate(*failed), std::nullopt);
    const int64_t failed_probe_ms = clock["last_probe_unix_ms"].get<int64_t>();
    EXPECT_EQ(probe_finished_log_lines(), 1U);

    // 周期推送继续：下一帧快照仍带同一「全部失败」结果（不是陈旧的成功值）。
    const auto periodic = wait_for_snapshot(
        [failed_probe_ms](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() == failed_probe_ms &&
                   payload["clock"]["offset_ms"].is_null();
        },
        5s);
    ASSERT_TRUE(periodic.has_value()) << "snapshot stream stopped after an all-failed probe round";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*periodic), std::nullopt);
}

// AC6：探测不阻塞采集与帧处理——探测在飞（假传输刻意延迟 2.5s）期间，计量 tick 照常推进。
TEST_F(ClockE2ETest, ProbeInFlightDoesNotBlockMeasureTicks) {
    write_config({"fake-ok-slow"}, {}, /*clock_enabled=*/true, /*push_interval_sec=*/1,
                 /*measure_interval_ms=*/200);
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value());
    pending_.clear();

    // 探测完成前至少两帧周期快照：计量样本在推进、且没有错过 tick。
    std::size_t in_flight_snapshots = 0;
    for (int i = 0; i < 2; ++i) {
        const auto snapshot = wait_for_snapshot(
            [](const nlohmann::json& payload) {
                return payload["clock"]["last_probe_unix_ms"].get<int64_t>() == 0 &&
                       payload["sampling"]["last_sample_unix_ms"].get<int64_t>() > 0;
            },
            5s);
        ASSERT_TRUE(snapshot.has_value())
            << "measure ticks stopped while a probe was in flight (contract sysmon §2.7)";
        EXPECT_EQ((*snapshot)["sampling"]["missed_ticks"].get<uint64_t>(), 0U);
        ++in_flight_snapshots;
    }
    EXPECT_EQ(in_flight_snapshots, 2U);

    // 慢探测最终完成并推送结果（异步路径没有丢回调）。
    const auto probed = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > 0;
        },
        10s);
    ASSERT_TRUE(probed.has_value()) << "slow probe did not complete";
    EXPECT_EQ(probed->at("clock").at("source"), "fake-ok-slow");
    EXPECT_EQ(platform::SysmonSnapshot::validate(*probed), std::nullopt);
}

// AC2 / AC4：运行期 SET `probe_times` 后按新时刻排期（不需重启）；到点后快照自动更新、
// `last_probe_unix_ms` 前进。
TEST_F(ClockE2ETest, RuntimeProbeTimesRescheduleAndFire) {
    write_config({"fake-ok"}, {});
    start_child();
    const auto startup_probe = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > 0;
        },
        10s);
    ASSERT_TRUE(startup_probe.has_value()) << "startup probe did not complete";
    const int64_t first_probe_ms =
        startup_probe->at("clock").at("last_probe_unix_ms").get<int64_t>();
    EXPECT_EQ(probe_finished_log_lines(), 1U);

    // 运行期改探测时刻：目标是「下一个整分钟」（TZ 秒级偏移生效时约 8s，否则最长 60s）。
    const std::string target = next_local_minute(/*min_seconds_ahead=*/4).first;
    ASSERT_TRUE(send_set_config({{"clock", {{"probe_times", {target}}}}}));
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s).has_value());

    const auto fired = wait_for_snapshot(
        [first_probe_ms](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > first_probe_ms;
        },
        75s);
    ASSERT_TRUE(fired.has_value()) << "scheduled probe did not fire at probe_times=" << target
                                   << " (contract sysmon §2.7 / §3.1.1 step 2)";
    EXPECT_NE(fired->at("clock").at("source"), "");
    EXPECT_EQ(platform::SysmonSnapshot::validate(*fired), std::nullopt);
    EXPECT_EQ(probe_finished_log_lines(), 2U)
        << "scheduled round must emit exactly one more log line";
}

// AC4 的反向 + 契约 §3.1：`clock` 域关闭时停止定时探测并清空该域；false→true 时**立即**
// 发起一轮探测（与进程启动同款），且重开不得复活关闭前的旧值。
TEST_F(ClockE2ETest, DisablingClockStopsTimerAndClearsDomain) {
    write_config({"fake-ok"}, {});
    start_child();
    const auto startup_probe = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > 0;
        },
        10s);
    ASSERT_TRUE(startup_probe.has_value());
    const int64_t first_probe_ms =
        startup_probe->at("clock").at("last_probe_unix_ms").get<int64_t>();
    EXPECT_EQ(probe_finished_log_lines(), 1U);

    // 先排一个即将到点的定时探测，再关闭域：该等待必须被取消。
    const auto [target, boundary_deadline] = next_local_minute(/*min_seconds_ahead=*/4);
    ASSERT_TRUE(send_set_config({{"clock", {{"probe_times", {target}}}}}));
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s).has_value());
    ASSERT_TRUE(send_set_config(
        {{"clock", {{"enabled", false}}}, {"realtime", {{"push_interval_sec", 1}}}}));

    const auto disabled = wait_for_snapshot(
        [](const nlohmann::json& payload) { return payload["clock"].is_null(); }, 5s);
    ASSERT_TRUE(disabled.has_value()) << "clock domain must be null after clock.enabled=false";
    for (const char* key : {"clock.offset", "clock.sync_service"}) {
        EXPECT_EQ((*disabled)["availability"]["metrics"].at(key).value("reason", std::string()),
                  "disabled_by_config")
            << key;
    }
    EXPECT_EQ(platform::SysmonSnapshot::validate(*disabled), std::nullopt);
    EXPECT_EQ(probe_finished_log_lines(), 1U);

    // 重新开启：SET 同步推送的一帧是域内空态（关闭时清掉的结果不得复活），随后**立即**
    // 发起一轮探测（契约 §3.1「与进程启动同款」），不必空等下一个 `probe_times` 时刻。
    ASSERT_TRUE(send_set_config({{"clock", {{"enabled", true}}}}));
    const auto restored = wait_for_snapshot(
        [](const nlohmann::json& payload) {
            return !payload["clock"].is_null() &&
                   payload["clock"]["last_probe_unix_ms"].get<int64_t>() == 0;
        },
        5s);
    ASSERT_TRUE(restored.has_value())
        << "re-enabling must restore an empty state (the cleared result must not resurrect)";
    EXPECT_TRUE((*restored)["clock"]["offset_ms"].is_null());
    EXPECT_EQ(platform::SysmonSnapshot::validate(*restored), std::nullopt);

    const auto reprobed = wait_for_snapshot(
        [first_probe_ms](const nlohmann::json& payload) {
            return payload["clock"]["last_probe_unix_ms"].get<int64_t>() > first_probe_ms;
        },
        5s);
    ASSERT_TRUE(reprobed.has_value())
        << "contract sysmon §3.1: re-enabling the clock domain must probe immediately";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*reprobed), std::nullopt);
    EXPECT_EQ(probe_finished_log_lines(), 2U);

    // 关闭时那个到点等待必须**失效**：重开后只应存在一个新的到点等待。若旧等待仍会回调，
    // 原定时刻会有两个 handler 同时到点——一个发起探测、另一个被在飞拒绝并记 `skipped` 日志。
    const auto observe_until = boundary_deadline + 3s;
    while (std::chrono::steady_clock::now() < observe_until) {
        EXPECT_EQ(log_occurrences("clock probe skipped"), 0U)
            << "leaked probe timer: a canceled wait fired after re-enable";
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(log_occurrences("clock probe skipped"), 0U);
    // 原定时刻的定时探测照常发生：启动 1 + 重开立即 1 + 到点 1。
    EXPECT_EQ(probe_finished_log_lines(), 3U);
}

}  // namespace
}  // namespace dztrader::sysmon
