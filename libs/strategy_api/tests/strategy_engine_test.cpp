#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/writer.h>
#include <dztrader/strategy_engine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_temp_dir.h"

namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;

constexpr uint64_t kMB = 1024 * 1024;

void create_event_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

void create_md_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        // NOLINT(temp-dir-unique): test_md 是 SDK 约定的行情通道名，其 shm_dir 每用例唯一
        .channel_name = "test_md", .shm_dir = shm_dir,   .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,      .lock_memory = false, .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

/// 引擎测试: 临时 DZTRADER_HOME + 预建事件/md 通道。run_strategy 内部自行
/// dz_init/dz_release, 测试体只准备环境并写帧。
///
/// 唤醒机制说明: 测试进程不是经 master 启动, 事件通道订阅者列表为空,
/// writer.notify_subscribers() 唤不醒策略进程的 dz_wait。因此多数测试的
/// Hook 策略在 on_start 里排 dz_schedule_after 定时器, dz_wait 以
/// wait_for(超时) 模式等待, 到期后 on_schedule 主动写 SHUTDOWN 帧退出。
/// rescue 线程仅作防挂死兜底 (正常路径不触发); 无定时器的停止信号路径由
/// ShutdownWakesBlockedWaitWithoutPendingTimers 直连信号量 notify 覆盖。
/// 另外: 写帧 helper 不得使用 gtest 断言宏 (致命断言在非主线程是 UB)。
class StrategyEngineTest : public ::testing::Test {
protected:
    std::string home_;
    std::thread rescue_thread_;
    std::atomic<bool> done_{false};

    void SetUp() override {
        home_ = dztrader::test::unique_temp_dir("dz_test_strategy_engine").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");
    }

    void TearDown() override {
        join_threads();
        std::filesystem::remove_all(home_);
    }

    void join_threads() {
        done_.store(true);
        if (rescue_thread_.joinable()) {
            rescue_thread_.join();
        }
    }

    std::shared_ptr<ChannelMeta> open_event_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    std::shared_ptr<ChannelMeta> open_md_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only("test_md", home_ + "/shm"));
    }

public:
    /// 事件通道 meta (HookStrategy 策略侧写回报帧用)
    std::shared_ptr<ChannelMeta> event_channel_meta() { return open_event_meta(); }

    /// 写一帧 md tick。无 gtest 断言: 可在任意线程调用。
    bool write_tick(double last_price) {
        MultiWriter writer = MultiWriter::create(open_md_meta(), "engine_test_md_writer");
        DzTick tick{};
        tick.last_price = last_price;
        return writer.write_frame(DZ_FRAME_TICK, tick);
    }

    /// 写定向本策略的 SHUTDOWN 帧。无 gtest 断言: 可在任意线程调用。
    bool write_shutdown(std::string_view strategy_id) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "engine_test_writer");
        const bool ok =
            writer.write_ext_inst_frame(DZ_FRAME_SHUTDOWN, strategy_id.data(), nullptr, 0);
        writer.notify_subscribers();
        return ok;
    }

    /// 防挂死兜底: timeout_ms 后强制写 SHUTDOWN 并唤醒策略信号量
    /// (正常路径策略侧自退出, 不触发)。策略 ID 由 exe_stem 直接计算
    /// (dz_init 身份 = 可执行文件名), 不依赖 on_start 时才写入的局部变量 --
    /// 否则调用点(均在 run_strategy 之前)传入空串, 兜底会静默失效并挂到 ctest 超时。
    /// 信号量名与 DzContext 构造一致 (strategy_identity: "stg." + exe_stem)。
    void start_rescue(int timeout_ms) { start_rescue_impl(timeout_ms, nullptr); }

    /// 停止信号测试专用: 等 on_start 置位 started 后再写 SHUTDOWN + notify。
    /// reader 在 dz_init 中才建立 (从写尾开始), 固定延时可能早于 reader 就绪,
    /// SHUTDOWN 落在读位置之前永不可见 → 挂到 ctest 超时。started 标志消除该竞态。
    void start_shutdown_after_start(const std::atomic<bool>& started) {
        start_rescue_impl(0, &started);
    }

