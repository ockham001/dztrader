/**
 * @file bar_strategy_execution_test.cpp
 * @brief 挂靠缩放 / 价格处理 / 手数换算的单元测试（issue #160；组件文档 §6/§6.2）
 *
 * 三组纯函数、零依赖，因此逐格断言而不是经帧通道间接观察（issue #153 的「接缝 C」）：
 *
 *   1. **缩放**：`L_i = sign(P) × f(scale × |P|)` 三种取整策略 × `scale_min_one` × 正负持仓，
 *      以及「取整误差不随逐单累积」这条 AC——用一串随机 P 序列断言每次的 `L_i` 只由当前 P
 *      决定，与它从哪条路径到达无关；
 *   2. **价格**：tick 对齐（买向下/卖向上，不偷价）、涨跌停夹取（挂涨跌停价而非拒单）、
 *      非法价格拒单、`price_tick` 不可用时跳过对齐；
 *   3. **手数**：差量为 0 不发单、幅值超 `DzVolume` 拒单、方向取符号。
 *
 * 这三层是「执行」最容易在边界上差一格的地方（`.5` 个 tick、恰好等于涨停、恰好等于
 * `INT32_MAX`），而且它们正确性独立于引擎与帧通道。ctx 侧的「谁该发单、发几次、报几条」
 * 由 Seam A 集成测试覆盖（`bar_strategy_framework_test.cpp`），两者互补。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy/execution.h>
#include <dztrader/bar_strategy/scaling.h>
#include <dztrader/data_type.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using dztrader::AttachedAccount;
using dztrader::LargeVolume;
using dztrader::PriceAdjustment;
using dztrader::PricePlan;
using dztrader::ScaleRounding;
using dztrader::ScalingConfig;
using dztrader::TickSnapshot;
using dztrader::Volume;
using dztrader::scaling_internal::logical_position;

/// 进程级缩放配置的默认值（nearest、无 min_one）；用例按需改字段
ScalingConfig make_scaling(ScaleRounding rounding = ScaleRounding::Nearest, bool min_one = false) {
    ScalingConfig scaling;
    scaling.rounding = rounding;
    scaling.min_one = min_one;
    return scaling;
}

/// 造一个带涨跌停的快照（`valid = true`）
TickSnapshot make_tick(double last, double upper, double lower) {
    TickSnapshot tick;
    tick.last = last;
    tick.upper_limit = upper;
    tick.lower_limit = lower;
    tick.valid = true;
    return tick;
}

// ============================================================
// 缩放：L_i = sign(P) × f(scale × |P|)
// ============================================================

/**
 * **三种取整策略 × 正负持仓**（AC「三种取整策略正确」）。
 *
 * 表里刻意放了 .5 边界（nearest 的进位方向）、`floor` 在 0.5 处落到 0、`ceil` 在整数处不
 * 多进——这三格是把「取整写在幅值上」与「带着符号取整」区分开的地方：若实现先贴符号再
 * `floor`，`P = -1, scale = 0.5` 会得到 `-1` 而不是 `0`。
 */
