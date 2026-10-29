/**
 * @file bar_strategy_position_test.cpp
 * @brief 虚拟台账 + Tier 1 的单元测试（issue #159；组件文档 §6/§6.1）
 *
 * 只经**公开面 + 内部构造缝**验证：`context_internal::ContextAccess::make()` 造一个脱离
 * 引擎的 ctx（不订阅、不发单），然后像 B 那样调四个意图方法并读 Tier 1 查询。这条缝是
 * issue #153 的「接缝 C（纯单元）」——台账口径是纯状态转移，用 Seam A（帧通道 + 真实回调）
 * 验证会把「哪一格算错」淹没在事件序断言里。
 *
 * 三组断言：
 *   1. **四函数全表**：`buy` / `sell_short` 反向目标化为 `P = ±n`；`sell` / `buy_to_cover`
 *      只减不增、超出钳制；`volume = 0` 两组等价对（`buy(0) ≡ buy_to_cover(0)`、
 *      `sell(0) ≡ sell_short(0)`）。跨 0 的平仓段 + 开仓段拆分也在这一组。
 *   2. **即时记账**：同一「回调」内 `buy()` 之后立刻查询即反映新状态（不依赖任何回报），
 *      并且 Tier 1 在同一漏斗里同步更新。
 *   3. **Tier 1 口径**：净持仓派生方向、`current_contracts` 非负、均价部分平仓不变/反手重置、
 *      建仓段数与历史峰值、进出场价/时间（对应 bar 标准结束时间）、`bars_since_*`
 *      （同 bar = 0、无参考 = -1）、盈亏乘合约乘数且不含费用、极值在 bar 收盘前更新且开仓
 *      重置 / 平仓归零。
 *
 * 诊断分级另有一组：把 stderr 抓下来断言「有 / 没有 / 哪一个 prose」。分级在组件文档 §6 是
 * 明文要求（WARN / INFO / 不诊断），而它是 B 唯一可观察到的意图反馈。
 *
 * 编译期不可见账户身份另有一组 `static_assert`：B 不能从 ctx 拿到任何账户标识（ADR 0028）。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy/context.h>
#include <dztrader/bar_strategy/context_internal.h>
#include <dztrader/bar_strategy/position.h>
#include <dztrader/data_type.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace {

using dztrader::InstrumentContext;
using dztrader::InstrumentInit;
using dztrader::InstrumentSpec;
using dztrader::LargeVolume;
using dztrader::PositionState;
using dztrader::Volume;
using dztrader::context_internal::ContextAccess;
using dztrader::position_internal::BarStamp;
using dztrader::position_internal::Intent;

/// 测试用合约乘数（10 = 常见的 10 吨/手；让「乘不乘乘数」在断言里一眼可辨）
constexpr double kVolumeMultiple = 10.0;
/// 测试用交易日（任意正数）
constexpr DzDate kTradingDay = 20000;
/// 测试用 bar 标准结束时间（墙钟 09:01:00）
constexpr DzTime kBarEndTime = 9 * 3600 + 60;

/**
 * @brief 造一个脱离引擎的 ctx（0 个挂靠账户、无通道、无配置）
 *
 * `max_lookback` / `buffer_bars` 取最小合法值：本文件的用例都绕过窗口推进（直接调意图），
 * 所以窗口容量只需够构造。0 个挂靠账户正是 AC「0 个挂靠账户时策略照常运行：只记逻辑账、
 * 不发单、不上报」——ctx 内本来就没有账户表，测试能构造成功本身就是那条路径的证据。
 */
std::unique_ptr<InstrumentContext> make_ctx(const char* instrument_id = "rb2601") {
    InstrumentSpec spec;
    spec.volume_multiple = kVolumeMultiple;  // 本文件的 Tier 1 用例只关心乘数
    return ContextAccess::make(InstrumentInit{
        .instrument_id = instrument_id,
        .max_lookback = 32,
        .buffer_bars = 64,
        .spec = spec,
        // 脱离引擎的虚拟台账模式：无会话句柄、无账户表 → 只记账、不发单、不上报（#160 的
        // 「0 个挂靠账户」口径）。执行 fanout 的接线由 Seam A 集成测试覆盖。
        .dz = nullptr,
        .accounts = nullptr,
        .scaling = {},
    });
}

/**
 * @brief 直接驱动台账（跳过 ctx 的 stderr 诊断）
 *
 * 「开平规则」这一层用纯函数验证：全表有 4 种意图 × 3 种持仓状态 × {n, 0, 超仓} 许多格，
 * 逐格断言时不该被诊断输出干扰（也不该因为「诊断分级」这一独立关注点而耦合失败）。
 * 诊断分级在下面的 `IntentDiagnostics` 组里、经 ctx 单独验证。
 */
struct Ledger {
    PositionState state;
    std::int64_t bar_index = 0;
    DzDate date = kTradingDay;
    DzTime time = kBarEndTime;
    double price = 100.0;

    dztrader::position_internal::LedgerOutcome apply(Intent intent,
                                                     Volume volume,
                                                     double intent_price) {
        price = intent_price;
        return dztrader::position_internal::apply_intent(
            state, intent, volume, intent_price,
            BarStamp{.date = date, .time = time, .bar_index = bar_index});
    }
};

// ============================================================
// 四函数开平意图全表（组件文档 §6 两张表）
// ============================================================

/**
 * @brief `buy(n)` 与 `sell_short(n)` 反向时**目标化**为 `P = ±n`，同向时累加
 *
 * 目标化（而不是「平对手 + 加 n」两步）是净持仓模型的关键：反手是一次意图、一张差量单，
 * 逻辑账上直接从 `-3` 到 `+2`，不会出现中间态 `0`。
 */
