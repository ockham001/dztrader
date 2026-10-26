/**
 * @file network_config_e2e_test.cpp
 * @brief #77 端到端：`SET_SYSMON_CONFIG` 成功后立即推送一帧快照（开关即时生效）。
 *
 * 这是「『显示全部』开关走配置并即时生效」这条验收标准的**唯一**真实验证点：
 * 契约 sysmon §4 触发 4 要求 SET 成功后立即推送快照，否则一次开关操作最长要等一个
 * 推送周期（默认 5s）才可见。纯函数与快照构造的单测无法覆盖这条链路——它由
 * `SysmonApp::apply_sysmon_config` 的调用顺序决定。
 *
 * 做法：起一个真实 `dzsysmon` 进程（临时 DZTRADER_HOME + 真实事件通道），用独立 reader
 * 观察帧序：发 SET 后必须**在周期推送之前**收到一帧 `RTN_SYSMON_SNAPSHOT`，且该帧
 * 的 `network` 体现新的 `hide_virtual`（本机若有虚拟网卡则滤掉；本机若只有物理网卡，
 * 退化为断言「快照在 SET 之后立即到达且帧合法」）。
 *
 * 信号量与事件通道是机器全局命名空间，与 dev 栈/并行测试互扰：检测到占用时
 * 默认 GTEST_SKIP，`DZ_SYSMON_E2E_STRICT=1` 时 FAIL（与 smoke_test 同约定）。
 * 该用例 RUN_SERIAL（见 CMakeLists）。
 */

#include "sysmon_e2e_support.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/sysmon.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/process_mutex.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/system/error_code.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <filesystem>
#include <fstream>
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

using test::e2e_strict;
using test::event_semaphore_busy;
using test::remove_semaphore;
using test::sysmon_executable;
using test::unique_suffix;

namespace process_v2 = boost::process::v2;
using namespace std::chrono_literals;

constexpr uint64_t kMB = 1024 * 1024;
/// 周期推送间隔设得远大于本用例等待窗口：若立即推送缺失，等待窗口内不会收到周期帧，
/// 「SET 后有快照到达」因此唯一归因于触发 4（而不是被周期推送蒙混）。
constexpr int kPushIntervalSec = 300;

class NetworkConfigE2ETest : public ::testing::Test {
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

        home_ = std::filesystem::temp_directory_path() / ("dz_sysmon_e2e_" + unique_suffix());
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
        // 命名信号量是机器全局资源：不 unlink 会在 /dev/shm 里随每次运行累积
        // （reader 名含随机后缀，无法靠固定名回收）。dzsysmon 的信号量同样清理。
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