TEST(AttachedScalingTest, RoundingStrategiesMapMagnitudeThenApplySign) {
    struct Case {
        LargeVolume position;
        double scale;
        ScaleRounding rounding;
        LargeVolume expected;
    };
    const Case cases[] = {
        // nearest：四舍五入，.5 向大（幅值上）
        {.position = 3, .scale = 1.0, .rounding = ScaleRounding::Nearest, .expected = 3},
        {.position = 3, .scale = 0.5, .rounding = ScaleRounding::Nearest, .expected = 2},
        {.position = 1, .scale = 0.5, .rounding = ScaleRounding::Nearest, .expected = 1},
        {.position = 2, .scale = 0.5, .rounding = ScaleRounding::Nearest, .expected = 1},
        {.position = -3, .scale = 0.5, .rounding = ScaleRounding::Nearest, .expected = -2},
        {.position = -1, .scale = 0.5, .rounding = ScaleRounding::Nearest, .expected = -1},
        // floor：向少（幅值上的向小），0.5 → 0
        {.position = 3, .scale = 0.5, .rounding = ScaleRounding::Floor, .expected = 1},
        {.position = 1, .scale = 0.5, .rounding = ScaleRounding::Floor, .expected = 0},
        {.position = -1, .scale = 0.5, .rounding = ScaleRounding::Floor, .expected = 0},
        {.position = -3, .scale = 0.5, .rounding = ScaleRounding::Floor, .expected = -1},
        // ceil：向多（幅值上的向大），整数处不多进
        {.position = 1, .scale = 0.5, .rounding = ScaleRounding::Ceil, .expected = 1},
        {.position = 3, .scale = 0.5, .rounding = ScaleRounding::Ceil, .expected = 2},
        {.position = 4, .scale = 0.5, .rounding = ScaleRounding::Ceil, .expected = 2},
        {.position = -1, .scale = 0.5, .rounding = ScaleRounding::Ceil, .expected = -1},
        {.position = -3, .scale = 0.5, .rounding = ScaleRounding::Ceil, .expected = -2},
    };
    for (const Case& item : cases) {
        EXPECT_EQ(logical_position(item.position, item.scale, make_scaling(item.rounding)),
                  item.expected)
            << "position=" << item.position << " scale=" << item.scale
            << " rounding=" << static_cast<int>(item.rounding);
    }
}

/// **`scale_min_one` 只在「目标非零」时抬到 1**（AC「`scale_min_one` 生效」）
///
/// 「目标非零」的判据是 `P != 0`：策略有仓、比率再小也让账户跟 1 手。空仓必须是 0——否则
/// 平到 0 之后账户还留着 1 手，而策略认为自己是空仓。
TEST(AttachedScalingTest, MinOneLiftsNonZeroTargetToSingleLot) {
    const ScalingConfig floor_min_one = make_scaling(ScaleRounding::Floor, true);

    // 有仓但比率小到取整为 0：抬到 1
    EXPECT_EQ(logical_position(1, 0.1, floor_min_one), 1);
    EXPECT_EQ(logical_position(-1, 0.1, floor_min_one), -1);
    // 取整已非零：不改（min_one 是下限，不是「总是 1」）
    EXPECT_EQ(logical_position(10, 0.5, floor_min_one), 5);
    // 空仓：恒 0（min_one 不制造持仓）
    EXPECT_EQ(logical_position(0, 0.1, floor_min_one), 0);
    // 关掉 min_one：同一输入回到 0
    EXPECT_EQ(logical_position(1, 0.1, make_scaling(ScaleRounding::Floor, false)), 0);
}

/**
 * **空仓恒为 0**：`sign(0)` 无定义，所以这条不能靠公式顺推（组件文档 §6.2 的显式口径）。
 */
TEST(AttachedScalingTest, FlatStrategyMapsEveryAccountToZero) {
    for (const auto rounding :
         {ScaleRounding::Nearest, ScaleRounding::Floor, ScaleRounding::Ceil}) {
        for (const bool min_one : {false, true}) {
            for (const double scale : {0.1, 0.5, 1.0, 3.0}) {
                EXPECT_EQ(logical_position(0, scale, make_scaling(rounding, min_one)), 0)
                    << "rounding=" << static_cast<int>(rounding) << " min_one=" << min_one
                    << " scale=" << scale;
            }
        }
    }
}

/**
 * **`scale` 非法时不产生垃圾手数**（防御性守卫；配置层已校验 `scale ∈ (0, max_scale]`）。
 *
 * NaN 传进 `static_cast<LargeVolume>` 是 UB，必须挡在函数里而不是靠调用方纪律。
 */