private:
    /// 共用线程体: started == nullptr 按 timeout_ms 延时兜底; 否则等 started 置位。
    void start_rescue_impl(int timeout_ms, const std::atomic<bool>* started) {
        rescue_thread_ = std::thread([this, timeout_ms, started] {
            if (started != nullptr) {
                while (!started->load() && !done_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } else {
                for (int i = 0; i < timeout_ms / 50; ++i) {
                    if (done_.load()) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            const std::string strategy_id = dztrader::this_process::exe_stem();
            if (!write_shutdown(strategy_id)) {
                return;  // 写帧失败: 下面 notify 也无意义
            }
            try {
                dztrader::shm::NamedSemaphore sem("stg." + strategy_id);
                sem.notify();  // 直连策略信号量: 兜底路径也必须能唤醒 dz_wait
            } catch (...) {
                // 信号量打开失败: 仅写帧, 引擎有 pending 定时器时可自行超时退出
            }
        });
    }
};

// ── concept 约束 (编译期, 无需 fixture) ──

class FullStrategy {
public:
    void on_start(DzContext*) { ++start_count; }
    void on_stop() { ++stop_count; }
    void on_tick(const DzTick& tick) {
        last_price = tick.last_price;
        ++tick_count;
    }
    void on_trade_report(const DzTradeReport& rpt) {
        trade_id = std::string(rpt.trade_id);
        trade_price = rpt.price;
        trade_volume = rpt.volume;
        ++trade_count;
    }
    void on_order_report(const DzOrderReport& rpt) {
        order_id = rpt.order_id;
        order_price = rpt.price;
        order_volume = rpt.volume;
        ++order_count;
    }
    void on_position_info(const DzPositionInfo& info) {
        last_position_instrument = std::string(info.instrument_id);
        position_volume = info.volume;
        position_frozen = info.frozen_volume;
        position_price = info.price;
        ++position_count;
    }
    void on_trading_account(const DzTradingAccount& account) {
        last_account_id = std::string(account.account_id);
        account_balance = account.balance;
        account_available = account.available;
        ++account_count;
    }
    void on_schedule(const DzScheduleEvent& ev) {
        ++schedule_count;
        timer_id = ev.timer_id;
    }
    void on_ui_input(std::string_view data) {
        ++input_count;
        last_input = std::string(data);
    }
    void on_error(DzErrorCode code, std::string_view message) {
        ++error_count;
        error_code = code;
        error_message = std::string(message);
    }
    void on_account_status(const DzAccountStatus& status) {
        last_account_status = status;
        ++account_status_count;
    }

    int start_count = 0;
    int tick_count = 0;
    int stop_count = 0;
    int trade_count = 0;
    int order_count = 0;
    int position_count = 0;
    int account_count = 0;
    int schedule_count = 0;
    int input_count = 0;
    int error_count = 0;
    int account_status_count = 0;
    double last_price = 0.0;
    std::string trade_id;
    double trade_price = 0.0;
    DzVolume trade_volume = 0;
    int64_t order_id = -1;
    double order_price = 0.0;
    DzVolume order_volume = 0;
    std::string last_position_instrument;
    DzLargeVolume position_volume = 0;
    DzLargeVolume position_frozen = 0;
    double position_price = 0.0;
    std::string last_account_id;
    double account_balance = 0.0;
    double account_available = 0.0;
    DzTimerId timer_id = 0;
    std::string last_input;
    DzErrorCode error_code = 0;
    std::string error_message;
    DzAccountStatus last_account_status{};
};

static_assert(dztrader::Strategy<FullStrategy>);
static_assert(dztrader::Strategy<dztrader::StrategyBase>);

// concept 约束 (on_start 必选, on_tick 可选; 拼错的可选回调由启动自检行暴露)
namespace {
struct MissingOnStart {
    void on_tick(const DzTick&) {}
};
struct WithoutOnTick {
    void on_start(DzContext*) {}
};
struct MisspelledTick {
    void on_start(DzContext*) {}
    void onTick(const DzTick&) {}  // 驼峰拼写错误: on_tick 探测为否, 不再是编译错误
};
static_assert(!dztrader::Strategy<MissingOnStart>);
static_assert(dztrader::Strategy<WithoutOnTick>);
static_assert(!dztrader::strategy_engine_internal::HasOnTick<WithoutOnTick>);
static_assert(dztrader::Strategy<MisspelledTick>);
static_assert(!dztrader::strategy_engine_internal::HasOnTick<MisspelledTick>);
}  // namespace

// exit_code 只接受精确 int32_t (拒绝 bool/int64 等可转换类型导致的静默收窄)
namespace {
struct BadExitCodeBool {
    void on_start(DzContext*) {}
    bool exit_code() const noexcept { return true; }
};
struct BadExitCodeInt64 {
    void on_start(DzContext*) {}
    int64_t exit_code() const noexcept { return 0; }
};
struct GoodExitCode {
    void on_start(DzContext*) {}
    int32_t exit_code() const noexcept { return 0; }
};
static_assert(!dztrader::strategy_engine_internal::HasExitCode<BadExitCodeBool>);
static_assert(!dztrader::strategy_engine_internal::HasExitCode<BadExitCodeInt64>);
static_assert(dztrader::strategy_engine_internal::HasExitCode<GoodExitCode>);
}  // namespace

// ── run_strategy 端到端 ──
//
// 策略 ID = 测试可执行文件 exe_stem, dz_init 以 exe_stem 为进程身份,
// SHUTDOWN 定向帧必须以它为 instance_id。on_start 回调在 run_strategy 内
// 执行, 此时从 dz_strategy_id(ctx) 取真实策略 ID。

TEST_F(StrategyEngineTest, RunStrategyDispatchesMdTickThenExitsOnShutdown) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> tick_written{false};
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& tick_written;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            // 100ms 后 on_schedule 主动退出; dz_wait 期间为定时器超时等待
            (void)dz_schedule_after(ctx, 100);
            // dz_init 已完成, md reader 定位就绪: 此刻写的 tick 才能被读到
            tick_written.store(fixture.write_tick(1234.5));
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, tick_written, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_TRUE(tick_written.load());
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.tick_count, 1);
    EXPECT_DOUBLE_EQ(strategy.last_price, 1234.5);
    EXPECT_EQ(strategy.stop_count, 1);
    EXPECT_EQ(strategy.schedule_count, 1);
}

TEST_F(StrategyEngineTest, MinimalStrategyStillExitsCleanlyWithoutOptionalCallbacks) {
    struct MinimalStrategy {
        void on_start(DzContext*) { ++start_count; }
        void on_tick(const DzTick&) { ++tick_count; }
        int start_count = 0;
        int tick_count = 0;
    };
    static_assert(dztrader::Strategy<MinimalStrategy>);

    MinimalStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        MinimalStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_schedule(const DzScheduleEvent&) {
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
        // Hook 未实现 on_stop/on_error: 引擎编译期探测应跳过, 不崩溃
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(!dztrader::strategy_engine_internal::HasOnStop<HookStrategy>);
    static_assert(!dztrader::strategy_engine_internal::HasOnError<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.tick_count, 0);  // 未写 tick 帧, on_tick 不被调
    EXPECT_TRUE(shutdown_written.load());
}

// 停止信号路径: 无 pending 定时器时 dz_wait 阻塞在策略信号量上; 定向 SHUTDOWN 帧
// + 信号量 notify 必须唤醒并退出。其余主循环用例都先排 100ms 定时器, 覆盖不到该路径
// (帧到达不唤醒 → 定时器仍会兜底退出, 回归不可见)。
TEST_F(StrategyEngineTest, ShutdownWakesBlockedWaitWithoutPendingTimers) {
    FullStrategy strategy;
    std::atomic<bool> started{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::atomic<bool>& started;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            started.store(true);  // reader/信号量已就绪, 通知测试线程写 SHUTDOWN
            // 不排任何定时器: 下一条 dz_wait 阻塞在信号量上
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, started};

    start_shutdown_after_start(started);  // on_start 后写 SHUTDOWN + notify 策略信号量
    const auto begin = std::chrono::steady_clock::now();
    const int32_t rc = dztrader::run_strategy(hook);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.tick_count, 0);
    EXPECT_EQ(strategy.stop_count, 1);            // on_stop 只调一次
    EXPECT_LT(elapsed, std::chrono::seconds(5));  // 无定时器也能被 SHUTDOWN 唤醒
}

