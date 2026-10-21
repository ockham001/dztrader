/**
 * @file stg_demo_test.cpp
 * @brief stg_demo 进程内集成测试 (Seam A: 帧通道边界)
 *
 * 只通过外部可观察行为验证示例策略: 临时 DZTRADER_HOME + 真实事件/行情通道,
 * 测试线程内运行 dztrader::run_strategy(demo), 再用独立 writer 写 UI_INPUT /
 * TICK / 回报帧, 用独立 probe reader 读 OUTPUT_UI; 阻塞在 dz_wait 的策略线程
 * 由定向信号量唤醒 (测试进程不是 master, 事件通道订阅者列表为空,
 * writer.notify_subscribers() 唤不醒策略, 故直连 "stg.<策略名>" 信号量)。
 *
 * 不测内部函数/私有状态: 断言只落在 OUTPUT_UI 文本行与进程退出码上。
 *
 * 唯一例外: dz_init 失败诊断在进程边界验证 (运行示例子进程并捕获 stderr),
 * 与 master 捕获子进程 stderr 的真实路径一致。
 */
#include <gtest/gtest.h>

#include "stg_demo_strategy.h"

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/string_util.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/strategy_engine.h>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr uint64_t kMB = 1024 * 1024;

void create_event_channel(const std::filesystem::path& shm_dir) {
    dztrader::shm::ChannelConfig cfg{
        .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)dztrader::shm::ChannelMeta::open_or_create(cfg);
}

void create_md_channel(const std::filesystem::path& shm_dir) {
    dztrader::shm::ChannelConfig cfg{
        .channel_name = "test_md",
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)dztrader::shm::ChannelMeta::open_or_create(cfg);
}

/// 示例可执行文件路径: 测试 exe 在 <out>/tests/, 示例在 <out>/ (Windows 补 .exe)
std::filesystem::path demo_exe_path() {
    auto path = dztrader::this_process::exe_dir().parent_path() / "stg_demo";
#ifdef _WIN32
    path += ".exe";
#endif
    return path;
}

