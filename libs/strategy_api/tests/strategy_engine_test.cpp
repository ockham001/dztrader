#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
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
#include <string>
#include <string_view>
#include <thread>

namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::DzUiInput;

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
        .channel_name = "test_md",
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

/// 引擎测试: 临时 DZTRADER_HOME + 预建事件/md 通道。run_strategy 内部自行
/// dz_init/dz_release, 测试体只准备环境并写帧。
///
/// 唤醒机制说明: 测试进程不是经 master 启动, 事件通道订阅者列表为空,
/// writer.notify_subscribers() 唤不醒策略进程的 dz_wait。因此各测试的
/// Hook 策略在 on_start 里排 dz_schedule_after 定时器, dz_wait 以
/// wait_for(超时) 模式等待, 到期后 on_schedule 主动写 SHUTDOWN 帧退出。
/// rescue 线程仅作防挂死兜底 (正常路径不触发)。
/// 另外: 写帧 helper 不得使用 gtest 断言宏 (致命断言在非主线程是 UB)。
class StrategyEngineTest : public ::testing::Test {
protected:
    std::string home_;
    std::thread rescue_thread_;
    std::atomic<bool> done_{false};

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_engine").string();
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
        return std::make_shared<ChannelMeta>(
            ChannelMeta::open_only(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                   home_ + "/shm"));
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
    /// (正常路径策略侧自退出, 不触发)。信号量名与 DzContext 构造一致
    /// (strategy_identity: "stg." + exe_stem), 写帧 notify_subscribers
    /// 对本测试进程无效 -- 订阅者列表为空, 见类头注释。
    void start_rescue(const std::string& strategy_id, int timeout_ms) {
        rescue_thread_ = std::thread([this, strategy_id, timeout_ms] {
            for (int i = 0; i < timeout_ms / 50; ++i) {
                if (done_.load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (strategy_id.empty()) {
                return;
            }
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
        ++trade_count;
    }
    void on_order_report(const DzOrderReport& rpt) {
        order_id = rpt.order_id;
        ++order_count;
    }
    void on_position_info(const DzPositionInfo& info) {
        last_position_instrument = std::string(info.instrument_id);
        ++position_count;
    }
    void on_trading_account(const DzTradingAccount& account) {
        last_account_id = std::string(account.account_id);
        ++account_count;
    }
    void on_schedule(const DzScheduleEvent& ev) {
        ++schedule_count;
        timer_id = ev.timer_id;
    }
    void on_ui_input(const DzUiInput& input) {
        ++input_count;
        last_instance_id = std::string(input.instance_id);
        last_input = std::string(input.data, input.data_size);
    }
    void on_error(DzErrorCode code, std::string_view message) {
        ++error_count;
        error_code = code;
        error_message = std::string(message);
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
    double last_price = 0.0;
    std::string trade_id;
    int64_t order_id = -1;
    std::string last_position_instrument;
    std::string last_account_id;
    DzTimerId timer_id = 0;
    std::string last_instance_id;
    std::string last_input;
    DzErrorCode error_code = 0;
    std::string error_message;
};

static_assert(dztrader::Strategy<FullStrategy>);
static_assert(dztrader::Strategy<dztrader::StrategyBase>);

// concept 负例 (编译期锁定: 缺失/拼错必选回调不得通过 Strategy)
namespace {
struct MissingOnStart {
    void on_tick(const DzTick&) {}
};
struct MissingOnTick {
    void on_start(DzContext*) {}
};
struct MisspelledTick {
    void on_start(DzContext*) {}
    void onTick(const DzTick&) {}  // 驼峰拼写错误
};
static_assert(!dztrader::Strategy<MissingOnStart>);
static_assert(!dztrader::Strategy<MissingOnTick>);
static_assert(!dztrader::Strategy<MisspelledTick>);
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

    start_rescue(strategy_id, 15000);
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

    start_rescue(strategy_id, 15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.start_count, 1);
    EXPECT_EQ(strategy.tick_count, 0);  // 未写 tick 帧, on_tick 不被调
    EXPECT_TRUE(shutdown_written.load());
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

    start_rescue(strategy_id, 15000);
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
        void on_error(DzErrorCode code, std::string_view message) {
            inner.on_error(code, message);
        }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, *this};

    start_rescue(strategy_id, 15000);
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
        void on_error(DzErrorCode code, std::string_view message) {
            inner.on_error(code, message);
        }
        void on_schedule(const DzScheduleEvent&) { (void)fixture.write_shutdown(strategy_id); }
    };
    static_assert(dztrader::Strategy<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, *this};

    start_rescue(strategy_id, 15000);
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
        void on_error(DzErrorCode code, std::string_view message) {
            inner.on_error(code, message);
        }
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
            // SDK 按 payload strategy_id 定向过滤, 须填本策略裸名才放行
            DzTradeReport trade{};
            trade.price = 3888.5;
            trade.volume = 7;
            dztrader::copy_string(trade.strategy_id, strategy_id.c_str(), true);
            dztrader::shm::MultiWriter writer = dztrader::shm::MultiWriter::create(
                fixture.event_channel_meta(), "engine_test_writer");
            (void)writer.write_frame(DZ_FRAME_TRADE_REPORT, trade);
            DzOrderReport order{};
            order.price = 3901.0;
            order.volume = 3;
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

    start_rescue(strategy_id, 15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.trade_count, 1);
    EXPECT_EQ(strategy.order_count, 1);
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
            // 2002/2003 为全量透传帧 (无策略过滤): 不填 strategy_id 也应分发,
            // 写法同 TradeAndOrderReportsDispatchedWithPayload (basic struct 帧)
            DzPositionInfo info{};
            dztrader::copy_string(info.instrument_id, "IF2603", true);
            info.direction = DZ_DIRECTION_LONG;
            DzTradingAccount account{};
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
    static_assert(
        dztrader::strategy_engine_internal::HasOnPositionInfo<HookStrategy>);
    static_assert(
        dztrader::strategy_engine_internal::HasOnTradingAccount<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(strategy_id, 15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.position_count, 1);
    EXPECT_EQ(strategy.last_position_instrument, "IF2603");
    EXPECT_EQ(strategy.account_count, 1);
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
        void on_ui_input(const DzUiInput& input) { inner.on_ui_input(input); }
        void on_schedule(const DzScheduleEvent& ev) {
            inner.on_schedule(ev);
            shutdown_written.store(fixture.write_shutdown(strategy_id));
        }
    };
    static_assert(dztrader::Strategy<HookStrategy>);
    static_assert(dztrader::strategy_engine_internal::HasOnUiInput<HookStrategy>);

    HookStrategy hook{strategy, strategy_id, shutdown_written, *this};

    start_rescue(strategy_id, 15000);
    const int32_t rc = dztrader::run_strategy(hook);
    join_threads();

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(strategy.input_count, 1);
    // instance_id 透传帧扩展头: SDK 已定向过滤, 引擎收到的必为本策略输入
    EXPECT_EQ(strategy.last_instance_id, strategy_id);
    EXPECT_EQ(strategy.last_input, R"({"action":"pause","reason":"manual"})");
    EXPECT_TRUE(shutdown_written.load());
    EXPECT_EQ(strategy.stop_count, 1);
}

}  // namespace