    /// 最小合法 dzsysmon 配置：推送间隔拉长（见 kPushIntervalSec），其余取默认值。
    void write_config() {
        nlohmann::json cfg = {
            {"log", {{"level", "debug"}, {"flush_on", "info"}}},
            {"export", nlohmann::json::object()},
            {"logs", nlohmann::json::object()},
            {"data", nlohmann::json::object()},
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
              {"memory_used_pct", 90.0},
              {"swap_used_pct", 50.0},
              {"volume_free_pct", 10.0},
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
        // reader 必须先注册：MultiWriter 构造时同步订阅者缓存，否则 notify 唤不醒本 reader。
        ASSERT_TRUE(meta_->add_reader(reader_name_, 0));
        // 生产链路由 master 的 register_subscriber(entry.name) 注册子进程；本用例自建通道，
        // 所以必须自己把 dzsysmon 登记为订阅者——否则 notify_subscribers 不会唤醒它的命名
        // 信号量，SET_SYSMON_CONFIG 永远到不了目标进程（本用例初次失败的原因）。
        ASSERT_TRUE(meta_->add_reader("dzsysmon", 0));
        reader_ = std::make_unique<shm::Reader>(shm::Reader::create(meta_, reader_name_));
        writer_ = std::make_unique<shm::MultiWriter>(
            shm::MultiWriter::create(meta_, "dzweb_e2e_" + unique_suffix()));
    }

    /// 启动真实 dzsysmon（instance_id 由可执行文件名决定 = dzsysmon）。
    /// 与 master 的 ChildProcess 同款启动方式：default_process_launcher + error_code（非抛出）。
    ///
    /// 注意订阅者缓存：MultiWriter 在构造时快照 ChannelMeta 的订阅者列表，而 dzsysmon 是在
    /// 之后注册的 reader/信号量。因此写帧前必须 refresh_subscribers()，否则 notify 只唤醒
    /// 启动前已存在的订阅者，dzsysmon 收不到 SET_SYSMON_CONFIG（首帧快照仍能读到，因为那是
    /// 子进程主动写、本 reader 早已注册）。
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
                                                          "dzsysmon", patch);
        if (!ok) {
            return false;
        }
        writer_->notify_subscribers();
        return true;
    }

    /// 从帧视图按帧头类型取 payload（快照 / NOTIFY_UI 无 instance_id，配置帧有）。
    static nlohmann::json payload_of(const shm::FrameView& view) {
        const bool broadcast =
            view.type() == DZ_FRAME_RTN_SYSMON_SNAPSHOT || view.type() == DZ_FRAME_NOTIFY_UI;
        const auto size = broadcast ? view.ext_payload_size() : view.ext_inst_payload_size();
        const auto* data = broadcast ? view.ext_payload() : view.ext_inst_payload();
        return nlohmann::json::parse(reinterpret_cast<const char*>(data),
                                     reinterpret_cast<const char*>(data) + size);
    }

    /// 在窗口内等指定类型的帧，**保留**窗口内读到的其它类型帧到 `pending_`。
    ///
    /// 保留是必需的：一次 SET 会先写 RTN_SYSMON_CONFIG 再写 RTN_SYSMON_SNAPSHOT，
    /// 若等待快照时直接丢弃配置帧，后面就再也读不到它（reader 是单向游标）。
    /// NOTIFY_UI 同理：失败路径先发通知、后回推配置，等配置时丢弃通知会让后续等待永远超时。
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
                // 只缓存本用例关心的 JSON 帧：通道上还有其它二进制/控制帧（如
                // PRELOAD_EVENT_SHM 等），对它们 parse 会抛。
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
    std::string reader_name_ = "dzmon_e2e_reader_" + unique_suffix();
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::unique_ptr<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    boost::asio::io_context io_;
    std::unique_ptr<process_v2::process> child_;
    /// 已读到但本次等待不需要的帧（按到达顺序保留，供后续 wait_for_frame 消费）。
    std::vector<std::pair<DzFrameType, nlohmann::json>> pending_;
};

// 契约 sysmon §4 触发 4：SET_SYSMON_CONFIG 成功后立即推送一帧快照。
// 断言的关键是**时序**：周期推送间隔被设为 300s，因此 SET 后几秒内出现的快照只能来自
// 立即推送；若实现退回「等下一个周期」，本用例会在窗口耗尽后失败。
TEST_F(NetworkConfigE2ETest, SetConfigPushesSnapshotImmediately) {
    start_child();

    // 等启动首帧（触发 1），确认进程已完成初始化并能写出快照帧。
    const auto first = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s);
    ASSERT_TRUE(first.has_value()) << "dzsysmon did not push the startup snapshot";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*first), std::nullopt);

    // 丢弃启动阶段残留的帧（启动时会先推一次 RTN_SYSMON_CONFIG，其 hide_virtual=true
    // 与本用例的 SET 后回推值相同形状；不清理会读到旧帧、断言到启动值）。
    pending_.clear();

    // 初始配置 hide_virtual=true；切换为 false（显示全部网卡）。
    ASSERT_TRUE(send_set_config({{"network", {{"hide_virtual", false}}}}));

    // 立即推送的快照应在数秒内到达（周期推送是 300s，不可能这么快）。
    // 注意：实现先写 RTN_SYSMON_CONFIG 再写快照，等待顺序不影响结果（不匹配的帧会被保留）。
    const auto after_set = wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 5s);
    ASSERT_TRUE(after_set.has_value())
        << "SET_SYSMON_CONFIG did not trigger an immediate snapshot push (contract sysmon §4 "
           "trigger 4)";
    EXPECT_EQ(platform::SysmonSnapshot::validate(*after_set), std::nullopt);

    // 生效证据：RTN_SYSMON_CONFIG 已回推新的 hide_virtual。
    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(config.has_value()) << "SET must roll the effective config back to the client";
    EXPECT_EQ((*config)["network"]["hide_virtual"], false);

    // ---- 开关的效果：hide_virtual=false 后虚拟网卡必须**出现**（未被过滤） ----
    // 启动首帧是 hide_virtual=true 的快照：先确认它确实过滤掉了虚拟网卡，再看切换后是否
    // 出现——两次对比才是"开关真的生效"的证据，只断言键存在是无效的（恒真）。
    const auto& startup_network = first->operator[]("network");
    ASSERT_TRUE(startup_network.is_array()) << "snapshot must carry the network array";
    const bool startup_has_virtual =
        std::any_of(startup_network.begin(), startup_network.end(),
                    [](const nlohmann::json& nic) { return nic.value("is_virtual", false); });
    EXPECT_FALSE(startup_has_virtual)
        << "hide_virtual=true must not emit virtual interfaces (contract sysmon §7)";

    const auto& shown_network = after_set->operator[]("network");
    ASSERT_TRUE(shown_network.is_array());
    ASSERT_FALSE(shown_network.empty()) << "host must have at least one interface";
    for (const auto& nic : shown_network) {
        ASSERT_TRUE(nic.contains("is_virtual"));
        ASSERT_TRUE(nic.contains("interface"));
    }
    // 本机（含 lo；多数宿主机还有 docker0/bridge/veth）至少有一个虚拟接口。
    // 若宿主机确实只有物理网卡，hide_virtual=false 与 true 的快照会相同——此时
    // 「显示全部未被过滤」无法从数据上区分，只能退化为上面"快照合法且立即到达"的断言。
    const bool shown_has_virtual =
        std::any_of(shown_network.begin(), shown_network.end(),
                    [](const nlohmann::json& nic) { return nic.value("is_virtual", false); });
    if (shown_has_virtual) {
        EXPECT_GE(shown_network.size(), startup_network.size())
            << "showing all interfaces cannot yield fewer entries";
    }
}