TEST_F(StrategyEngineTest, ScheduleCallbackReceivesDzScheduleEventPayload) {
    struct TimerStrategy {
        void on_start(DzContext*) { ++start_count; }
        void on_tick(const DzTick&) {}
        void on_schedule(const DzScheduleEvent& ev) {
            ++schedule_count;
            fired_timer_id = ev.timer_id;
        }
        int start_count = 0;
        int schedule_count = 0;
        DzTimerId fired_timer_id = 0;
    };
    static_assert(dztrader::Strategy<TimerStrategy>);

    TimerStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        TimerStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;
        DzTimerId scheduled_id = DZ_TIMER_INVALID;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            // 50ms 一次性定时器: on_schedule 收 DzScheduleEvent 后写 SHUTDOWN 结束
            scheduled_id = dz_schedule_after(ctx, 50);
        }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnSchedule<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_NE(hook.scheduled_id, DZ_TIMER_INVALID);
    EXPECT_EQ(strategy.schedule_count, 1);
    EXPECT_EQ(strategy.fired_timer_id, hook.scheduled_id);
    EXPECT_TRUE(shutdown_written.load());
}

TEST_F(StrategyEngineTest, ExceptionInOnTickIsCaughtAndEngineKeepsRunning) {
    struct ThrowingStrategy {
        void on_start(DzContext*) { ++start_count; }
        void on_tick(const DzTick&) {
            ++tick_count;
            throw std::runtime_error("boom in on_tick");
        }
        void on_error(DzErrorCode code, std::string_view message) {
            ++error_count;
            last_error_code = code;
            last_error_message = std::string(message);
        }
        int start_count = 0;
        int tick_count = 0;
        int error_count = 0;
        DzErrorCode last_error_code = 0;
        std::string last_error_message;
    };
    static_assert(dztrader::Strategy<ThrowingStrategy>);

    ThrowingStrategy strategy;
    std::string strategy_id;

    struct HookStrategy {
        ThrowingStrategy& inner;
        std::string& strategy_id;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            (void)fixture.write_tick(99.0);
        }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_error(DzErrorCode code, std::string_view message) { inner.on_error(code, message); }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    // on_tick 抛异常不终止进程: 引擎捕获后经 on_error 报告, SHUTDOWN 正常退出
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.tick_count, 1);
    EXPECT_EQ(strategy.error_count, 1);
    EXPECT_EQ(strategy.last_error_code, DZ_EC_INTERNAL);
    EXPECT_NE(strategy.last_error_message.find("boom"), std::string::npos);
}

// dispatch_ui_input 有独立 try/catch 边界 (strategy_engine.h): on_ui_input 抛异常
// 须经 on_error 报告 DZ_EC_INTERNAL, 引擎继续运行并正常退出。
TEST_F(StrategyEngineTest, ExceptionInOnUiInputIsCaughtAndEngineKeepsRunning) {
    struct ThrowingStrategy {
        void on_start(DzContext*) { ++start_count; }
        void on_ui_input(std::string_view) {
            ++input_count;
            throw std::runtime_error("boom");  // 不含回调名: 断言引擎自己带 callback=
        }
        void on_error(DzErrorCode code, std::string_view message) {
            ++error_count;
            last_error_code = code;
            last_error_message = std::string(message);
        }
        int start_count = 0;
        int input_count = 0;
        int error_count = 0;
        DzErrorCode last_error_code = 0;
        std::string last_error_message;
    };
    static_assert(dztrader::Strategy<ThrowingStrategy>);

    ThrowingStrategy strategy;
    std::string strategy_id;

    struct HookStrategy {
        ThrowingStrategy& inner;
        std::string& strategy_id;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            const char* payload = R"({"action":"pause"})";
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, strategy_id.c_str(),
                                              reinterpret_cast<const std::byte*>(payload),
                                              static_cast<uint32_t>(std::strlen(payload)));
        }
        void on_ui_input(std::string_view data) { inner.on_ui_input(data); }
        void on_error(DzErrorCode code, std::string_view message) { inner.on_error(code, message); }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.input_count, 1);
    EXPECT_EQ(strategy.error_count, 1);
    EXPECT_EQ(strategy.last_error_code, DZ_EC_INTERNAL);
    EXPECT_NE(strategy.last_error_message.find("callback=on_ui_input"), std::string::npos);
}