/// 文本拆行 (md dump 单帧多行); 空文本返回空
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string::npos) {
            if (start < text.size()) {
                lines.push_back(text.substr(start));
            }
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

/// 在 "key=123" 中取数值 (仅演示测试用; key 已含前导空格/等号由调用方给全)
std::optional<int64_t> find_key_number(std::string_view text, std::string_view key) {
    const std::size_t pos = text.find(key);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t i = pos + key.size();
    int64_t value = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        ++i;
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    return value;
}

/// 取 " time=HH:MM:SS.uuuuuu" 的值 (定长 15 字符)
std::optional<std::string> find_tick_time(std::string_view line) {
    const std::string_view key = " time=";
    const std::size_t pos = line.find(key);
    if (pos == std::string_view::npos || pos + key.size() + 15 > line.size()) {
        return std::nullopt;
    }
    return std::string(line.substr(pos + key.size(), 15));
}

/// 策略写出的订阅/退订请求帧: 帧头来源 + 解码 payload
struct CapturedSubscribe {
    std::string frame_instance_id;
    dztrader::SubscribeReq req;
};

class StgDemoTest : public ::testing::Test {
protected:
    void SetUp() override {
        // pid 后缀: 同机并行跑本测试二进制 (如双 worktree) 时互不删对方的 shm 目录
        home_ = std::filesystem::temp_directory_path() /
                ("dz_test_stg_demo_" + std::to_string(dztrader::this_process::pid()));
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        std::filesystem::create_directories(home_ / "shm", ec);
        ASSERT_FALSE(ec) << "create temp home failed: " << ec.message();
        // paths::shm() 按 DZTRADER_HOME 进程内缓存: 同二进制全部用例共用同一 home
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        orig_md_source_ = dztrader::env::get("DZTRADER_MD_SOURCE");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();  // 清空进程级缓存, 指向本用例 home
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
#ifdef _WIN32
        // boost env::set 走 SetEnvironmentVariableW, 而示例策略用 std::getenv 展示源名;
        // MSVC CRT 的 getenv 读的是 CRT 环境副本, 进程内改环境需同步该副本。
        // (生产路径由 master 在 CreateProcess 时注入, 子进程 CRT 启动即读到, 无此差异)
        (void)_putenv_s("DZTRADER_MD_SOURCE", "test_md");
#endif
        create_event_channel(home_ / "shm");
        create_md_channel(home_ / "shm");
        strategy_id_ = dztrader::this_process::exe_stem();  // 策略身份 = 测试 exe 名
        probe_.emplace(
            dztrader::shm::Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                          home_ / "shm", "stg_demo_test_probe"));
        done_.store(false);
        // 防挂死兜底: 正常路径用例自行 quit/shutdown, 超时则定向 SHUTDOWN + 唤醒
        rescue_ = std::thread([this] {
            for (int i = 0; i < 1200; ++i) {  // 60s
                if (done_.load()) {
                    return;
                }
                std::this_thread::sleep_for(50ms);
            }
            write_shutdown_frame();
            wake_strategy();
        });
    }

    void TearDown() override {
        if (strategy_thread_.joinable()) {
            // 兜底: 用例断言提前失败时策略仍在跑, 定向 SHUTDOWN 结束它
            write_shutdown_frame();
            wake_strategy();
            strategy_thread_.join();
        }
        done_.store(true);
        if (rescue_.joinable()) {
            rescue_.join();
        }
        EXPECT_EQ(unexpected_ui_instance_count_, 0U)
            << "OUTPUT_UI frame with unexpected instance_id observed (identity boundary)";
        probe_.reset();  // 先释放 mmap 再删目录
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        if (orig_md_source_) {
            dztrader::env::set("DZTRADER_MD_SOURCE", *orig_md_source_);
        } else {
            dztrader::env::unset("DZTRADER_MD_SOURCE");
        }
        dztrader::paths::reset_for_test();
    }

    // ---- 通道与帧 helpers (全部只经帧通道, 不碰策略内部) ----

    std::shared_ptr<dztrader::shm::ChannelMeta> open_event_meta() {
        return std::make_shared<dztrader::shm::ChannelMeta>(dztrader::shm::ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ / "shm"));
    }

    std::shared_ptr<dztrader::shm::ChannelMeta> open_md_meta() {
        return std::make_shared<dztrader::shm::ChannelMeta>(
            dztrader::shm::ChannelMeta::open_only("test_md", home_ / "shm"));
    }

    /// UI_INPUT 变长文本帧 → 目标策略
    bool write_ui_input(std::string_view command) {
        try {
            dztrader::shm::MultiWriter writer =
                dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
            const bool ok =
                writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, strategy_id_.c_str(),
                                            reinterpret_cast<const std::byte*>(command.data()),
                                            static_cast<uint32_t>(command.size()));
            writer.notify_subscribers();
            return ok;
        } catch (...) {
            return false;
        }
    }

    /// 定向 SHUTDOWN 帧 (目标本策略)
    void write_shutdown_frame() {
        try {
            dztrader::shm::MultiWriter writer =
                dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
            (void)writer.write_ext_inst_frame(DZ_FRAME_SHUTDOWN, strategy_id_.c_str(), nullptr, 0);
            writer.notify_subscribers();
        } catch (...) {
        }
    }

    /// 直连策略信号量唤醒 dz_wait (测试进程无 master, 订阅者列表为空)
    void wake_strategy() {
        try {
            dztrader::shm::NamedSemaphore sem("stg." + strategy_id_);
            sem.notify();
        } catch (...) {
        }
    }

    // ---- 策略生命周期 ----

    bool start_strategy() {
        strategy_thread_ = std::thread([this] {
            exit_code_ = dztrader::run_strategy(strategy_);
            done_.store(true);
        });
        return wait_until([this] { return any_line("[ready] ", "strategy=" + strategy_id_); }, 10s);
    }

    void join_strategy() {
        if (strategy_thread_.joinable()) {
            strategy_thread_.join();
        }
        done_.store(true);
    }

    // ---- OUTPUT_UI 采集与断言 helpers ----

    void pump_output() {
        if (!probe_) {
            return;
        }
        for (;;) {
            const std::byte* frame = probe_->next_frame();
            if (frame == nullptr) {
                return;
            }
            const dztrader::shm::FrameView view(frame);
            if (view.type() == DZ_FRAME_REQUEST_MD_SUBSCRIBE) {
                // 策略写出的订阅/退订请求帧 (行情订阅帧边界): 解码留作订阅/退订断言;
                // 帧头 instance_id 应为绑定的行情源名, payload.instance_id 应为 stg.<策略名>
                try {
                    CapturedSubscribe captured;
                    captured.frame_instance_id = std::string(view.ext_inst_id());
                    captured.req =
                        dztrader::shm::decode_ext_inst_json<dztrader::SubscribeReq>(view);
                    subscribe_reqs_.push_back(std::move(captured));
                } catch (const std::exception&) {
                }
                continue;
            }
            if (view.type() != DZ_FRAME_OUTPUT_UI) {
                continue;
            }
            // 身份边界: 策略上行帧的 instance_id 必须是本策略裸名
            if (std::string_view(view.ext_inst_id()) != strategy_id_) {
                ++unexpected_ui_instance_count_;
                continue;
            }
            std::string text(reinterpret_cast<const char*>(view.ext_inst_payload()),
                             view.ext_inst_payload_size());
            for (auto& line : split_lines(text)) {
                output_lines_.push_back(std::move(line));
            }
            output_frames_.push_back(std::move(text));
        }
    }

    template <typename Pred>
    bool wait_until(Pred pred, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump_output();
            if (pred()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            wake_strategy();
            std::this_thread::sleep_for(10ms);
        }
    }

    /// 已写出的行情订阅请求帧中, 是否存在指定动作且包含该合约的请求
    bool saw_subscribe_action(dztrader::SubscribeAction action, std::string_view instrument) const {
        for (const auto& captured : subscribe_reqs_) {
            if (captured.req.action != action) {
                continue;
            }
            for (const auto& listed : captured.req.instruments) {
                if (listed == instrument) {
                    return true;
                }
            }
        }
        return false;
    }

    /// 订阅帧身份边界: 帧头 instance_id = 绑定行情源名, payload.instance_id = stg.<策略名>
    bool subscribe_identity_all_valid() const {
        for (const auto& captured : subscribe_reqs_) {
            if (captured.frame_instance_id != "test_md" ||
                captured.req.instance_id != "stg." + strategy_id_) {
                return false;
            }
        }
        return true;
    }

    bool any_line(std::string_view prefix, std::string_view needle) const {
        for (const auto& line : output_lines_) {
            if (line.starts_with(prefix) && line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// 最近一条以 prefix 开头的行是否含 needle (用于"最新输出"断言)
    bool last_line_contains(std::string_view prefix, std::string_view needle) const {
        for (auto it = output_lines_.rbegin(); it != output_lines_.rend(); ++it) {
            if (it->starts_with(prefix)) {
                return it->find(needle) != std::string::npos;
            }
        }
        return false;
    }

    std::size_t count_lines(std::string_view prefix, std::string_view needle) const {
        std::size_t count = 0;
        for (const auto& line : output_lines_) {
            if (line.starts_with(prefix) && line.find(needle) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }

    std::string dump_output() const {
        std::string out = "--- OUTPUT_UI frames ---\n";
        for (const auto& frame : output_frames_) {
            out += "  frame: " + frame + "\n";
        }
        return out;
    }

    // ---- 交互 ----

    void send_command(std::string_view command) {
        EXPECT_TRUE(write_ui_input(command)) << "write UI_INPUT failed: " << command;
        wake_strategy();
    }

    /// 重复下发 md dump 直至输出帧满足 pred (tick 可能在多条事件批次中陆续消费,
    /// 首次 dump 可能小于最终缓存条数); 命中帧写入 out
    template <typename Pred>
    bool request_dump_until(std::string_view command, Pred pred, std::string& out) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            const std::size_t before = output_frames_.size();
            send_command(command);
            const bool got = wait_until(
                [&] {
                    for (std::size_t i = before; i < output_frames_.size(); ++i) {
                        if (output_frames_[i].starts_with("[md] action=dump") &&
                            pred(output_frames_[i])) {
                            out = output_frames_[i];
                            return true;
                        }
                    }
                    return false;
                },
                1s);
            if (got) {
                return true;
            }
        }
        return false;
    }

    // ---- 帧注入 (行情/回报) ----

    /// 逆序 (时间递减) 注入 tick: 缓存按到达顺序存, md dump 必须重排才是时间正序,
    /// 从而让"时间正序"验收真实可证 (若实现直接按到达顺序输出会失败)
    void push_ticks_descending(std::string_view instrument, int count) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_md_meta(), "stg_demo_test_md_writer");
        const std::string name(instrument);
        for (int i = 0; i < count; ++i) {
            const int time_seconds = count - 1 - i;  // 时间随写入次序递减
            DzTick tick{};
            dztrader::copy_string(tick.instrument_id, name.c_str(), true);
            tick.time = time_seconds;
            tick.last_price = 100.0 + static_cast<double>(time_seconds);
            tick.volume = time_seconds;
            tick.bid_price[0] = 99.0;
            tick.bid_volume[0] = 10;
            tick.ask_price[0] = 101.0;
            tick.ask_volume[0] = 20;
            tick.open_interest = 1000 + time_seconds;
            EXPECT_TRUE(writer.write_frame(DZ_FRAME_TICK, tick));
        }
        wake_strategy();
    }

    /// 注入一条含 CTP DBL_MAX 哨兵档位价格的 tick (md_spi 对无效档位原样透传)
    void push_sentinel_price_tick() {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_md_meta(), "stg_demo_test_md_writer");
        DzTick tick{};
        dztrader::copy_string(tick.instrument_id, "IF2606", true);
        tick.time = 1;
        tick.last_price = 100.5;
        tick.bid_price[0] = std::numeric_limits<double>::max();
        tick.ask_price[0] = -std::numeric_limits<double>::max();
        tick.bid_volume[0] = 7;
        tick.ask_volume[0] = 8;
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_TICK, tick));
        wake_strategy();
    }

    void inject_order_report(DzOrderId order_id,
                             DzOrderStatus status,
                             std::string_view strategy_override = {}) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
        DzOrderReport report{};
        report.order_id = order_id;
        const std::string report_strategy =
            strategy_override.empty() ? strategy_id_ : std::string(strategy_override);
        dztrader::copy_string(report.strategy_id, report_strategy.c_str(), true);
        dztrader::copy_string(report.account_id, "CTP001", true);
        dztrader::copy_string(report.instrument_id, "IF2606", true);
        report.direction = DZ_DIRECTION_SHORT;
        report.price_type = DZ_PRICE_LIMIT;
        report.position_effect = DZ_POSITION_EFFECT_OPEN;
        report.status = status;
        report.price = 100.5;
        report.volume = 2;
        report.seq = 1;
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_ORDER_REPORT, report));
        writer.notify_subscribers();
        wake_strategy();
    }

    void inject_trade_report(DzOrderId order_id, std::string_view trade_id, uint64_t seq) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
        DzTradeReport report{};
        report.order_id = order_id;
        report.trade_id[0] = '\0';
        dztrader::copy_string(report.trade_id, std::string(trade_id).c_str(), true);
        dztrader::copy_string(report.strategy_id, strategy_id_.c_str(), true);
        dztrader::copy_string(report.account_id, "CTP001", true);
        dztrader::copy_string(report.instrument_id, "IF2606", true);
        report.price = 100.5;
        report.volume = 1;
        report.direction = DZ_DIRECTION_SHORT;
        report.position_effect = DZ_POSITION_EFFECT_OPEN;
        report.seq = seq;
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_TRADE_REPORT, report));
        writer.notify_subscribers();
        wake_strategy();
    }

    void inject_position_info(uint64_t seq) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
        DzPositionInfo info{};
        dztrader::copy_string(info.account_id, "CTP001", true);
        dztrader::copy_string(info.instrument_id, "IF2606", true);
        info.direction = DZ_DIRECTION_SHORT;
        info.volume = 3;
        info.frozen_volume = 1;
        info.price = 100.5;
        info.seq = seq;
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_POSITION_INFO, info));
        writer.notify_subscribers();
        wake_strategy();
    }

    void inject_trading_account(double balance, uint64_t seq) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
        DzTradingAccount account{};
        dztrader::copy_string(account.account_id, "CTP001", true);
        account.balance = balance;
        account.available = balance - 100.0;
        account.margin = 100.0;
        account.seq = seq;
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_TRADING_ACCOUNT, account));
        writer.notify_subscribers();
        wake_strategy();
    }

    void inject_account_status(DzAccountStatus status) {
        dztrader::shm::MultiWriter writer =
            dztrader::shm::MultiWriter::create(open_event_meta(), "stg_demo_test_writer");
        EXPECT_TRUE(writer.write_frame(DZ_FRAME_ACCOUNT_STATUS, status));
        writer.notify_subscribers();
        wake_strategy();
    }

    /// 最后一条 [order] action=order 行的 order_id
    std::optional<int64_t> last_order_id() const {
        for (auto it = output_lines_.rbegin(); it != output_lines_.rend(); ++it) {
            if (it->starts_with("[order] ") && it->find("action=order") != std::string::npos) {
                return find_key_number(*it, " order_id=");
            }
        }
        return std::nullopt;
    }

    /// 最近一次 md dump 帧中某合约的 tick 时间序列 (时间正序语义由输出顺序断言)
    std::vector<std::string> last_dump_tick_times(std::string_view instrument) const {
        std::vector<std::string> times;
        for (const auto& frame : output_frames_) {
            if (!frame.starts_with("[md] action=dump")) {
                continue;
            }
            times.clear();
            for (auto& line : split_lines(frame)) {
                const std::string needle = "[tick] instrument=" + std::string(instrument);
                if (!line.starts_with(needle)) {
                    continue;
                }
                const auto time = find_tick_time(line);
                if (time.has_value()) {
                    times.push_back(*time);
                }
            }
        }
        return times;
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_md_source_;
    std::string strategy_id_;
    dztrader::demo::DemoStrategy strategy_;
    std::thread strategy_thread_;
    std::thread rescue_;
    std::atomic<bool> done_{false};
    int32_t exit_code_ = -1;
    std::optional<dztrader::shm::Reader> probe_;
    std::vector<std::string> output_frames_;
    std::vector<std::string> output_lines_;
    /// 策略写出的行情订阅/退订请求帧 (帧头来源 + 解码 payload)
    std::vector<CapturedSubscribe> subscribe_reqs_;
    /// instance_id != 本策略名的 OUTPUT_UI 帧数 (身份边界回归断言)
    std::size_t unexpected_ui_instance_count_ = 0;
};