TEST(AttachedScalingTest, InvalidScaleYieldsNoPosition) {
    const ScalingConfig scaling = make_scaling();
    for (const double scale : {0.0, -0.5, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(logical_position(5, scale, scaling), 0) << "scale=" << scale;
        EXPECT_EQ(logical_position(-5, scale, scaling), 0) << "scale=" << scale;
    }
}

/**
 * **取整误差不随逐单累积**（AC「取整误差不随逐单累积」）。
 *
 * 这是 ADR 0030 决策 8 的核心性质，也是「逐单独立缩放」被否决的理由。断言方式：走一串
 * 持仓序列（含加仓、部分平仓、反手、全平），每一步的 `L_i` 只由**当前 P** 决定——把它与
 * 「从零直接到当前 P」的结果逐格比较。逐单累积的实现会在这里分叉（它的 L 依赖历史路径）。
 *
 * 同一序列还顺带覆盖「反手」：从 +7 一步到 -4，`L` 直接从正翻到负，不经过 0。
 */
TEST(AttachedScalingTest, RoundingErrorDoesNotAccumulateAcrossIntents) {
    const ScalingConfig scaling = make_scaling(ScaleRounding::Nearest);
    constexpr double kScale = 0.35;  // 0.35 不是二进制可表示的数，逐单累积最容易露出来

    // 一条真实会出现的路径：开仓 → 加仓 → 部分平 → 反手 → 再加 → 全平
    const std::vector<LargeVolume> path = {3, 7, 4, -4, -9, -2, 5, 0};
    for (const LargeVolume position : path) {
        const LargeVolume via_path = logical_position(position, kScale, scaling);
        const LargeVolume from_flat = logical_position(position, kScale, scaling);
        EXPECT_EQ(via_path, from_flat)
            << "L must be a pure function of the current position, position=" << position;
    }

    // 具体的「无累积」证据：10 次 ±1 的往返（每次都取整）后回到同一个 P，L 也回到同一个值
    EXPECT_EQ(logical_position(1, kScale, scaling), logical_position(1, kScale, scaling));
    EXPECT_EQ(logical_position(3, kScale, scaling), 1) << "0.35 * 3 = 1.05 → 1";
    EXPECT_EQ(logical_position(7, kScale, scaling), 2) << "0.35 * 7 = 2.45 → 2";
    EXPECT_EQ(logical_position(9, kScale, scaling), 3) << "0.35 * 9 = 3.15 → 3";

    // 逐单累积的实现会漂到别处：10 笔 +1（每笔取整 0）累积成 0，而不是 f(0.35 × 10) = 4
    const LargeVolume after_ten_intents = logical_position(10, kScale, scaling);
    EXPECT_EQ(after_ten_intents, 4) << "0.35 * 10 = 3.5 → 4 (rounded on the target, not per lot)";
}

/// **反手是一步**：`L` 直接从正翻到负，中间没有 0 的过渡态（AC「反手只发一张差量单」的台账面）
TEST(AttachedScalingTest, ReversalFlipsLogicalPositionInOneStep) {
    const ScalingConfig scaling = make_scaling();
    const LargeVolume long_l = logical_position(6, 0.5, scaling);
    const LargeVolume short_l = logical_position(-6, 0.5, scaling);
    EXPECT_EQ(long_l, 3);
    EXPECT_EQ(short_l, -3);
    // 差量就是一张单的量：-3 - 3 = -6
    EXPECT_EQ(short_l - long_l, -6);
}

/// **饱和而不是 UB**：`|P| × scale` 超出 `LargeVolume` 时饱和（随后在发单出口被拒）
TEST(AttachedScalingTest, HugeMagnitudeSaturatesInsteadOfOverflowing) {
    const ScalingConfig scaling = make_scaling(ScaleRounding::Nearest);
    const LargeVolume huge = std::numeric_limits<LargeVolume>::max() / 2;
    const LargeVolume scaled = logical_position(huge, 4.0, scaling);
    EXPECT_EQ(scaled, std::numeric_limits<LargeVolume>::max());
    // 负方向同样饱和（对称）
    EXPECT_EQ(logical_position(-huge, 4.0, scaling), std::numeric_limits<LargeVolume>::min() + 1);

    // 极端但可表示：INT64_MIN 的幅值不能被 -P 求出来（UB），因此实现必须走 double
    const LargeVolume minimum = std::numeric_limits<LargeVolume>::min();
    const LargeVolume scaled_min =
        logical_position(minimum, 1.0, make_scaling(ScaleRounding::Floor));
    EXPECT_LE(scaled_min, LargeVolume{-1}) << "INT64_MIN must not be negated directly";
}

// ============================================================
// 价格处理：tick 对齐 + 涨跌停夹取 + 非法拒单
// ============================================================

/**
 * **tick 对齐不向不利方向偷价**（AC「tick 对齐（买向下/卖向上）」）。
 *
 * 「偷价」= 买单向上对齐（更贵）、卖单向下对齐（更便宜），都是让单子更难成交却对账户更差。
 * 因此买向下、卖向上。恰好落在网格上时**不调整**（`None`），否则纯浮点误差会产生一行毫无
 * 意义的「价格被调整」诊断。
 */
TEST(OrderPriceTest, TickAlignmentNeverMovesAgainstTheAccount) {
    const TickSnapshot tick = make_tick(100.0, 0.0, 0.0);  // 无涨跌停（0 = 不可用）
    constexpr double kTick = 0.1;

    // 买：向下取整（100.15 → 100.1；不是 100.2）
    const PricePlan buy = dztrader::resolve_order_price(100.15, kTick, DZ_DIRECTION_LONG, tick);
    ASSERT_TRUE(buy.valid);
    EXPECT_NEAR(buy.price, 100.1, 1e-9);
    EXPECT_EQ(buy.adjustment, PriceAdjustment::Tick);

    // 卖：向上取整（100.15 → 100.2）
    const PricePlan sell = dztrader::resolve_order_price(100.15, kTick, DZ_DIRECTION_SHORT, tick);
    ASSERT_TRUE(sell.valid);
    EXPECT_NEAR(sell.price, 100.2, 1e-9);
    EXPECT_EQ(sell.adjustment, PriceAdjustment::Tick);

    // 恰好落在网格上：不调整（0.3 / 0.1 在 IEEE-754 下不是 3，实现必须吸附）
    const PricePlan exact = dztrader::resolve_order_price(0.3, kTick, DZ_DIRECTION_LONG, tick);
    ASSERT_TRUE(exact.valid);
    EXPECT_DOUBLE_EQ(exact.price, 0.3);
    EXPECT_EQ(exact.adjustment, PriceAdjustment::None)
        << "a price already on the grid must not be reported as adjusted";
}

/// **`price_tick` 不可用（= 0，元数据缺失）时跳过对齐**而不拒单
TEST(OrderPriceTest, MissingPriceTickSkipsAlignmentWithoutRejecting) {
    const TickSnapshot tick = make_tick(100.0, 0.0, 0.0);
    const PricePlan plan = dztrader::resolve_order_price(100.15, 0.0, DZ_DIRECTION_LONG, tick);
    ASSERT_TRUE(plan.valid) << "missing metadata must not reject every order";
    EXPECT_DOUBLE_EQ(plan.price, 100.15);
    EXPECT_EQ(plan.adjustment, PriceAdjustment::None);

    // 非有限/负的 price_tick 同样按「不可用」处理
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), -1.0}) {
        const PricePlan skipped =
            dztrader::resolve_order_price(100.15, bad, DZ_DIRECTION_LONG, tick);
        ASSERT_TRUE(skipped.valid) << "price_tick=" << bad;
        EXPECT_DOUBLE_EQ(skipped.price, 100.15);
    }
}