TEST_F(StrategyEngineTest, ExceptionInOnErrorDoesNotEscapeEngine) {
    struct BadErrorStrategy {
        void on_start(DzContext*) { ++start_count; }
        void on_tick(const DzTick&) {
            ++tick_count;
            throw std::runtime_error("primary failure");
        }
        void on_error(DzErrorCode, std::string_view) {
            ++error_count;
            throw std::runtime_error("error handler itself throws");
        }
        int start_count = 0;
        int tick_count = 0;
        int error_count = 0;
    };
    static_assert(dztrader::Strategy<BadErrorStrategy>);

    BadErrorStrategy strategy;
    std::string strategy_id;

    struct HookStrategy {
        BadErrorStrategy& inner;
        std::string& strategy_id;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            (void)fixture.write_tick(1.0);
        }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_error(DzErrorCode code, std::string_view message) { inner.on_error(code, message); }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    // on_error 自身抛异常: report_error noexcept 兜底, 异常不逃逸, 进程正常退出
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.tick_count, 1);
    EXPECT_EQ(strategy.error_count, 1);
}

TEST_F(StrategyEngineTest, ExceptionInOnStartReleasesSessionAndReturnsError) {
    struct ThrowingStartStrategy {
        void on_start(DzContext*) { throw std::runtime_error("boom in on_start"); }
        void on_tick(const DzTick&) {}
        void on_error(DzErrorCode code, std::string_view message) {
            ++error_count;
            last_error_code = code;
            last_error_message = std::string(message);
        }
        int error_count = 0;
        DzErrorCode last_error_code = 0;
        std::string last_error_message;
    };
    static_assert(dztrader::Strategy<ThrowingStartStrategy>);

    ThrowingStartStrategy strategy;
    std::string strategy_id;

    // on_start 抛异常 = 生命周期失败: 引擎应释放会话并返回 DZ_EC_INTERNAL,
    // 不进入主循环 (on_tick 永不触发, 亦无需 SHUTDOWN 退出)
    struct HookStrategy {
        ThrowingStartStrategy& inner;
        std::string& strategy_id;
        int rogue_tick_count = 0;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            inner.on_start(ctx);
        }
        void on_tick(const DzTick&) { ++rogue_tick_count; }
        void on_error(DzErrorCode code, std::string_view message) { inner.on_error(code, message); }
    };

    HookStrategy hook{strategy, strategy_id};
    const int32_t rc = dztrader::run_strategy(hook);

    EXPECT_EQ(rc, DZ_EC_INTERNAL);
    EXPECT_EQ(strategy.error_count, 1);
    EXPECT_EQ(strategy.last_error_code, DZ_EC_INTERNAL);
    EXPECT_NE(strategy.last_error_message.find("boom in on_start"), std::string::npos);
    EXPECT_EQ(hook.rogue_tick_count, 0);  // 未进入主循环

    // 会话已释放: 同进程可再次 dz_init 成功 (证明 dz_release 已执行)
    DzContext* ctx = dz_init();
    ASSERT_NE(ctx, nullptr) << "session should be releasable after on_start failure";
    dz_release(ctx);
}

TEST_F(StrategyEngineTest, TradeAndOrderReportsDispatchedWithPayload) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            // TD 回报帧走事件通道 (basic struct 帧, 写法同 trade_api_test)
            // SDK 按 payload strategy_id 定向过滤, 须填本策略裸名才放行;
            // 且各帧须带递增 seq (账户级 ingest 去重按 (account_id, seq), 同值会被判已应用)
            DzTradeReport trade{};
            trade.price = 3888.5;
            trade.volume = 7;
            trade.seq = 1;
            dztrader::copy_string(trade.trade_id, "T-3888", true);
            dztrader::copy_string(trade.strategy_id, strategy_id.c_str(), true);
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_frame(DZ_FRAME_TRADE_REPORT, trade);
            DzOrderReport order{};
            order.order_id = 42;
            order.price = 3901.0;
            order.volume = 3;
            order.seq = 2;
            dztrader::copy_string(order.strategy_id, strategy_id.c_str(), true);
            (void)writer.write_frame(DZ_FRAME_ORDER_REPORT, order);
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_trade_report(const DzTradeReport& rpt) { inner.on_trade_report(rpt); }
        void on_order_report(const DzOrderReport& rpt) { inner.on_order_report(rpt); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnTradeReport<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnOrderReport<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.trade_count, 1);
    EXPECT_EQ(strategy.trade_id, "T-3888");
    EXPECT_DOUBLE_EQ(strategy.trade_price, 3888.5);
    EXPECT_EQ(strategy.trade_volume, 7);
    EXPECT_EQ(strategy.order_count, 1);
    EXPECT_EQ(strategy.order_id, 42);
    EXPECT_DOUBLE_EQ(strategy.order_price, 3901.0);
    EXPECT_EQ(strategy.order_volume, 3);
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

TEST_F(StrategyEngineTest, PositionAndAccountFramesDispatchedUnfiltered) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            // DZ_FRAME_POSITION_INFO/DZ_FRAME_TRADING_ACCOUNT 为全量透传帧 (无策略过滤): 不填
            // strategy_id 也应分发, 写法同 TradeAndOrderReportsDispatchedWithPayload (basic struct
            // 帧); 各帧须带递增 seq (账户级 ingest 去重按 (account_id, seq), 同值会被判已应用)
            DzPositionInfo info{};
            info.seq = 1;
            dztrader::copy_string(info.account_id, "CTP001", true);
            dztrader::copy_string(info.instrument_id, "IF2603", true);
            info.direction = DZ_DIRECTION_LONG;
            info.volume = 5;
            info.frozen_volume = 2;
            info.price = 3950.5;
            DzTradingAccount account{};
            account.seq = 2;
            dztrader::copy_string(account.account_id, "CTP001", true);
            account.balance = 100000.0;
            account.available = 80000.0;
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_frame(DZ_FRAME_POSITION_INFO, info);
            (void)writer.write_frame(DZ_FRAME_TRADING_ACCOUNT, account);
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_position_info(const DzPositionInfo& info) { inner.on_position_info(info); }
        void on_trading_account(const DzTradingAccount& account) {
            inner.on_trading_account(account);
        }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnPositionInfo<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnTradingAccount<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.position_count, 1);
    EXPECT_EQ(strategy.last_position_instrument, "IF2603");
    EXPECT_EQ(strategy.position_volume, 5);
    EXPECT_EQ(strategy.position_frozen, 2);
    EXPECT_DOUBLE_EQ(strategy.position_price, 3950.5);
    EXPECT_EQ(strategy.account_count, 1);
    EXPECT_EQ(strategy.last_account_id, "CTP001");
    EXPECT_DOUBLE_EQ(strategy.account_balance, 100000.0);
    EXPECT_DOUBLE_EQ(strategy.account_available, 80000.0);
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