TEST(IntentTableTest, EntryIntentsTargetTheNetPosition) {
    // buy(n)：P<0 → +n；P=0 → +n；P>0 → P+n
    struct Case {
        LargeVolume before;
        Volume volume;
        LargeVolume expected;
    };
    const Case buy_cases[] = {
        {.before = -3, .volume = 2, .expected = 2},  // 反向：目标化 +2
        {.before = -3, .volume = 5, .expected = 5},  // 反向且超过对手仓：仍只到 +5（不叠加）
        {.before = 0, .volume = 4, .expected = 4},   // 空仓开多
        {.before = 2, .volume = 3, .expected = 5},   // 加多
    };
    for (const Case& c : buy_cases) {
        Ledger ledger;
        ledger.state.net_volume = c.before;
        ledger.apply(Intent::Buy, c.volume, 100.0);
        EXPECT_EQ(ledger.state.net_volume, c.expected)
            << "buy | before=" << c.before << " volume=" << c.volume;
    }

    // sell_short(n)：P>0 → -n；P=0 → -n；P<0 → P-n
    const Case short_cases[] = {
        {.before = 3, .volume = 2, .expected = -2},   // 反向：目标化 -2
        {.before = 3, .volume = 5, .expected = -5},   // 反向且超过对手仓
        {.before = 0, .volume = 4, .expected = -4},   // 空仓开空
        {.before = -2, .volume = 3, .expected = -5},  // 加空
    };
    for (const Case& c : short_cases) {
        Ledger ledger;
        ledger.state.net_volume = c.before;
        ledger.apply(Intent::SellShort, c.volume, 100.0);
        EXPECT_EQ(ledger.state.net_volume, c.expected)
            << "sell_short | before=" << c.before << " volume=" << c.volume;
    }
}

/**
 * @brief `sell(n)` / `buy_to_cover(n)` 只减不增；超出逻辑持仓钳制到全平
 *
 * 反向（无该方向持仓）时是**无操作 + WARN**，不是「反手开仓」——出口函数的语义约束。
 */
TEST(IntentTableTest, ExitIntentsOnlyReduceAndClamp) {
    struct Case {
        Intent intent;
        LargeVolume before;
        Volume volume;
        LargeVolume expected;
        bool expect_clamped;
        bool expect_direction_invalid;
    };
    const Case cases[] = {
        // sell(n) 平多
        {.intent = Intent::Sell,
         .before = 5,
         .volume = 2,
         .expected = 3,
         .expect_clamped = false,
         .expect_direction_invalid = false},
        {.intent = Intent::Sell,
         .before = 5,
         .volume = 5,
         .expected = 0,
         .expect_clamped = false,
         .expect_direction_invalid = false},
        {.intent = Intent::Sell,
         .before = 5,
         .volume = 9,
         .expected = 0,
         .expect_clamped = true,
         .expect_direction_invalid = false},
        {.intent = Intent::Sell,
         .before = 0,
         .volume = 2,
         .expected = 0,
         .expect_clamped = false,
         .expect_direction_invalid = true},
        {.intent = Intent::Sell,
         .before = -3,
         .volume = 2,
         .expected = -3,
         .expect_clamped = false,
         .expect_direction_invalid = true},
        // buy_to_cover(n) 平空
        {.intent = Intent::BuyToCover,
         .before = -5,
         .volume = 2,
         .expected = -3,
         .expect_clamped = false,
         .expect_direction_invalid = false},
        {.intent = Intent::BuyToCover,
         .before = -5,
         .volume = 5,
         .expected = 0,
         .expect_clamped = false,
         .expect_direction_invalid = false},
        {.intent = Intent::BuyToCover,
         .before = -5,
         .volume = 9,
         .expected = 0,
         .expect_clamped = true,
         .expect_direction_invalid = false},
        {.intent = Intent::BuyToCover,
         .before = 0,
         .volume = 2,
         .expected = 0,
         .expect_clamped = false,
         .expect_direction_invalid = true},
        {.intent = Intent::BuyToCover,
         .before = 3,
         .volume = 2,
         .expected = 3,
         .expect_clamped = false,
         .expect_direction_invalid = true},
    };
    for (const Case& c : cases) {
        Ledger ledger;
        ledger.state.net_volume = c.before;
        const auto outcome = ledger.apply(c.intent, c.volume, 100.0);
        EXPECT_EQ(ledger.state.net_volume, c.expected)
            << dztrader::position_internal::intent_name(c.intent) << " | before=" << c.before
            << " volume=" << c.volume;
        EXPECT_EQ(outcome.clamped, c.expect_clamped)
            << dztrader::position_internal::intent_name(c.intent) << " clamped flag";
        EXPECT_EQ(outcome.direction_invalid, c.expect_direction_invalid)
            << dztrader::position_internal::intent_name(c.intent) << " direction_invalid flag";
    }
}

/**
 * @brief `volume == 0` 是**模式值**：两组等价对 + 无可平仓时完全无操作
 *
 * `buy(0) ≡ buy_to_cover(0)`（买方向平空）、`sell(0) ≡ sell_short(0)`（卖方向平多）。这条
 * 等价关系是 TB/MC 迁移的核心约定（TB 的 `Sell(0)` = 全平多，MC 的 `Buy 0` = 只平空），
 * 所以它必须是**同一条规则**的两个入口，而不是两段相似代码——这里逐格比较两个入口的结果。
 */