// ── 就绪横幅与身份 ──

TEST_F(StgDemoTest, ReadyBannerAndInfoExposeIdentity) {
    ASSERT_TRUE(start_strategy()) << dump_output();
    // ready 横幅: 行首 ASCII 类别标签 + 身份 key=value + help 提示
    bool ready_ok = false;
    for (const auto& line : output_lines_) {
        if (!line.starts_with("[ready] ")) {
            continue;
        }
        ready_ok = line.find("strategy=" + strategy_id_) != std::string::npos &&
                   line.find("md_source=test_md") != std::string::npos &&
                   line.find("sdk=") != std::string::npos && line.find("help") != std::string::npos;
    }
    EXPECT_TRUE(ready_ok) << dump_output();

    send_command("info");
    ASSERT_TRUE(wait_until([&] { return count_lines("[info] ", "strategy=") >= 1; }))
        << dump_output();
    // 只认最新一条 info 行, 且 strategy 值必须是本策略名
    EXPECT_TRUE(last_line_contains("[info] ", "strategy=" + strategy_id_)) << dump_output();
    EXPECT_TRUE(last_line_contains("[info] ", "md_source=test_md")) << dump_output();
    EXPECT_TRUE(last_line_contains("[info] ", "home=")) << dump_output();

    // on_start 不自动订阅: 初始订阅集合为空
    send_command("md list");
    ASSERT_TRUE(wait_until([&] { return any_line("[md] ", "action=list"); })) << dump_output();
    EXPECT_TRUE(last_line_contains("[md] action=list", "count=0")) << dump_output();
}