TEST_F(StrategyEngineTest, UserInputFrameDirectedToOwnStrategyDispatched) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            // UI_INPUT 变长 ext_inst 帧: instance_id + data_size + UTF-8 payload
            // (契约 strategy: UI→策略, SDK 按 instance_id == 裸策略名 定向过滤,
            //  写法同 dz_output_ui 的写端, instance_id 须为本策略名才放行)
            const char* payload = R"({"action":"pause","reason":"manual"})";
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, strategy_id.c_str(),
                                              reinterpret_cast<const std::byte*>(payload),
                                              static_cast<uint32_t>(std::strlen(payload)));
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_ui_input(std::string_view data) { inner.on_ui_input(data); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnUiInput<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.input_count, 1);
    // instance_id 透传帧扩展头: SDK 已定向过滤, 引擎收到的必为本策略输入
    EXPECT_EQ(strategy.last_input, R"({"action":"pause","reason":"manual"})");
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

TEST_F(StrategyEngineTest, AccountStatusFrameDispatchedUnfiltered) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            // DZ_FRAME_ACCOUNT_STATUS 为全量透传帧 (无策略过滤), basic struct 帧写法同
            // DZ_FRAME_POSITION_INFO/DZ_FRAME_TRADING_ACCOUNT 用例
            DzAccountStatus status{};
            dztrader::copy_string(status.account_id, "CTP001", true);
            dztrader::copy_string(status.gateway_name, "dztd_ctp", true);
            status.state = DZ_ACCOUNT_READY;
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_frame(DZ_FRAME_ACCOUNT_STATUS, status);
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_account_status(const DzAccountStatus& status) { inner.on_account_status(status); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnAccountStatus<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.account_status_count, 1);
    EXPECT_STREQ(strategy.last_account_status.account_id, "CTP001");
    EXPECT_STREQ(strategy.last_account_status.gateway_name, "dztd_ctp");
    EXPECT_EQ(strategy.last_account_status.state, DZ_ACCOUNT_READY);
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

TEST_F(StrategyEngineTest, AccountStatusWrongSizeFrameNotDispatched) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
            // 超长帧防御 (payload_size_matches): frame_size = 头 + 104 + 8,
            // SDK 对布局偏离的 DZ_FRAME_ACCOUNT_STATUS 帧丢弃不分发
            DzAccountStatus status{};
            dztrader::copy_string(status.account_id, "CTP001", true);
            dztrader::copy_string(status.gateway_name, "dztd_ctp", true);
            status.state = DZ_ACCOUNT_READY;
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            auto* frame = writer.open_frame(DZ_FRAME_ACCOUNT_STATUS, sizeof(DzAccountStatus) + 8);
            if (frame != nullptr) {
                std::memcpy(frame, &status, sizeof(DzAccountStatus));
                writer.close_frame();
            }
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_account_status(const DzAccountStatus& status) { inner.on_account_status(status); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnAccountStatus<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    // frame_size 不符: DZ_FRAME_ACCOUNT_STATUS 帧被 SDK 丢弃, 不分发到策略
    EXPECT_EQ(strategy.account_status_count, 0);
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

// ── 启动自检 / 生命周期承诺 / 边界防御 ──

/// std::cout 捕获: 启动自检行走 stdout (master 以 info 级转发), 测试需断言探测清单
class CoutCapture {
public:
    CoutCapture()
        : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }
    CoutCapture(const CoutCapture&) = delete;
    CoutCapture& operator=(const CoutCapture&) = delete;
    [[nodiscard]] std::string str() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* old_;
};

TEST_F(StrategyEngineTest, StartupSelfCheckListsAllProbes) {
    FullStrategy strategy;
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};

    struct HookStrategy {
        FullStrategy& inner;
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_stop() { inner.on_stop(); }
        void on_tick(const DzTick& tick) { inner.on_tick(tick); }
        void on_trade_report(const DzTradeReport&) {}
        void on_account_status(const DzAccountStatus& status) { inner.on_account_status(status); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnTick<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnAccountStatus<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};
    std::string self_check;
    {
        CoutCapture capture;
        start_rescue(15000);
        const int32_t rc = dztrader::run_strategy(hook);
        join_threads();
        self_check = capture.str();
        EXPECT_EQ(rc, 0);
    }
    // 完整探测清单 (字段顺序 = report_start 格式): Hook 实现
    // stop/tick/trade/account_status/schedule, 其余 no
    const std::string expected =
        "strategy callbacks | stop=yes tick=yes trade=yes order=no position=no account=no "
        "account_status=yes schedule=yes input=no error=no self_exit=no exit_code=no\n";
    EXPECT_EQ(self_check, expected);
}