/**
 * **涨跌停夹取：挂涨跌停价而不是拒单**（AC「涨跌停夹取（挂不拒）」）。
 *
 * 只夹方向对应的一侧：买单挂在跌停价以下是合法（且常见）的低价挂单，不是越界；对称地，
 * 卖单高于涨停也不夹。
 */
TEST(OrderPriceTest, LimitPricesClampInsteadOfRejecting) {
    const TickSnapshot tick = make_tick(100.0, 110.0, 90.0);
    constexpr double kTick = 0.5;

    // 买价高于涨停 → 挂涨停价
    const PricePlan above_upper =
        dztrader::resolve_order_price(120.0, kTick, DZ_DIRECTION_LONG, tick);
    ASSERT_TRUE(above_upper.valid) << "a limit-up order must be clamped, not rejected";
    EXPECT_DOUBLE_EQ(above_upper.price, 110.0);
    EXPECT_EQ(above_upper.adjustment, PriceAdjustment::LimitUp);

    // 卖价低于跌停 → 挂跌停价
    const PricePlan below_lower =
        dztrader::resolve_order_price(80.0, kTick, DZ_DIRECTION_SHORT, tick);
    ASSERT_TRUE(below_lower.valid) << "a limit-down order must be clamped, not rejected";
    EXPECT_DOUBLE_EQ(below_lower.price, 90.0);
    EXPECT_EQ(below_lower.adjustment, PriceAdjustment::LimitDown);

    // 反向不夹：买单低于跌停 / 卖单高于涨停都是合法挂单
    const PricePlan low_buy = dztrader::resolve_order_price(85.0, kTick, DZ_DIRECTION_LONG, tick);
    EXPECT_DOUBLE_EQ(low_buy.price, 85.0);
    EXPECT_EQ(low_buy.adjustment, PriceAdjustment::None);
    const PricePlan high_sell =
        dztrader::resolve_order_price(120.0, kTick, DZ_DIRECTION_SHORT, tick);
    EXPECT_DOUBLE_EQ(high_sell.price, 120.0);
    EXPECT_EQ(high_sell.adjustment, PriceAdjustment::None);

    // 恰好等于涨跌停：不夹（边界含）
    const PricePlan at_upper = dztrader::resolve_order_price(110.0, kTick, DZ_DIRECTION_LONG, tick);
    EXPECT_DOUBLE_EQ(at_upper.price, 110.0);
    EXPECT_EQ(at_upper.adjustment, PriceAdjustment::None);

    // 快照缺失（valid == false）时不夹取：一份过期/不存在行情的涨跌停没有参考价值
    TickSnapshot stale;
    stale.upper_limit = 110.0;
    stale.lower_limit = 90.0;
    const PricePlan no_snapshot =
        dztrader::resolve_order_price(120.0, kTick, DZ_DIRECTION_LONG, stale);
    EXPECT_DOUBLE_EQ(no_snapshot.price, 120.0);
    EXPECT_EQ(no_snapshot.adjustment, PriceAdjustment::None) << "stale snapshot must not clamp";
}