TEST(IntentTableTest, ZeroVolumeIsAPatternAndEquivalentPairsMatch) {
    const LargeVolume positions[] = {-7, -1, 0, 1, 7};
    for (const LargeVolume before : positions) {
        for (const double price : {100.0, 123.5}) {
            // 买方向：buy(0) ≡ buy_to_cover(0)
            Ledger via_buy;
            via_buy.state.net_volume = before;
            via_buy.apply(Intent::Buy, 0, price);
            Ledger via_cover;
            via_cover.state.net_volume = before;
            via_cover.apply(Intent::BuyToCover, 0, price);
            EXPECT_EQ(via_buy.state.net_volume, via_cover.state.net_volume)
                << "buy(0) must equal buy_to_cover(0) | before=" << before;
            EXPECT_EQ(via_buy.state.net_volume, before < 0 ? 0 : before) << "before=" << before;

            // 卖方向：sell(0) ≡ sell_short(0)
            Ledger via_sell;
            via_sell.state.net_volume = before;
            via_sell.apply(Intent::Sell, 0, price);
            Ledger via_short;
            via_short.state.net_volume = before;
            via_short.apply(Intent::SellShort, 0, price);
            EXPECT_EQ(via_sell.state.net_volume, via_short.state.net_volume)
                << "sell(0) must equal sell_short(0) | before=" << before;
            EXPECT_EQ(via_sell.state.net_volume, before > 0 ? 0 : before) << "before=" << before;
        }
    }

    // 0 且无可平仓：无操作、无诊断依据（`applied()` false 且四个标志位全 false）
    for (const Intent intent : {Intent::Buy, Intent::Sell, Intent::SellShort, Intent::BuyToCover}) {
        for (const LargeVolume before : {LargeVolume{0}, LargeVolume{-3}, LargeVolume{3}}) {
            Ledger ledger;
            ledger.state.net_volume = before;
            const bool no_close_side =
                ((intent == Intent::Buy || intent == Intent::BuyToCover) ? before >= 0
                                                                         : before <= 0);
            if (!no_close_side) {
                continue;
            }
            const auto outcome = ledger.apply(intent, 0, 100.0);
            EXPECT_FALSE(outcome.applied());
            EXPECT_FALSE(outcome.direction_invalid);
            EXPECT_FALSE(outcome.clamped);
            EXPECT_FALSE(outcome.zero_volume_closed);
        }
    }
}

/**
 * @brief 跨越 0 的意图拆成「平仓段 + 开仓段」：`exit_*` 记平仓价、`entry_*` 记开仓价
 *
 * 反手是唯一同时含平仓与开仓的意图。不拆分的话均价会横跨两个方向算出无意义的数，段数也
 * 会把它当成一次巨额加仓。这里用两个方向的完整场景锁定拆分：先顺向建仓（留下 D、T 标记），
 * 再反手，然后逐项检查 `exit_*` 指向平仓段、`entry_*` 指向开仓段。
 */
TEST(IntentTableTest, ReversalSplitsIntoCloseLegThenOpenLeg) {
    Ledger ledger;
    // 第一根 bar 建多 2 手 @100（entry_* 记为 bar 0）
    ledger.bar_index = 0;
    ledger.date = kTradingDay;
    ledger.time = kBarEndTime;
    ledger.apply(Intent::Buy, 2, 100.0);
    ASSERT_EQ(ledger.state.net_volume, 2);

    // 第二根 bar 反手到 -3 @110：平掉 2 多（exit 段）再开 3 空（entry 段）
    ledger.bar_index = 1;
    ledger.date = kTradingDay + 1;
    ledger.time = kBarEndTime + 60;
    const auto outcome = ledger.apply(Intent::SellShort, 3, 110.0);
    ASSERT_TRUE(outcome.applied());

    EXPECT_EQ(ledger.state.net_volume, -3);
    // 开仓段：均价 = 反手价，段数从 1 起（不是「原 2 手 + 新 3 手」的加权）
    EXPECT_DOUBLE_EQ(ledger.state.avg_entry_price, 110.0);
    EXPECT_EQ(ledger.state.current_entries, 1);
    EXPECT_DOUBLE_EQ(ledger.state.entry_price, 110.0);
    EXPECT_EQ(ledger.state.entry_date, kTradingDay + 1);
    EXPECT_EQ(ledger.state.entry_time, kBarEndTime + 60);
    EXPECT_EQ(ledger.state.entry_bar, 1);
    // 平仓段：exit_* 记的是**本次**反手的平仓价与时间
    EXPECT_DOUBLE_EQ(ledger.state.exit_price, 110.0);
    EXPECT_EQ(ledger.state.exit_date, kTradingDay + 1);
    EXPECT_EQ(ledger.state.exit_time, kBarEndTime + 60);
    EXPECT_EQ(ledger.state.exit_bar, 1);
    // 历史峰值取两个方向的 |P| 较大者
    EXPECT_EQ(ledger.state.max_contracts, 3);
}

// ============================================================
// 即时记账（同一回调内意图 → 查询一致；不依赖任何回报）
// ============================================================

/**
 * @brief 同一「回调」内 `buy()` 之后立刻查询即反映新状态
 *
 * 这是 AC 的字面要求，也是虚拟台账与成交驱动台账的分界：没有成交回报可以等，所以记账
 * 必须发生在意图函数内部。测试在两次查询之间不做任何推进/等待。
 */