TEST_F(StrategyEngineTest, StrategyWithoutOnTickRunsAndSelfCheckShowsTickNo) {
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};
    int stop_count = 0;

    struct NoTickStrategy {
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;
        int& stop_count;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_stop() { ++stop_count; }
        void on_schedule(const DzScheduleEvent&) {
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<NoTickStrategy>);
    static_assert(!dztrader::strategy_engine_internal::HasOnTick<NoTickStrategy>);

    static_assert(!dztrader::strategy_engine_internal::HasOnAccountStatus<NoTickStrategy>);

    NoTickStrategy strategy{strategy_id, shutdown_written, *this, stop_count};
    ASSERT_TRUE(write_tick(1234.5));  // 无 on_tick: 引擎仍排空 md 帧, 只是不派发
    std::string self_check;
    {
        CoutCapture capture;
        start_rescue(15000);
        const int32_t rc = dztrader::run_strategy(strategy);
        join_threads();
        self_check = capture.str();
        EXPECT_EQ(rc, 0);
    }
    const std::string expected =
        "strategy callbacks | stop=yes tick=no trade=no order=no position=no account=no "
        "account_status=no schedule=yes input=no error=no self_exit=no exit_code=no\n";
    EXPECT_EQ(self_check, expected);
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(stop_count, 1);
}

TEST_F(StrategyEngineTest, OnStopExceptionIsNotForwardedToOnError) {
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};
    int stop_count = 0;
    int error_count = 0;

    struct HookStrategy {
        std::string& strategy_id;
        std::atomic<bool>& shutdown_written;
        StrategyEngineTest& fixture;
        int& stop_count;
        int& error_count;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_stop() {
            ++stop_count;
            throw std::runtime_error("on_stop failed");
        }
        void on_schedule(const DzScheduleEvent&) {
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
        void on_error(DzErrorCode, std::string_view) { ++error_count; }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy_id, shutdown_written, *this, stop_count, error_count};
    start_rescue(15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(stop_count, 1);
    EXPECT_EQ(error_count, 0);  // on_stop 异常只写 stderr 诊断行, 不触发 on_error
}

TEST_F(StrategyEngineTest, MalformedUiInputFramesAreDropped) {
    FullStrategy strategy;
    constexpr size_t kMinExtSize = sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);

    // 1) 帧长不足 ext 头
    std::vector<std::byte> too_short(sizeof(DzFrameHeader), std::byte{0});
    auto* short_hdr = reinterpret_cast<DzFrameHeader*>(too_short.data());
    short_hdr->frame_size = static_cast<uint32_t>(too_short.size());
    short_hdr->frame_type = DZ_FRAME_UI_INPUT;
    dztrader::strategy_engine_internal::dispatch_ui_input(strategy, too_short.data());

    // 2) data_size 越出帧尾
    std::vector<std::byte> out_of_frame(kMinExtSize, std::byte{0});
    auto* out_hdr = reinterpret_cast<DzFrameHeader*>(out_of_frame.data());
    out_hdr->frame_size = static_cast<uint32_t>(out_of_frame.size());
    out_hdr->frame_type = DZ_FRAME_UI_INPUT;
    auto* out_ext =
        reinterpret_cast<DzExtInstFrameHeader*>(out_of_frame.data() + sizeof(DzFrameHeader));
    dztrader::copy_string(out_ext->instance_id, "stg_test", true);
    out_ext->data_size = 512;
    dztrader::strategy_engine_internal::dispatch_ui_input(strategy, out_of_frame.data());

    // 3) instance_id 无 NUL 终止符
    std::vector<std::byte> no_terminator(kMinExtSize, std::byte{0});
    auto* term_hdr = reinterpret_cast<DzFrameHeader*>(no_terminator.data());
    term_hdr->frame_size = static_cast<uint32_t>(no_terminator.size());
    term_hdr->frame_type = DZ_FRAME_UI_INPUT;
    auto* term_ext =
        reinterpret_cast<DzExtInstFrameHeader*>(no_terminator.data() + sizeof(DzFrameHeader));
    std::memset(term_ext->instance_id, 'x', sizeof(DzInstanceId));
    term_ext->data_size = 0;
    dztrader::strategy_engine_internal::dispatch_ui_input(strategy, no_terminator.data());

    EXPECT_EQ(strategy.input_count, 0);

    // 4) 良构帧仍正常分发 (帧契约要求 frame_size 为 8 的倍数: 80 + 8)
    std::vector<std::byte> valid(kMinExtSize + 8, std::byte{0});
    auto* valid_hdr = reinterpret_cast<DzFrameHeader*>(valid.data());
    valid_hdr->frame_size = static_cast<uint32_t>(valid.size());
    valid_hdr->frame_type = DZ_FRAME_UI_INPUT;
    auto* valid_ext = reinterpret_cast<DzExtInstFrameHeader*>(valid.data() + sizeof(DzFrameHeader));
    dztrader::copy_string(valid_ext->instance_id, "stg_test", true);
    valid_ext->data_size = 3;
    std::memcpy(valid.data() + kMinExtSize, "abc", 3);
    dztrader::strategy_engine_internal::dispatch_ui_input(strategy, valid.data());

    EXPECT_EQ(strategy.input_count, 1);
    EXPECT_EQ(strategy.last_input, "abc");
}

TEST_F(StrategyEngineTest, MalformedUiInputViaHandleEventDropped) {
    // 路由层 (handle_event) 也必须走防御分支, 防止有人只保留 helper 而回退分支
    FullStrategy strategy;
    bool running = true;
    std::vector<std::byte> too_short(sizeof(DzFrameHeader), std::byte{0});
    auto* hdr = reinterpret_cast<DzFrameHeader*>(too_short.data());
    hdr->frame_size = static_cast<uint32_t>(too_short.size());
    hdr->frame_type = DZ_FRAME_UI_INPUT;
    dztrader::strategy_engine_internal::handle_event(strategy, too_short.data(), running);
    EXPECT_EQ(strategy.input_count, 0);
    EXPECT_TRUE(running);
}