// ── help 命令列表 ──

TEST_F(StgDemoTest, HelpListsEveryCommand) {
    ASSERT_TRUE(start_strategy()) << dump_output();
    send_command("help");
    ASSERT_TRUE(wait_until([&] { return count_lines("[info] ", "command=") >= 10; }))
        << dump_output();
    for (const std::string_view command : {"help", "info", "md add", "md del", "md list", "md dump",
                                           "order", "cancel", "account", "quit"}) {
        EXPECT_TRUE(any_line("[info] command=", command))
            << "missing help line for: " << command << "\n"
            << dump_output();
    }
}

// ── 非法命令/参数: error 行且进程不崩溃 ──

TEST_F(StgDemoTest, InvalidInputEmitsErrorButKeepsRunning) {
    ASSERT_TRUE(start_strategy()) << dump_output();

    send_command("");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=empty_command"); }))
        << dump_output();

    send_command("md");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=missing_subcommand"); }))
        << dump_output();

    send_command("bogus");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "cmd=bogus"); })) << dump_output();
    EXPECT_TRUE(any_line("[error] ", "reason=unknown_command")) << dump_output();

    send_command("md add");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=missing_instrument"); }))
        << dump_output();

    send_command("md dump abc");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_count"); }))
        << dump_output();

    send_command("order CTP001 IF2606 notanumber 1");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_price"); }))
        << dump_output();

    send_command("order CTP001 IF2606 100 1 sideways");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_direction"); }))
        << dump_output();

    send_command("order CTP001 IF2606 100 0");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_volume"); }))
        << dump_output();

    send_command("order CTP001 IF2606 100 1 long extra");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_arguments"); }))
        << dump_output();

    send_command("info extra");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=unexpected_args"); }))
        << dump_output();

    send_command("md dump 0");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_count"); }))
        << dump_output();

    send_command("md dump -1");
    ASSERT_TRUE(wait_until([&] { return count_lines("[error] ", "reason=invalid_count") >= 2; }))
        << dump_output();

    send_command("cancel CTP001 notanumber");
    ASSERT_TRUE(wait_until([&] { return any_line("[error] ", "reason=invalid_order_id"); }))
        << dump_output();

    // 进程仍响应: 非法输入不破坏事件循环
    send_command("info");
    ASSERT_TRUE(wait_until([&] { return count_lines("[info] ", "strategy=") >= 1; }))
        << dump_output();
}