// 契约 sysmon §3.3 四件套：非法 patch 被拒绝时旧值不变（内存镜像回滚）、记日志、
// `NOTIFY_UI` error+popup、回推全量旧值，且失败**不**触发 §4 触发 4 的立即快照。
TEST_F(NetworkConfigE2ETest, RejectedPatchNotifiesRollsBackAndDoesNotPushSnapshot) {
    start_child();
    ASSERT_TRUE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 15s).has_value());
    // 丢弃启动期残留帧：否则下面的 RTN_SYSMON_CONFIG 会命中启动推送的旧配置帧
    // （其 hide_virtual=true 恰好等于回滚期望值），断言被旧帧满足 = 证据无效。
    pending_.clear();

    // hide_virtual 必须是 bool：给字符串 → SysmonConfig::validate 拒绝。
    ASSERT_TRUE(send_set_config({{"network", {{"hide_virtual", "yes"}}}}));

    // 四件套之一：error 级通知 + popup=true。配置失败是需要人处理的真故障，与资源越限的
    // 「只写日志、不弹窗」（ADR 0034）正好相反。
    const auto notify = wait_for_frame(DZ_FRAME_NOTIFY_UI, 5s);
    ASSERT_TRUE(notify.has_value()) << "a rejected SET must notify the ui";
    EXPECT_EQ(notify->value("source", std::string()), "dzsysmon");
    EXPECT_EQ(notify->value("level", std::string()), "error");
    EXPECT_TRUE(notify->value("popup", false));
    // 通知必须能归因到本次配置失败，而不是任意一条 dzsysmon error（否则断言对别的故障也成立）。
    EXPECT_NE(notify->value("message", std::string()).find("sysmon config update failed"),
              std::string::npos);

    const auto config = wait_for_frame(DZ_FRAME_RTN_SYSMON_CONFIG, 5s);
    ASSERT_TRUE(config.has_value()) << "failure path must still push the effective config";
    // 回滚旧值：仍为 true。
    EXPECT_EQ((*config)["network"]["hide_virtual"], true);
    // 失败路径不推快照（契约 §4 触发 4 明确「成功后」）。
    EXPECT_FALSE(wait_for_frame(DZ_FRAME_RTN_SYSMON_SNAPSHOT, 2s).has_value())
        << "a rejected SET must not trigger an immediate snapshot push";
}

}  // namespace
}  // namespace dztrader::sysmon