TEST_F(StrategyEngineTest, MalformedShutdownDirectCallStopsSafely) {
    // SDK 分发层会先丢弃畸形 SHUTDOWN; 此用例锁定引擎直调时的 fail-safe 语义
    FullStrategy strategy;
    bool running = true;
    std::vector<std::byte> too_short(sizeof(DzFrameHeader), std::byte{0});
    auto* hdr = reinterpret_cast<DzFrameHeader*>(too_short.data());
    hdr->frame_size = static_cast<uint32_t>(too_short.size());
    hdr->frame_type = DZ_FRAME_SHUTDOWN;
    dztrader::strategy_engine_internal::handle_event(strategy, too_short.data(), running);
    EXPECT_FALSE(running);
    EXPECT_EQ(strategy.stop_count, 1);
}

TEST_F(StrategyEngineTest, PayloadSizeMismatchFramesAreDropped) {
    FullStrategy strategy;
    bool running = true;

    // ORDER_REPORT: 超长/欠长丢弃, 正常帧分发 (控制组证明派发路径存在)
    std::vector<std::byte> order_oversized(sizeof(DzFrameHeader) + sizeof(DzOrderReport) + 8,
                                           std::byte{0});
    auto* oversized_hdr = reinterpret_cast<DzFrameHeader*>(order_oversized.data());
    oversized_hdr->frame_size = static_cast<uint32_t>(order_oversized.size());
    oversized_hdr->frame_type = DZ_FRAME_ORDER_REPORT;
    dztrader::strategy_engine_internal::handle_event(strategy, order_oversized.data(), running);
    EXPECT_EQ(strategy.order_count, 0);
    EXPECT_TRUE(running);

    std::vector<std::byte> order_undersized(sizeof(DzFrameHeader) + 8, std::byte{0});
    auto* undersized_hdr = reinterpret_cast<DzFrameHeader*>(order_undersized.data());
    undersized_hdr->frame_size = static_cast<uint32_t>(order_undersized.size());
    undersized_hdr->frame_type = DZ_FRAME_ORDER_REPORT;
    dztrader::strategy_engine_internal::handle_event(strategy, order_undersized.data(), running);
    EXPECT_EQ(strategy.order_count, 0);

    std::vector<std::byte> order_valid(sizeof(DzFrameHeader) + sizeof(DzOrderReport), std::byte{0});
    auto* valid_order_hdr = reinterpret_cast<DzFrameHeader*>(order_valid.data());
    valid_order_hdr->frame_size = static_cast<uint32_t>(order_valid.size());
    valid_order_hdr->frame_type = DZ_FRAME_ORDER_REPORT;
    dztrader::strategy_engine_internal::handle_event(strategy, order_valid.data(), running);
    EXPECT_EQ(strategy.order_count, 1);

    // TICK: 超长丢弃, 正常帧分发
    std::vector<std::byte> tick_oversized(sizeof(DzFrameHeader) + sizeof(DzTick) + 8, std::byte{0});
    auto* bad_tick_hdr = reinterpret_cast<DzFrameHeader*>(tick_oversized.data());
    bad_tick_hdr->frame_size = static_cast<uint32_t>(tick_oversized.size());
    bad_tick_hdr->frame_type = DZ_FRAME_TICK;
    dztrader::strategy_engine_internal::handle_md(strategy, tick_oversized.data());
    EXPECT_EQ(strategy.tick_count, 0);

    std::vector<std::byte> tick_valid(sizeof(DzFrameHeader) + sizeof(DzTick), std::byte{0});
    auto* valid_tick_hdr = reinterpret_cast<DzFrameHeader*>(tick_valid.data());
    valid_tick_hdr->frame_size = static_cast<uint32_t>(tick_valid.size());
    valid_tick_hdr->frame_type = DZ_FRAME_TICK;
    dztrader::strategy_engine_internal::handle_md(strategy, tick_valid.data());
    EXPECT_EQ(strategy.tick_count, 1);
}