// ── 未订阅合约: tick 不缓存, md del 仍回执 ──

TEST_F(StgDemoTest, UnsubscribedInstrumentTicksAreNotCached) {
    ASSERT_TRUE(start_strategy()) << dump_output();

    // on_start 不自动订阅; 未订阅合约的 tick 到达后不得进入缓存
    push_ticks_descending("IF9999", 3);
    std::string dump;
    ASSERT_TRUE(request_dump_until(
        "md dump", [](const std::string& frame) { return find_key_number(frame, " count=") == 0; },
        dump))
        << dump_output();
    EXPECT_EQ(dump.find("instrument=IF9999"), std::string::npos) << dump;

    // 退订一个从未订阅的合约: 仍写退订请求并回执, 订阅集合保持为空
    send_command("md del IF9999");
    ASSERT_TRUE(wait_until([&] {
        return last_line_contains("[md] action=del", "IF9999") &&
               saw_subscribe_action(dztrader::SubscribeAction::Unsubscribe, "IF9999");
    })) << dump_output();
    send_command("md list");
    ASSERT_TRUE(wait_until([&] { return any_line("[md] ", "action=list"); })) << dump_output();
    EXPECT_TRUE(last_line_contains("[md] action=list", "count=0")) << dump_output();
}

// ── CTP DBL_MAX 哨兵价格 → "-" (不输出 300+ 位数字) ──