TEST(IntentImmediacyTest, LedgerIsVisibleImmediatelyInsideTheSameCallback) {
    const auto ctx = make_ctx();
    ASSERT_EQ(ctx->market_position(), 0);
    ASSERT_EQ(ctx->current_contracts(), 0);

    ctx->buy(3, 100.0);
    EXPECT_EQ(ctx->market_position(), 1) << "buy must be visible right away";
    EXPECT_EQ(ctx->current_contracts(), 3);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 100.0);
    EXPECT_EQ(ctx->current_entries(), 1);

    ctx->sell(1, 105.0);
    EXPECT_EQ(ctx->current_contracts(), 2) << "partial close must be visible right away";
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 100.0) << "partial close keeps the entry price";

    ctx->sell_short(4, 110.0);
    EXPECT_EQ(ctx->market_position(), -1) << "reversal must be visible right away";
    EXPECT_EQ(ctx->current_contracts(), 4);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 110.0) << "reversal resets the entry price";

    ctx->buy_to_cover(0, 120.0);
    EXPECT_EQ(ctx->market_position(), 0) << "zero-volume exit must be visible right away";
    EXPECT_EQ(ctx->current_contracts(), 0);
}

/**
 * @brief 意图在 `on_init` 阶段（`bar_index == -1`）也能记账，但时间戳记为「无参考」
 *
 * B 在 `on_init` 里通常不该下单，但框架不拦（执行门禁只有「失败实例」一条）。此时还没有
 * bar，`bars_since_*` 必须是 -1 而不是某个靠 -1 参与差值算出的数。
 */
TEST(IntentImmediacyTest, IntentBeforeFirstBarRecordsNoBarReference) {
    const auto ctx = make_ctx();
    ctx->buy(2, 100.0);  // 此刻 ctx 没有 bar（bar_index 仍为 -1）
    EXPECT_EQ(ctx->current_contracts(), 2);
    EXPECT_EQ(ctx->bars_since_entry(), -1);
    EXPECT_EQ(ctx->bars_since_last_entry(), -1);
    EXPECT_EQ(ctx->bars_since_exit(), -1);
}

// ============================================================
// Tier 1 口径（组件文档 §6.1）
// ============================================================

/// @brief `market_position` 由净持仓派生；`current_contracts` / `max_contracts` 非负
TEST(Tier1Test, DirectionIsDerivedAndCountsAreNonNegative) {
    const auto ctx = make_ctx();
    ctx->buy(5, 100.0);
    EXPECT_EQ(ctx->market_position(), 1);
    EXPECT_EQ(ctx->current_contracts(), 5);

    ctx->sell_short(9, 100.0);  // 反手到 -9
    EXPECT_EQ(ctx->market_position(), -1);
    EXPECT_EQ(ctx->current_contracts(), 9) << "current_contracts is an absolute value";
    EXPECT_EQ(ctx->max_contracts(), 9) << "max_contracts tracks |P| peak regardless of sign";

    ctx->buy_to_cover(0, 100.0);  // 全平
    EXPECT_EQ(ctx->market_position(), 0);
    EXPECT_EQ(ctx->current_contracts(), 0);
    EXPECT_EQ(ctx->max_contracts(), 9) << "max_contracts is a historical peak and never falls";
}

/// @brief 加权均价：加仓摊薄、部分平仓不变、反手重置
TEST(Tier1Test, AverageEntryPriceIsWeightedKeptOnPartialCloseAndResetOnReversal) {
    const auto ctx = make_ctx();
    ctx->buy(2, 100.0);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 100.0);

    // 加 2 手 @120 → (2×100 + 2×120) / 4 = 110
    ctx->buy(2, 120.0);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 110.0);
    EXPECT_EQ(ctx->current_entries(), 2) << "each add is one more entry segment";

    // 部分平仓 1 手 → 均价不变（剩余持仓的建仓成本没变）
    ctx->sell(1, 130.0);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 110.0);
    EXPECT_EQ(ctx->current_entries(), 2) << "partial close keeps the entry segment count";

    // 反手 → 均价重置为新成交价
    ctx->sell_short(3, 90.0);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), 90.0);
    EXPECT_EQ(ctx->current_entries(), 1) << "reversal starts a fresh entry segment";
    EXPECT_EQ(ctx->max_entries(), 2) << "max_entries keeps the historical peak";
}

/// @brief 建仓段数与历史峰值：每次开仓/加仓 +1，反手新开 +1，部分平仓不变
TEST(Tier1Test, EntrySegmentsCountOpensAndAddsAndPeakIsHistorical) {
    const auto ctx = make_ctx();
    ctx->buy(1, 100.0);
    EXPECT_EQ(ctx->current_entries(), 1);
    ctx->buy(1, 101.0);
    EXPECT_EQ(ctx->current_entries(), 2);
    ctx->sell(1, 102.0);  // 部分平仓
    EXPECT_EQ(ctx->current_entries(), 2);
    ctx->buy_to_cover(0, 103.0);  // 全平（当前无空，等价于无操作）
    ctx->sell(0, 103.0);          // 全平多
    EXPECT_EQ(ctx->current_entries(), 0) << "flat position has no entry segments";
    EXPECT_EQ(ctx->max_entries(), 2) << "peak survives going flat";

    ctx->sell_short(2, 99.0);  // 新开空
    EXPECT_EQ(ctx->current_entries(), 1);
    EXPECT_EQ(ctx->max_entries(), 2) << "peak survives a fresh position";
}

/**
 * @brief 进出场价格与时间取**对应 bar 的标准结束时间**（经真实 `advance_bar` 接线）
 *
 * 这里不手写时间戳、而是调真实的 `advance_bar`：`current_date_` / `current_time_` 由它从
 * `bar::Bar` 推导（取 `standard_end_sec()`），`bar_index_` 自增，价格写进窗口。若接线写错
 * （例如取了 tick 时间或开始时间），本用例的断言会失败——纯台账层的测试覆盖不到这一层，
 * 而它正是「进出场时间」口径的落地点。
 */