TEST_F(StrategyEngineTest, InitFailureReportsErrorBeforeOnStart) {
    // 缺 DZTRADER_MD_SOURCE -> dz_init 失败 -> on_error 在 on_start 之前被调用。
    // 若 dz_init 意外成功, on_start 排的定时器会写 SHUTDOWN, 保证用例快速失败而非挂死。
    const auto previous = dztrader::env::get("DZTRADER_MD_SOURCE");
    dztrader::env::unset("DZTRADER_MD_SOURCE");
    FullStrategy inner;
    std::string strategy_id;

    struct HookStrategy {
        FullStrategy& inner;
        StrategyEngineTest& fixture;
        std::string& strategy_id;

        void on_start(DzContext* ctx) {
            inner.on_start(ctx);
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            (void)fixture.write_shutdown(strategy_id);
        }
        void on_error(DzErrorCode code, std::string_view msg) { inner.on_error(code, msg); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{inner, *this, strategy_id};
    const int32_t rc = dztrader::run_strategy(hook);
    if (previous.has_value()) {
        dztrader::env::set("DZTRADER_MD_SOURCE", *previous);
    } else {
        dztrader::env::unset("DZTRADER_MD_SOURCE");
    }

    EXPECT_NE(rc, 0);
    EXPECT_EQ(inner.start_count, 0);
    EXPECT_EQ(inner.error_count, 1);
    EXPECT_NE(inner.error_code, 0);
}

// ── 自主退出 ──

TEST_F(StrategyEngineTest, SelfExitReturnsCodeAndCallsOnStop) {
    int stop_count = 0;
    int start_count = 0;
    std::string strategy_id;

    struct HookStrategy {
        int& stop_count;
        int& start_count;
        std::string& strategy_id;
        StrategyEngineTest& fixture;

        void on_start(DzContext* ctx) {
            ++start_count;
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);  // 回归兜底: 不退出时由定时器写 SHUTDOWN
        }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
        void on_stop() { ++stop_count; }
        bool should_exit() const noexcept { return true; }
        int32_t exit_code() const noexcept { return 42; }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasShouldExit<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasExitCode<HookStrategy>);

    HookStrategy hook{stop_count, start_count, strategy_id, *this};
    std::string out;
    int32_t rc = 0;
    {
        CoutCapture capture;
        rc = dztrader::run_strategy(hook);
        out = capture.str();
    }
    EXPECT_EQ(rc, 42);
    EXPECT_EQ(start_count, 1);
    EXPECT_EQ(stop_count, 1);
    EXPECT_NE(out.find("strategy exit | reason=self code=42\n"), std::string::npos) << out;
    // dz_release 已执行: 同进程可再次 dz_init
    DzContext* reinit = dz_init();
    ASSERT_NE(reinit, nullptr) << dz_errmsg();
    dz_release(reinit);
}

TEST_F(StrategyEngineTest, SelfExitRequestedFromCallbackExitsAfterPump) {
    std::string strategy_id;
    int stop_count = 0;

    struct HookStrategy {
        std::string& strategy_id;
        StrategyEngineTest& fixture;
        int& stop_count;
        bool exit_requested = false;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_schedule(const DzScheduleEvent&) { exit_requested = true; }
        bool should_exit() const noexcept { return exit_requested; }
        void on_stop() { ++stop_count; }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasShouldExit<HookStrategy>);
    static_assert(!dztrader::strategy_engine_internal::HasExitCode<HookStrategy>);

    HookStrategy hook{strategy_id, *this, stop_count};
    start_rescue(15000);
    std::string out;
    int32_t rc = 0;
    {
        CoutCapture capture;
        rc = dztrader::run_strategy(hook);
        out = capture.str();
    }
    join_threads();

    // 无 exit_code(): 默认 0; 必须走自主退出路径而非 SHUTDOWN 路径
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(stop_count, 1);
    EXPECT_TRUE(hook.exit_requested);
    EXPECT_NE(out.find("strategy exit | reason=self code=0\n"), std::string::npos) << out;
}

TEST_F(StrategyEngineTest, ThrowingShouldExitReportedOnceAndDoesNotExit) {
    std::string strategy_id;
    std::atomic<bool> shutdown_written{false};
    int stop_count = 0;
    int error_count = 0;

    struct HookStrategy {
        std::string& strategy_id;
        StrategyEngineTest& fixture;
        std::atomic<bool>& shutdown_written;
        int& stop_count;
        int& error_count;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 100);
        }
        void on_schedule(const DzScheduleEvent&) {
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
        bool should_exit() { throw std::runtime_error("should_exit failed"); }
        void on_stop() { ++stop_count; }
        void on_error(DzErrorCode, std::string_view) { ++error_count; }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasShouldExit<HookStrategy>);

    HookStrategy hook{strategy_id, *this, shutdown_written, stop_count, error_count};
    start_rescue(15000);
    std::string out;
    int32_t rc = 0;
    {
        CoutCapture capture;
        rc = dztrader::run_strategy(hook);
        out = capture.str();
    }
    join_threads();

    EXPECT_EQ(rc, 0);                                              // SHUTDOWN 正常退出
    EXPECT_TRUE(shutdown_written.load());                          // 确实走 SHUTDOWN 路径
    EXPECT_EQ(stop_count, 1);                                      // 仅 SHUTDOWN 路径调 on_stop
    EXPECT_EQ(error_count, 1);                                     // 探测异常只上报一次 (不刷屏)
    EXPECT_EQ(out.find("reason=self"), std::string::npos) << out;  // 未走自主退出
}

TEST_F(StrategyEngineTest, SelfExitWithThrowingExitCodeUsesZeroAndReportsOnce) {
    std::string strategy_id;
    int stop_count = 0;
    int error_count = 0;

    struct HookStrategy {
        std::string& strategy_id;
        StrategyEngineTest& fixture;
        int& stop_count;
        int& error_count;

        void on_start(DzContext* ctx) {
            strategy_id = dz_strategy_id(ctx);
            (void)dz_schedule_after(ctx, 500);  // 回归兜底: 不退出时写 SHUTDOWN
        }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
        void on_stop() { ++stop_count; }
        void on_error(DzErrorCode, std::string_view) { ++error_count; }
        bool should_exit() const noexcept { return true; }
        int32_t exit_code() const { throw std::runtime_error("exit_code failed"); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasExitCode<HookStrategy>);

    HookStrategy hook{strategy_id, *this, stop_count, error_count};
    std::string out;
    int32_t rc = 0;
    {
        CoutCapture capture;
        rc = dztrader::run_strategy(hook);
        out = capture.str();
    }
    EXPECT_EQ(rc, 0);  // 异常 -> 退出码按 0
    EXPECT_EQ(stop_count, 1);
    EXPECT_EQ(error_count, 1);  // exit_code 异常上报一次
    EXPECT_NE(out.find("strategy exit | reason=self code=0\n"), std::string::npos) << out;
}

TEST_F(StrategyEngineTest, SelfExitNegativeCodeIsReturned) {
    int stop_count = 0;

    struct HookStrategy {
        int& stop_count;

        void on_start(DzContext*) {}
        void on_stop() { ++stop_count; }
        bool should_exit() const noexcept { return true; }
        int32_t exit_code() const noexcept { return -3; }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasExitCode<HookStrategy>);

    HookStrategy hook{stop_count};
    start_rescue(2000);  // 防御: 探测回归时不挂死 (2s 后兜底 SHUTDOWN)
    std::string out;
    int32_t rc = 0;
    {
        CoutCapture capture;
        rc = dztrader::run_strategy(hook);
        out = capture.str();
    }
    join_threads();

    EXPECT_EQ(rc, -3);  // 负值原样返回 (契约只建议 0-127, 不做钳制)
    EXPECT_EQ(stop_count, 1);
    EXPECT_NE(out.find("strategy exit | reason=self code=-3\n"), std::string::npos) << out;
}

}  // namespace