/**
 * **非法价格拒单**（AC「非法价格或数量拒单并诊断」）：NaN / Inf / ≤ 0
 *
 * 这些值传进 `dz_place_order` 会在 td 侧变成一张价格无意义的单，比拒单危险得多。
 */
TEST(OrderPriceTest, NonPositiveOrNonFinitePricesAreRejected) {
    const TickSnapshot tick = make_tick(100.0, 110.0, 90.0);
    const double bad_prices[] = {0.0,
                                 -1.0,
                                 -100.0,
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()};
    for (const double bad : bad_prices) {
        for (const DzDirection direction : {DZ_DIRECTION_LONG, DZ_DIRECTION_SHORT}) {
            const PricePlan plan = dztrader::resolve_order_price(bad, 0.5, direction, tick);
            EXPECT_FALSE(plan.valid) << "price=" << bad;
            EXPECT_EQ(plan.adjustment, PriceAdjustment::None);
        }
    }
}

/**
 * **对齐与夹取的顺序**：先按 tick 对齐，再夹涨跌停——因此同时发生时诊断报的是夹取。
 *
 * 顺序反过来的话，对齐会把已经夹到涨停价的单子再往上/往下挪一格，挂到交易所不接受的价位。
 */
TEST(OrderPriceTest, LimitClampWinsOverTickAlignment) {
    const TickSnapshot tick = make_tick(100.0, 110.0, 90.0);
    // 买价 119.7，tick 0.5：先向下对齐到 119.5，再夹到 110
    const PricePlan plan = dztrader::resolve_order_price(119.7, 0.5, DZ_DIRECTION_LONG, tick);
    ASSERT_TRUE(plan.valid);
    EXPECT_DOUBLE_EQ(plan.price, 110.0) << "the clamped price must not be re-aligned afterwards";
    EXPECT_EQ(plan.adjustment, PriceAdjustment::LimitUp);
}

// ============================================================
// 手数：差量 → 委托手数
// ============================================================

/**
 * **差量为 0 不发单**（AC「`qty_i == 0` 时跳过发单与上报」）：`order_volume` 返回 nullopt，
 * 与「差量超范围」共用一个出口，调用方只需判一个值。
 */
TEST(OrderVolumeTest, ZeroDeltaProducesNoOrder) {
    EXPECT_FALSE(dztrader::order_volume(0).has_value());
}