TEST(Tier1Test, EntryAndExitStampsUseTheBarStandardEndTime) {
    const auto ctx = make_ctx();
    // 第一根：begin_sec = 0，period_sec = 60 → 标准结束时间 = 60s = 墙钟 18:01:00
    dztrader::bar::Bar first{};
    first.trading_day = kTradingDay;
    first.begin_sec = 0;
    first.period_sec = 60;
    first.close = 100.0;
    first.is_final = false;
    ContextAccess::advance(*ctx, first);
    EXPECT_EQ(ctx->bar_index(), 0);

    ctx->buy(2, 88.5);
    EXPECT_DOUBLE_EQ(ctx->entry_price(), 88.5);
    EXPECT_EQ(ctx->entry_date(), kTradingDay) << "the bar's trading day";
    EXPECT_EQ(ctx->entry_time(), dztrader::bar::TradingTime::to_clock(60))
        << "the bar's *standard end* time (begin_sec + period_sec), not the tick time";
    EXPECT_EQ(ctx->bars_since_entry(), 0) << "same bar = 0";

    // 第二根：跨交易日、900 秒周期 → 标准结束时间 = 300 + 900 = 1200s（tsec）
    dztrader::bar::Bar second{};
    second.trading_day = kTradingDay + 1;
    second.begin_sec = 300;
    second.period_sec = 900;
    second.close = 95.0;
    second.is_final = false;
    ContextAccess::advance(*ctx, second);
    EXPECT_EQ(ctx->bar_index(), 1);
    EXPECT_EQ(ctx->bars_since_entry(), 1) << "one bar later";

    ctx->sell(1, 91.25);  // 部分平仓
    EXPECT_DOUBLE_EQ(ctx->entry_price(), 88.5) << "entry price is the first entry";
    EXPECT_DOUBLE_EQ(ctx->exit_price(), 91.25);
    EXPECT_EQ(ctx->exit_date(), kTradingDay + 1);
    EXPECT_EQ(ctx->exit_time(), dztrader::bar::TradingTime::to_clock(1200));
    EXPECT_EQ(ctx->bars_since_exit(), 0) << "closed on this bar = 0";

    // 第三根：加仓更新 last_entry_*，不动 entry_*
    dztrader::bar::Bar third{};
    third.trading_day = kTradingDay + 1;
    third.begin_sec = 1200;
    third.period_sec = 900;
    third.close = 95.0;
    third.is_final = false;
    ContextAccess::advance(*ctx, third);
    ctx->buy(1, 95.0);
    EXPECT_DOUBLE_EQ(ctx->entry_price(), 88.5);
    EXPECT_DOUBLE_EQ(ctx->last_entry_price(), 95.0);
    EXPECT_EQ(ctx->bars_since_last_entry(), 0);
    EXPECT_EQ(ctx->bars_since_entry(), 2);
}

/**
 * @brief 盈亏取窗口 `[0]` 的最新价、乘合约乘数、随 bar 收盘价变化；无仓 0
 *
 * `position_profit()` 的「最新价」不是另存一个字段，而是读当前 bar 的 `close`——这样
 * `on_bar_update` 里每次 tick 都能读到最新价，没有第二份真相需要同步。本用例经真实
 * `advance_bar` 把价格写进窗口，因此同时覆盖「盈亏公式」与「最新价来源」两件事。
 */
TEST(Tier1Test, ProfitReadsTheWindowCloseAndMultipliesContractMultiplier) {
    const auto ctx = make_ctx();
    // 回调门禁之下 B 只在窗口填满后才会被回调；这里不需要推进 32 根，只验证盈亏口径，
    // 所以直接推进一根（`advance_bar` 不判门禁，门禁在 A 的分发里）。
    dztrader::bar::Bar bar{};
    bar.trading_day = kTradingDay;
    bar.begin_sec = 0;
    bar.period_sec = 60;
    bar.close = 100.0;
    bar.is_final = false;
    ContextAccess::advance(*ctx, bar);

    EXPECT_DOUBLE_EQ(ctx->position_profit(), 0.0) << "flat position has no floating profit";
    EXPECT_DOUBLE_EQ(ctx->contract_profit(), 0.0);

    ctx->buy(3, 100.0);
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 0.0) << "just opened at the current price";

    // 同一根 bar 内最新价上移：更新 [0] 后盈亏立刻反映（每 tick 一次 update 的口径）
    bar.close = 110.0;
    ContextAccess::update_current(*ctx, bar);
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 300.0) << "(110-100) × 3 × 10";
    EXPECT_DOUBLE_EQ(ctx->contract_profit(), 100.0) << "300 / 3 手 = 每手 100";

    // 亏损是负的钱，不是价格点数；空头方向相反
    bar.close = 95.0;
    ContextAccess::update_current(*ctx, bar);
    EXPECT_DOUBLE_EQ(ctx->position_profit(), -150.0) << "(95-100) × 3 × 10";

    // 每手盈亏按 §6.1 公式直接乘，等于总盈亏 / 手数（手数整除时两者逐位相同）
    EXPECT_DOUBLE_EQ(ctx->contract_profit() * static_cast<double>(ctx->current_contracts()),
                     ctx->position_profit());

    ctx->sell_short(4, 95.0);  // 反手到 -4，均价 95
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 0.0) << "reversed at the current price";
    bar.close = 90.0;
    ContextAccess::update_current(*ctx, bar);
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 200.0) << "(90-95) × -4 × 10";
    EXPECT_DOUBLE_EQ(ctx->contract_profit(), 50.0) << "200 / 4 手";

    ctx->buy_to_cover(0, 90.0);  // 全平
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 0.0);
    EXPECT_DOUBLE_EQ(ctx->contract_profit(), 0.0);
}