TEST_F(StgDemoTest, SentinelPricesRenderAsDash) {
    ASSERT_TRUE(start_strategy()) << dump_output();
    send_command("md add IF2606");
    ASSERT_TRUE(wait_until([&] { return any_line("[md] ", "action=add"); })) << dump_output();
    push_sentinel_price_tick();

    std::string dump;
    ASSERT_TRUE(request_dump_until(
        "md dump 1",
        [](const std::string& frame) { return find_key_number(frame, " count=") == 1; }, dump))
        << dump_output();
    EXPECT_NE(dump.find("bid1=-/7"), std::string::npos) << dump;
    EXPECT_NE(dump.find("ask1=-/8"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("1.79769"), std::string::npos) << dump;
}

// ── md 订阅集合 + tick 环形缓存 + dump 时间正序 + del 清缓存 ──

TEST_F(StgDemoTest, MdCacheDumpAndDelLifecycle) {
    constexpr int kCap = static_cast<int>(dztrader::demo::DemoStrategy::kTickCacheCapacity);
    ASSERT_TRUE(start_strategy()) << dump_output();

    send_command("md add IF2606 IF2609");
    ASSERT_TRUE(wait_until([&] {
        return any_line("[md] ", "action=add") && any_line("[md] ", "IF2606") &&
               any_line("[md] ", "IF2609");
    })) << dump_output();
    // 帧边界: 订阅请求已写出 (md add 不只是本地状态)
    ASSERT_TRUE(wait_until([&] {
        return saw_subscribe_action(dztrader::SubscribeAction::Subscribe, "IF2606") &&
               saw_subscribe_action(dztrader::SubscribeAction::Subscribe, "IF2609");
    })) << dump_output();
    EXPECT_TRUE(subscribe_identity_all_valid()) << dump_output();

    send_command("md list");
    ASSERT_TRUE(wait_until([&] { return any_line("[md] ", "action=list"); })) << dump_output();
    // 只看最新一条 list 行 (之前的 add 行也含合约名, 不能用来证明 list 正确)
    EXPECT_TRUE(last_line_contains("[md] action=list", "IF2606")) << dump_output();
    EXPECT_TRUE(last_line_contains("[md] action=list", "IF2609")) << dump_output();

    // IF2606 注入 kCap+5 条 (时间逆序), 验证每合约上限 100 的环形淘汰 + dump 重排
    push_ticks_descending("IF2606", kCap + 5);
    push_ticks_descending("IF2609", 5);

    std::string dump;
    ASSERT_TRUE(request_dump_until(
        "md dump",
        [kCap](const std::string& frame) { return find_key_number(frame, " count=") == kCap + 5; },
        dump))
        << dump_output();
    // 最旧 5 条按到达顺序被挤掉: IF2606 保留时间 0..99, dump 输出必须时间正序
    const std::vector<std::string> all_times = last_dump_tick_times("IF2606");
    ASSERT_EQ(all_times.size(), static_cast<std::size_t>(kCap)) << dump;
    EXPECT_EQ(all_times.front(), "00:00:00.000000") << dump;
    EXPECT_EQ(all_times.back(), "00:01:39.000000") << dump;
    EXPECT_TRUE(std::is_sorted(all_times.begin(), all_times.end())) << dump;
    const std::vector<std::string> other_times = last_dump_tick_times("IF2609");
    ASSERT_EQ(other_times.size(), 5U) << dump;
    EXPECT_TRUE(std::is_sorted(other_times.begin(), other_times.end())) << dump;

    // n 限制每合约最近 n 条: 两合约各 3 条 → 单帧 count=6, IF2606 为 97/98/99 秒
    ASSERT_TRUE(request_dump_until(
        "md dump 3",
        [](const std::string& frame) { return find_key_number(frame, " count=") == 6; }, dump))
        << dump_output();
    const std::vector<std::string> last3 = last_dump_tick_times("IF2606");
    ASSERT_EQ(last3.size(), 3U) << dump;
    EXPECT_EQ(last3[0], "00:01:37.000000") << dump;
    EXPECT_EQ(last3[1], "00:01:38.000000") << dump;
    EXPECT_EQ(last3[2], "00:01:39.000000") << dump;

    // del: 退订帧已写出 + 丢弃该合约缓存; 其他合约缓存保留
    send_command("md del IF2606");
    ASSERT_TRUE(wait_until([&] {
        return any_line("[md] ", "action=del") &&
               saw_subscribe_action(dztrader::SubscribeAction::Unsubscribe, "IF2606");
    })) << dump_output();
    ASSERT_TRUE(request_dump_until(
        "md dump", [](const std::string& frame) { return find_key_number(frame, " count=") == 5; },
        dump))
        << dump_output();
    EXPECT_EQ(dump.find("instrument=IF2606"), std::string::npos) << dump;
    EXPECT_NE(dump.find("instrument=IF2609"), std::string::npos) << dump;

    send_command("md list");
    ASSERT_TRUE(wait_until([&] {
        return count_lines("[md] ", "action=list") >= 2 &&
               !last_line_contains("[md] action=list", "IF2606") &&
               last_line_contains("[md] action=list", "IF2609");
    })) << dump_output();
}

// ── order / cancel / account 命令 + 回报帧转文本行 ──

TEST_F(StgDemoTest, TradingCommandsAndReportsBecomeUiLines) {
    ASSERT_TRUE(start_strategy()) << dump_output();

    send_command("order CTP001 IF2606 100.5 2 short");
    ASSERT_TRUE(wait_until([&] { return any_line("[order] ", "action=order"); })) << dump_output();
    ASSERT_TRUE(last_order_id().has_value()) << dump_output();
    const int64_t order_id = *last_order_id();
    EXPECT_TRUE(any_line("[order] ", "direction=short(卖)")) << dump_output();

    // 空账户拒单回报 (basic 帧, strategy_id 定向本策略) → report 文本行带枚举锚点
    inject_order_report(static_cast<DzOrderId>(order_id), DZ_ORDER_REJECTED);
    ASSERT_TRUE(wait_until([&] { return any_line("[report] ", "status=REJECTED(拒单)"); }))
        << dump_output();
    EXPECT_TRUE(any_line("[report] ", "order_id=" + std::to_string(order_id))) << dump_output();
    EXPECT_TRUE(any_line("[report] ", "traded=0")) << dump_output();

    // 定向过滤负例: 他策略的 ORDER_REPORT 不得转成本策略的 [report] 行
    inject_order_report(static_cast<DzOrderId>(order_id) + 1, DZ_ORDER_REJECTED, "other_strategy");
    send_command("info");  // 握手: 确保事件泵已处理注入帧
    ASSERT_TRUE(wait_until([&] { return any_line("[info] ", "strategy=" + strategy_id_); }))
        << dump_output();
    EXPECT_TRUE(!any_line("[report] ", "order_id=" + std::to_string(order_id + 1)))
        << dump_output();

    // 成交/持仓/资金异步推送 → 各自的文本行 (父 spec 保留的调试能力)
    inject_trade_report(static_cast<DzOrderId>(order_id), "T0001", 2);
    ASSERT_TRUE(wait_until([&] { return any_line("[trade] ", "trade_id=T0001"); }))
        << dump_output();
    EXPECT_TRUE(any_line("[trade] ", "price=100.500")) << dump_output();
    inject_position_info(3);
    ASSERT_TRUE(wait_until([&] { return any_line("[position] ", "volume=3"); })) << dump_output();
    EXPECT_TRUE(any_line("[position] ", "instrument=IF2606")) << dump_output();
    inject_trading_account(12345.0, 4);
    ASSERT_TRUE(wait_until([&] { return any_line("[account] ", "action=fund"); })) << dump_output();
    EXPECT_TRUE(any_line("[account] ", "balance=12345.00")) << dump_output();

    send_command("cancel CTP001 " + std::to_string(order_id));
    ASSERT_TRUE(wait_until([&] { return any_line("[order] ", "action=cancel"); })) << dump_output();
    EXPECT_TRUE(last_line_contains("[order] action=cancel", "order_id=" + std::to_string(order_id)))
        << dump_output();

    send_command("account CTP001");
    ASSERT_TRUE(wait_until([&] { return any_line("[account] ", "action=query"); }))
        << dump_output();
    send_command("account");
    ASSERT_TRUE(wait_until([&] { return any_line("[account] ", "account=all"); })) << dump_output();

    // 账户状态广播帧 → account status 文本行
    DzAccountStatus status{};
    dztrader::copy_string(status.account_id, "CTP001", true);
    dztrader::copy_string(status.gateway_name, "dztd_ctp", true);
    status.state = DZ_ACCOUNT_OFFLINE;
    inject_account_status(status);
    ASSERT_TRUE(wait_until([&] { return any_line("[account] ", "state=offline(离线)"); }))
        << dump_output();
    EXPECT_TRUE(any_line("[account] ", "action=status")) << dump_output();
}

// ── quit 自主退出: exit 0 + on_stop 收尾输出 ──

TEST_F(StgDemoTest, QuitExitsZeroWithStopOutput) {
    ASSERT_TRUE(start_strategy()) << dump_output();
    send_command("quit");
    ASSERT_TRUE(wait_until([&] { return any_line("[info] ", "action=quit"); })) << dump_output();
    join_strategy();
    pump_output();
    EXPECT_EQ(exit_code_, 0) << dump_output();
    EXPECT_TRUE(any_line("[stop] ", "reason=self")) << dump_output();
}

// ── dz_init 失败: 无会话的 on_error 经 SDK 诊断通道保留错误 (不再只剩退出码) ──

class StgDemoInitFailureTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 与主 fixture 相同的进程级缓存的 home 路径 (paths::shm 进程内缓存),
        // 但故意不创建事件/行情通道 -> dz_init 必失败
        home_ = std::filesystem::temp_directory_path() /
                ("dz_test_stg_demo_" + std::to_string(dztrader::this_process::pid()));
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        std::filesystem::create_directories(home_ / "shm", ec);
        ASSERT_FALSE(ec) << "create temp home failed: " << ec.message();
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        orig_md_source_ = dztrader::env::get("DZTRADER_MD_SOURCE");
        dztrader::env::set("DZTRADER_HOME", home_.string());
        dztrader::paths::reset_for_test();  // 清空进程级缓存, 指向本用例 home
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        if (orig_md_source_) {
            dztrader::env::set("DZTRADER_MD_SOURCE", *orig_md_source_);
        } else {
            dztrader::env::unset("DZTRADER_MD_SOURCE");
        }
        dztrader::paths::reset_for_test();
    }

    std::filesystem::path home_;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_md_source_;
};