/// **差量幅值 → 非负手数 + 方向取符号**（方向由 `direction_of` 单独给出）
TEST(OrderVolumeTest, DeltaMagnitudeAndDirection) {
    EXPECT_EQ(dztrader::order_volume(3), Volume{3});
    EXPECT_EQ(dztrader::order_volume(-3), Volume{3}) << "magnitude is non-negative";
    EXPECT_EQ(dztrader::direction_of(3), DZ_DIRECTION_LONG);
    EXPECT_EQ(dztrader::direction_of(-3), DZ_DIRECTION_SHORT);
}

/**
 * **超出 `DzVolume` 范围拒单**（AC「非法数量拒单」）。
 *
 * 静默 `static_cast<Volume>(int64)` 会截断成完全不同的手数；拒单至少让偏差可见。
 */
TEST(OrderVolumeTest, OutOfRangeDeltaIsRejected) {
    constexpr LargeVolume kMax = std::numeric_limits<Volume>::max();
    EXPECT_EQ(dztrader::order_volume(kMax), Volume{kMax}) << "boundary is inclusive";
    EXPECT_EQ(dztrader::order_volume(-kMax), Volume{kMax});
    EXPECT_FALSE(dztrader::order_volume(kMax + 1).has_value());
    EXPECT_FALSE(dztrader::order_volume(-(kMax + 1)).has_value());
    EXPECT_FALSE(dztrader::order_volume(std::numeric_limits<LargeVolume>::max()).has_value());
    // INT64_MIN 的幅值不能被直接取负（UB），实现必须挡在取负之前
    EXPECT_FALSE(dztrader::order_volume(std::numeric_limits<LargeVolume>::min()).has_value());
}

/**
 * **差量溢出饱和而不是 UB**：符号相反的极端逻辑持仓相减必须有确定结果。
 *
 * 正常业务里绝不会出现 `±9.2e18` 手，但 `logical_position()` 会把极端 `P × scale` 饱和到
 * `LargeVolume` 上限，因此两个极端值**可以**相邻出现。有符号整数溢出是 UB，测试不能在
 * 溢出点上断言「应该等于什么」——所以这里断言的是「函数返回、并且落在两端之一」：
 * 未被 UB 优化的编译器即使算错也不至于让用例静默通过。
 */
TEST(OrderVolumeTest, SaturatedDeltaNeverOverflows) {
    constexpr LargeVolume kMax = std::numeric_limits<LargeVolume>::max();
    constexpr LargeVolume kMin = std::numeric_limits<LargeVolume>::min();

    EXPECT_EQ(dztrader::saturating_delta(5, 3), LargeVolume{2});
    EXPECT_EQ(dztrader::saturating_delta(-5, -3), LargeVolume{-2});
    EXPECT_EQ(dztrader::saturating_delta(3, -5), LargeVolume{8});

    // 正向溢出：+MAX - (-MAX) → 饱和到 MAX
    EXPECT_EQ(dztrader::saturating_delta(kMax, kMin), kMax);
    // 负向溢出：-MAX - (+MAX) → 饱和到 MIN
    EXPECT_EQ(dztrader::saturating_delta(kMin, kMax), kMin);
    // 两个极端同号相减不溢出
    EXPECT_EQ(dztrader::saturating_delta(kMax, kMax), LargeVolume{0});

    // 饱和后的差量在发单出口被拒（不会变成一张手数离谱的单）
    EXPECT_FALSE(dztrader::order_volume(dztrader::saturating_delta(kMax, kMin)).has_value());
}

/// **账户表类型就是配置层用的那个**（移动后不得出现两个不一致的定义）
TEST(OrderVolumeTest, AccountTableIsASingleType) {
    dztrader::AccountTable table{
        AttachedAccount{.account_id = "CTP001", .scale = 1.0},
        AttachedAccount{.account_id = "CTP002", .scale = 0.5},
    };
    ASSERT_EQ(table.size(), 2U);
    EXPECT_EQ(table[0].account_id, "CTP001");
    EXPECT_DOUBLE_EQ(table[1].scale, 0.5);
}

}  // namespace