/**
 * @brief 浮盈浮亏极值：bar 收盘前更新、开仓重置 0、平仓归零
 *
 * 直接调 ctx 的**真实** `update_profit_extremes`（经内部缝），而不是在测试里重写一遍公式：
 * 重写会变成「测试验证测试」。乘数取自 `InstrumentSpec`（本文件的 `make_ctx` 注入 10），
 * 因此断言里的数字同时证明「乘了合约乘数」。
 *
 * 极值只跟踪**当前持仓期间**，所以反手（新开）必须归零——否则上一段的极值会当成新持仓的
 * 历史，止盈止损逻辑会读到虚假的「曾经盈利」。
 */
TEST(Tier1Test, ProfitExtremesUpdatePerCloseResetOnOpenAndZeroAfterFlat) {
    const auto ctx = make_ctx();

    dztrader::bar::Bar bar{};
    bar.trading_day = kTradingDay;
    bar.begin_sec = 0;
    bar.period_sec = 60;
    bar.close = 100.0;
    bar.is_final = false;
    ContextAccess::advance(*ctx, bar);
    ctx->buy(2, 100.0);  // 均价 100、2 手、乘数 10

    // 无仓时没有被累积（这条底线在下面"平仓后"再验证一次）
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0);

    ContextAccess::update_profit_extremes(*ctx, 110.0);  // (110-100)×2×10 = 200
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 200.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0) << "no loss seen yet";

    ContextAccess::update_profit_extremes(*ctx, 95.0);  // (95-100)×2×10 = -100
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 200.0) << "profit extreme survives a pullback";
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), -100.0);

    ContextAccess::update_profit_extremes(*ctx, 120.0);  // 浮盈 400（新高）
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 400.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), -100.0) << "loss extreme survives";

    // 平仓：归零
    ctx->sell(0, 120.0);
    EXPECT_EQ(ctx->current_contracts(), 0);
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0);

    // 平仓后继续收 bar：极值不会被重新累积
    ContextAccess::update_profit_extremes(*ctx, 200.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0) << "flat position accumulates no extremes";

    // 反手新开也归零：新的持仓期间从 0 起算极值
    ctx->sell_short(1, 90.0);
    EXPECT_EQ(ctx->market_position(), -1);
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0);
    // 空头浮盈方向相反：价格跌 → 浮盈 (80-90)×-1×10 = +100
    ContextAccess::update_profit_extremes(*ctx, 80.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 100.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0);
}

/**
 * @brief **没有价格时不产生虚假盈亏**（无 bar 的 ctx / `on_init` 期）
 *
 * 回归用例：`current_price()` 在无 bar 时返回 0（哨兵），如果盈亏公式不识别这个哨兵，
 * `(0 − 均价) × P × 乘数` 会算出一笔巨大的**虚假浮亏**（均价 100、2 手、乘数 10 → −2000）。
 * B 在 `on_init` 里下单后读 `position_profit()` 就会拿到它，并据此写出错误的止损逻辑。
 * 口径是「无仓 0、有仓但没有价格也 0」——没有价格时「浮盈」没有定义，返回 0 而不是编一个数。
 */
TEST(Tier1Test, ProfitIsZeroWhenNoPriceExistsYet) {
    const auto ctx = make_ctx();  // 未推进窗口：没有任何 bar，也没有价格
    ctx->buy(2, 100.0);
    ASSERT_EQ(ctx->current_contracts(), 2) << "the position itself is booked immediately";
    EXPECT_DOUBLE_EQ(ctx->position_profit(), 0.0) << "no price yet, so no floating profit";
    EXPECT_DOUBLE_EQ(ctx->contract_profit(), 0.0);

    // 极值更新同样不能用哨兵价格算出虚假极值
    ContextAccess::update_profit_extremes(*ctx, 0.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0) << "a zero price must not book a loss";
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0);
}

/// @brief 无仓时极值更新是无操作（A 在每次 close 都会调，不能因此攒出极值）
TEST(Tier1Test, ProfitExtremesAreNotAccumulatedWhileFlat) {
    const auto ctx = make_ctx();
    for (const double close : {100.0, 200.0, 300.0}) {
        ContextAccess::update_profit_extremes(*ctx, close);
    }
    EXPECT_DOUBLE_EQ(ctx->max_position_profit(), 0.0);
    EXPECT_DOUBLE_EQ(ctx->max_position_loss(), 0.0);
}

// ============================================================
// 拒单：逻辑持仓与 Tier 1 完全不变
// ============================================================

/**
 * @brief 负手数与非法价格拒单：状态一个字段都不动
 *
 * 被拒意图若在 Tier 1 里留下痕迹（哪怕只是 `max_contracts`），台账就再也无法自证——均价会被
 * 一个没成交的价格污染，且没有任何字段能看出来。所以这里逐字段比较快照。
 */
TEST(IntentRejectionTest, NegativeVolumeAndInvalidPriceLeaveTheLedgerUntouched) {
    const auto ctx = make_ctx();
    ctx->buy(2, 100.0);
    const LargeVolume net = ctx->current_contracts();
    const double avg = ctx->avg_entry_price();
    const std::int32_t entries = ctx->current_entries();
    const LargeVolume peak = ctx->max_contracts();

    ctx->buy(-5, 100.0);
    ctx->sell(-1, 100.0);
    ctx->sell_short(-2, 100.0);
    ctx->buy_to_cover(-3, 100.0);
    EXPECT_EQ(ctx->market_position(), 1) << "rejected intents must not change direction";
    EXPECT_EQ(ctx->current_contracts(), net);
    EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), avg);
    EXPECT_EQ(ctx->current_entries(), entries);
    EXPECT_EQ(ctx->max_contracts(), peak);

    // 非法价格：NaN / inf / ≤ 0 全部拒单
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(), 0.0, -1.0}) {
        ctx->buy(1, bad);
        EXPECT_EQ(ctx->current_contracts(), net)
            << "invalid price must be rejected | price=" << bad;
        EXPECT_DOUBLE_EQ(ctx->avg_entry_price(), avg);
    }
}