TEST_F(StgDemoInitFailureTest, InitFailureIsDiagnosedOnStderr) {
    // 在进程边界验证 —— 与 master 捕获策略 stderr 的真实路径一致:
    // 无事件通道 -> dz_init 失败 -> on_error 写 stderr 诊断后非零退出。
    // boost::process::v2 直接把子进程 stderr 重定向到文件 (不经 shell, 跨平台)。
    const auto err_path = home_ / "stderr.txt";
    std::FILE* err_file = std::fopen(err_path.string().c_str(), "wb");
    ASSERT_NE(err_file, nullptr) << "open " << err_path << " failed";
    boost::asio::io_context ioc;
    boost::process::v2::process demo(
        ioc, demo_exe_path().string(), {},
        boost::process::v2::process_stdio{.in = nullptr, .out = nullptr, .err = err_file});
    const int rc = demo.wait();
    std::fclose(err_file);
    EXPECT_NE(rc, 0) << "stg_demo should exit non-zero when dz_init fails";

    std::ifstream err_stream(err_path, std::ios::binary);
    ASSERT_TRUE(err_stream) << "open " << err_path << " failed";
    const std::string diagnostics((std::istreambuf_iterator<char>(err_stream)),
                                  std::istreambuf_iterator<char>());
    EXPECT_NE(diagnostics.find("strategy init failed"), std::string::npos) << diagnostics;
    EXPECT_NE(diagnostics.find("code="), std::string::npos) << diagnostics;
    const auto message_pos = diagnostics.find("message=\"");
    ASSERT_NE(message_pos, std::string::npos) << diagnostics;
    constexpr std::string_view kMessagePrefix = "message=\"";
    ASSERT_GT(diagnostics.size(), message_pos + kMessagePrefix.size()) << diagnostics;
    // 非空 message: 起始引号之后不应立即是结束引号
    EXPECT_NE(diagnostics[message_pos + kMessagePrefix.size()], '"') << diagnostics;
}

// ── SHUTDOWN (master 停止路径): exit 0 + on_stop 收尾输出 ──

TEST_F(StgDemoTest, ShutdownFrameExitsZeroWithStopOutput) {
    ASSERT_TRUE(start_strategy()) << dump_output();
    write_shutdown_frame();
    wake_strategy();
    join_strategy();
    pump_output();
    EXPECT_EQ(exit_code_, 0) << dump_output();
    EXPECT_TRUE(any_line("[stop] ", "reason=shutdown")) << dump_output();
}

}  // namespace