/**
 * @brief 失败实例上，`volume == 0` 且无可平仓同样**不诊断**（§6 与 §4.6 的交点）
 *
 * 失败实例要拒绝一切意图（§4.6），但「零值 + 无可平仓」按 §6 是**无操作**、不该诊断——
 * 策略里的平仓语句逐 bar 无条件调用，实例失败后若每根都刷一行拒单诊断，日志会被淹没。
 * 有可平仓的意图仍然必须拒单并诊断。
 */
TEST(IntentDiagnosticsTest, FailedInstanceStaysSilentOnZeroVolumeNoOp) {
    const auto ctx = make_ctx();
    ContextAccess::mark_failed(*ctx);

    testing::internal::CaptureStderr();
    ctx->sell(0, 100.0);          // 空仓：无可平多 → 无操作，静默
    ctx->buy_to_cover(0, 100.0);  // 空仓：无可平空 → 无操作，静默
    const std::string quiet = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(quiet.empty()) << "zero-volume no-op on a failed instance must not diagnose: "
                               << quiet;

    // 对照：有可平仓的意图必须拒单并诊断（实例已失败）
    ctx->buy(2, 100.0);  // 失败实例：拒单 → 持仓仍为 0
    EXPECT_EQ(ctx->current_contracts(), 0);
    testing::internal::CaptureStderr();
    ctx->sell(1, 100.0);
    const std::string rejected = testing::internal::GetCapturedStderr();
    EXPECT_NE(rejected.find("reason=instance_failed"), std::string::npos) << rejected;
}

/// @brief 失败实例不再受理下单意图（组件文档 §4.6 / AC「失败实例不再受理下单意图」）
TEST(IntentRejectionTest, FailedInstanceRejectsEveryIntent) {
    const auto ctx = make_ctx();
    ctx->buy(2, 100.0);
    ContextAccess::mark_failed(*ctx);

    ctx->buy(5, 100.0);
    ctx->sell(1, 100.0);
    ctx->sell_short(5, 100.0);
    ctx->buy_to_cover(0, 100.0);
    EXPECT_EQ(ctx->market_position(), 1) << "the retained logical position is untouched";
    EXPECT_EQ(ctx->current_contracts(), 2);
}

// ============================================================
// 诊断分级（组件文档 §6「参数与诊断」）
// ============================================================

/**
 * @brief 诊断的**存在性**与 prose：WARN（无效方向/钳制）、INFO（0 触发平仓）、
 *        拒单（负手数/非法价格）、零值无操作**不诊断**
 *
 * 断言 prose 而不只是「有输出」：分级靠 prose 区分（master 把子进程 stderr 一律按 WARN
 * 转发，日志级别只看得到 prose），所以「打了日志」不等于「打了对的那一级」。
 */
TEST(IntentDiagnosticsTest, DiagnosticsAreGradedByProse) {
    const auto ctx = make_ctx();

    // 1) 0 且无可平仓：**不诊断**（避免平仓语句逐 bar 刷屏）
    testing::internal::CaptureStderr();
    ctx->sell(0, 100.0);
    ctx->buy_to_cover(0, 100.0);
    ctx->buy(0, 100.0);
    ctx->sell_short(0, 100.0);
    const std::string quiet = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(quiet.empty()) << "zero volume with nothing to close must not diagnose: " << quiet;

    // 2) n > 0 方向无效：WARN（intent ignored）
    testing::internal::CaptureStderr();
    ctx->sell(3, 100.0);
    const std::string ignored = testing::internal::GetCapturedStderr();
    EXPECT_NE(ignored.find("intent ignored |"), std::string::npos) << ignored;
    EXPECT_NE(ignored.find("instrument=rb2601"), std::string::npos) << ignored;
    EXPECT_NE(ignored.find("intent=sell"), std::string::npos) << ignored;

    // 3) 超仓钳制：WARN（intent clamped）
    ctx->buy(2, 100.0);
    testing::internal::CaptureStderr();
    ctx->sell(99, 100.0);
    const std::string clamped = testing::internal::GetCapturedStderr();
    EXPECT_NE(clamped.find("intent clamped |"), std::string::npos) << clamped;
    EXPECT_EQ(ctx->current_contracts(), 0) << "clamping still flattens";

    // 4) `volume = 0` 实际触发平仓：INFO（intent flat_by_zero）
    ctx->buy(3, 100.0);
    testing::internal::CaptureStderr();
    ctx->sell(0, 100.0);
    const std::string flat = testing::internal::GetCapturedStderr();
    EXPECT_NE(flat.find("intent flat_by_zero |"), std::string::npos) << flat;
    EXPECT_EQ(ctx->current_contracts(), 0);

    // 5) 拒单：负手数 / 非法价格
    testing::internal::CaptureStderr();
    ctx->buy(-1, 100.0);
    const std::string negative = testing::internal::GetCapturedStderr();
    EXPECT_NE(negative.find("intent rejected |"), std::string::npos) << negative;
    EXPECT_NE(negative.find("reason=negative_volume"), std::string::npos) << negative;

    testing::internal::CaptureStderr();
    ctx->buy(1, 0.0);
    const std::string price = testing::internal::GetCapturedStderr();
    EXPECT_NE(price.find("intent rejected |"), std::string::npos) << price;
    EXPECT_NE(price.find("reason=invalid_price"), std::string::npos) << price;

    // 6) 失败实例：拒单并诊断
    ContextAccess::mark_failed(*ctx);
    testing::internal::CaptureStderr();
    ctx->buy(1, 100.0);
    const std::string failed = testing::internal::GetCapturedStderr();
    EXPECT_NE(failed.find("intent rejected |"), std::string::npos) << failed;
    EXPECT_NE(failed.find("reason=instance_failed"), std::string::npos) << failed;
}

/**
 * @brief **`volume == 0` 且无可平仓时，价格非法也不诊断**（组件文档 §6 明文）
 *
 * 回归用例。`sell(0, price)` 是策略里最常见的无条件平仓语句，每个 bar 都会执行；当方向没有
 * 持仓时它是**无操作**，§6 明确要求「`0` 且无可平仓时**不诊断**」。若价格校验排在「是否有
 * 变化」之前，一次 `sell(0, 0.0)` 就会打出拒单诊断——价格非法在这种情况下无关紧要，因为
 * 根本没有要用到这个价格的动作。
 */
TEST(IntentDiagnosticsTest, ZeroVolumeNoOpStaysSilentEvenWithAnInvalidPrice) {
    const auto ctx = make_ctx();

    testing::internal::CaptureStderr();
    // 空仓：买方向无可平空、卖方向无可平多——全部无操作，且价格非法（0）也不该诊断
    ctx->sell(0, 0.0);
    ctx->sell_short(0, 0.0);
    ctx->buy(0, 0.0);
    ctx->buy_to_cover(0, 0.0);
    const std::string quiet = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(quiet.empty())
        << "zero volume with nothing to close must not diagnose, even with price 0: " << quiet;
    EXPECT_EQ(ctx->current_contracts(), 0);

    // 对照：同样非法价格、但**有**可平仓 → 必须拒单（价格非法在这里是真的问题）
    ctx->buy(2, 100.0);
    testing::internal::CaptureStderr();
    ctx->sell(1, 0.0);
    const std::string rejected = testing::internal::GetCapturedStderr();
    EXPECT_NE(rejected.find("intent rejected |"), std::string::npos) << rejected;
    EXPECT_NE(rejected.find("reason=invalid_price"), std::string::npos) << rejected;
    EXPECT_EQ(ctx->current_contracts(), 2) << "the rejected intent must not change the ledger";
}

/// @brief 正常受理的意图**不打**日志（热路径无日志约束，组件文档 §10）
TEST(IntentDiagnosticsTest, AcceptedIntentsAreSilent) {
    const auto ctx = make_ctx();
    testing::internal::CaptureStderr();
    ctx->buy(2, 100.0);
    ctx->buy(1, 101.0);
    ctx->sell(1, 102.0);
    ctx->sell_short(3, 103.0);
    const std::string silent = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(silent.empty()) << "accepted intents must not log (hot path): " << silent;
}

// ============================================================
// 编译期：账户身份对 B 不可见（ADR 0028）
// ============================================================

/**
 * @brief 类型 T 上是否存在可调用的 `account_id()` / `accounts()`
 *
 * 必须是命名空间作用域的类模板（局部类不能有成员模板），所以检测器写在测试函数外面。
 */
template <class T, class = void>
struct has_account_id : std::false_type {};

template <class T>
struct has_account_id<T, std::void_t<decltype(std::declval<T>().account_id())>> : std::true_type {};

template <class T, class = void>
struct has_accounts : std::false_type {};

template <class T>
struct has_accounts<T, std::void_t<decltype(std::declval<T>().accounts())>> : std::true_type {};

/// @brief 变量模板形式的检测结果（`static_assert(!has_account_id_v<T>)` 比 `...::value` 好读）
template <class T>
inline constexpr bool has_account_id_v = has_account_id<T>::value;

template <class T>
inline constexpr bool has_accounts_v = has_accounts<T>::value;

/**
 * @brief B 的 API 面里没有任何账户标识
 *
 * ADR 0028：策略进程只有一个逻辑账户（纯虚拟台账，不落地、不参与订单路由），挂靠账户 0..N
 * 个对 B **完全不可见**。这条不是「实现时记得别暴露」，而是编译期约束：如果哪天有人给 ctx
 * 加了 `account_id()` 或 `accounts()`，这里的检测会失败。
 */
TEST(AccountIdentityTest, ContextExposesNoAccountIdentity) {
    static_assert(!has_account_id_v<InstrumentContext>,
                  "B must not see any account identity (ADR 0028)");
    static_assert(!has_accounts_v<InstrumentContext>,
                  "B must not see the attached account table (ADR 0028)");

    // 四函数签名只接受策略手数，没有任何账户参数（ADR 0030：手数缩放由框架内部完成）
    static_assert(std::is_same_v<decltype(&InstrumentContext::buy),
                                 void (InstrumentContext::*)(Volume, double)>);
    static_assert(std::is_same_v<decltype(&InstrumentContext::sell),
                                 void (InstrumentContext::*)(Volume, double)>);
    static_assert(std::is_same_v<decltype(&InstrumentContext::sell_short),
                                 void (InstrumentContext::*)(Volume, double)>);
    static_assert(std::is_same_v<decltype(&InstrumentContext::buy_to_cover),
                                 void (InstrumentContext::*)(Volume, double)>);
}

}  // namespace
