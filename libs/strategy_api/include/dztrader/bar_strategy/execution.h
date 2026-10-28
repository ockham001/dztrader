/**
 * @file execution.h
 * @brief 执行原语：最近 tick 快照、下单价格处理、手数取范围（ADR 0030，组件文档 §3/§6）
 *
 * 三件事都是**纯函数 + 纯值类型**，因此可以在不碰引擎、不搭帧通道的前提下逐格单测
 * （issue #153 的「接缝 C（纯单元）」）：
 *
 *   1. `TickSnapshot` + `snapshot_from_tick()`：最近一笔**已被 builder 接受**的 tick 的涨跌停，
 *      供价格夹取使用；
 *   2. `resolve_order_price()`：限价 → tick 对齐（买向下、卖向上，不向不利方向偷价）→
 *      涨跌停夹取（挂涨跌停价而不是拒单），并给出「调整过、原因是什么」以便 ctx 打诊断；
 *   3. `order_volume()`：账户逻辑持仓差量 → 委托手数（`DzVolume`）的范围校验。
 *
 * 为什么这些口径要抽成纯函数：价格对齐与夹取的边界（`.5` 个 tick、恰好等于涨跌停、`price_tick`
 * 缺失）是典型的「实现看起来对、边界差一格」的地方，混在 ctx 的 fanout 循环里只能用整套帧
 * 通道间接观察；抽出来之后每一格都能直接断言，ctx 只剩下「谁该发单、发多少」。
 *
 * 本头不依赖配置层（不含 JSON）：`InstrumentContext` 依赖它，而 ctx 不该为几个纯函数把内置
 * nlohmann 单头拉进每个策略 TU（ADR 0026 的隔离口径）。
 */
#ifndef DZTRADER_BAR_STRATEGY_EXECUTION_H_
#define DZTRADER_BAR_STRATEGY_EXECUTION_H_

#include <dztrader/data_type.h>
#include <dztrader/struct.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace dztrader {

/**
 * @brief 最近一笔已接受 tick 的快照（组件文档 §3；价格夹取与同步单对手价用）
 *
 * 由 A 在**每个事件入口**用本笔 tick 刷新（§5.1）：只有被 builder 接受的 tick 才会产生事件，
 * 因此被丢弃的 tick（乱序/非交易时段/量额倒退）不会刷新快照——否则一次乱序 tick 就能把涨跌停
 * 换成过期的值，后续发单会夹到错误的价格上。
 *
 * `valid == false`（默认构造）表示「还没有任何行情」，此时价格夹取被跳过。历史回放结束后
 * 清掉 `valid` 以免用历史涨跌停夹实盘单，属 #161（回放落地时接线）；v1 没有回放，因此本票
 * 范围内 `valid` 一旦被置 true 就不再变。
 */
struct TickSnapshot {
    double upper_limit = 0;  ///< 涨停价（0/非有限 = 不可用，不夹取）
    double lower_limit = 0;  ///< 跌停价（同上）
    bool valid = false;      ///< 是否有过可用的 tick（false = 价格夹取跳过）
    // 只放**当前有消费者**的字段。同步单的对手价（bid1/ask1）与回放期的 valid 清零随
    // 命令层（#162）与回放（#161）加进来——不提前留字段，避免把「以后可能要」写成接口承诺
    // （#157/#159 的同类判断）。
};

/**
 * @brief 从一个行情 tick 构造快照（A 在事件入口调用，不校验 tick 是否被 builder 接受）
 * @param tick 触发本次事件的 tick
 * @return 快照（`valid = true`）
 */
[[nodiscard]] inline TickSnapshot snapshot_from_tick(const DzTick& tick) noexcept {
    return TickSnapshot{
        .upper_limit = tick.upper_limit_price,
        .lower_limit = tick.lower_limit_price,
        .valid = true,
    };
}

/**
 * @brief 价格被调整的原因（诊断用；组件文档 §6「价格处理」）
 *
 * 顺序有意义：先按 tick 对齐，再夹涨跌停，因此同时发生时最后生效的是夹取。
 */
enum class PriceAdjustment : std::uint8_t {
    None,      ///< 无需调整（传入价已在 tick 网格上且未越界）
    Tick,      ///< 按最小变动价位对齐（买向下、卖向上）
    LimitUp,   ///< 买价高于涨停 → 挂涨停价
    LimitDown  ///< 卖价低于跌停 → 挂跌停价
};

/// @brief 调整原因的诊断名（`none` / `tick` / `limit_up` / `limit_down`）
[[nodiscard]] inline const char* price_adjustment_name(PriceAdjustment adjustment) noexcept {
    switch (adjustment) {
        case PriceAdjustment::None:
            return "none";
        case PriceAdjustment::Tick:
            return "tick";
        case PriceAdjustment::LimitUp:
            return "limit_up";
        case PriceAdjustment::LimitDown:
            return "limit_down";
    }
    return "none";
}

/**
 * @brief 一次价格处理的结果（`valid == false` 时价格非法、应拒单）
 *
 * 同时给出「调整过什么」，让 ctx 只需在 `adjustment != None` 时打一行诊断——是否调整是
 * 纯函数算出来的事实，不靠调用方再比一次浮点数。
 */
struct PricePlan {
    double price = 0;                                    ///< 最终下单价（非法时为 0）
    PriceAdjustment adjustment = PriceAdjustment::None;  ///< 调整原因
    bool valid = false;                                  ///< false = 价格非法（NaN/Inf/≤ 0）
};

namespace execution_internal {

/// @brief 把 `value` 对齐到 `step` 的整数倍网格（`upwards` = 向大取整）
///
/// 直接 `floor(value / step) * step` 会在**恰好是整数倍**的浮点表示上出错：`0.3 / 0.1` 在
/// IEEE-754 下是 `2.9999999999999996`，向下取整会掉到 `2`，把一笔本来不需要调整的买单
/// 少挂一个 tick（向不利方向偷价，正是 §6 明令禁止的）。因此先判断「离最近网格点多近算
/// 恰好」，用相对误差（量级 1e-9）吸附到最近网格点，再按方向取整。
///
/// @param value    待对齐的正数
/// @param step     网格步长（正数）
/// @param upwards  true = 向大取整，false = 向小取整
/// @return 对齐后的价格；本来就在网格上时原值返回（不重算）
[[nodiscard]] inline double align_to_step(double value, double step, bool upwards) noexcept {
    double steps = value / step;
    const double nearest = std::round(steps);
    const double tolerance = 1e-9 * (std::fabs(steps) > 1.0 ? std::fabs(steps) : 1.0);
    if (std::fabs(steps - nearest) <= tolerance) {
        // 已落在网格上（或只差浮点尾数）：**原值返回，不做 `steps * step` 重算**。重算会
        // 引入一次乘法舍入（`3 * 0.1 = 0.30000000000000004`），让一笔无需调整的价格被判成
        // 「调整过」——调用方据此打出的诊断就是假的，真正的对齐调整还会被这种噪音淹没。
        return value;
    }
    steps = upwards ? std::ceil(steps) : std::floor(steps);
    return steps * step;
}

}  // namespace execution_internal

/**
 * @brief 把策略传入的限价处理成交付下单的价格（组件文档 §6「执行」）
 *
 * 四步，顺序固定：
 *
 *   1. **合法性**：非有限或 ≤ 0 → `valid = false`（拒单；NaN 直接传进 `dz_place_order`
 *      会在 td 侧变成一张价格无意义的单）；
 *   2. **tick 对齐**：买方向（`DZ_DIRECTION_LONG`）向下、卖方向（`DZ_DIRECTION_SHORT`）
 *      向上——对齐只能让单子更容易成交，**不允许**借对齐向不利方向偷价。`price_tick <= 0`
 *      或非有限表示「合约元数据不可用」（#161 的数据源尚未落地，见组件文档 §6 实现状态），
 *      此时跳过对齐：凭空拒绝所有单子会让框架在元数据缺失时完全不能交易，而对齐不是语义
 *      正确性的前提；
 *   3. **涨停夹取**：买价高于涨停 → 挂涨停价（不拒单）；
 *   4. **跌停夹取**：卖价低于跌停 → 挂跌停价（不拒单）。
 *
 * 第 3/4 步只在方向对应的一侧生效：买单挂在跌停价上是合法（且常见）的低价挂单，不是越界。
 * 涨跌停来自 `tick`，不可用（0/非有限/`valid == false`）时跳过夹取。
 *
 * @param requested  策略传入的限价
 * @param price_tick 合约最小变动价位（`<= 0` = 元数据不可用，跳过对齐）
 * @param direction  委托方向（`DZ_DIRECTION_LONG` 买 / `DZ_DIRECTION_SHORT` 卖）
 * @param tick       最近一笔已接受 tick 的快照（涨跌停来源）
 * @return 最终价格与调整原因；`valid == false` 表示应拒单
 */
[[nodiscard]] inline PricePlan resolve_order_price(double requested,
                                                   double price_tick,
                                                   DzDirection direction,
                                                   const TickSnapshot& tick) noexcept {
    PricePlan plan;
    if (!std::isfinite(requested) || requested <= 0.0) {
        return plan;  // valid = false：非法价格拒单
    }
    plan.valid = true;
    plan.price = requested;

    const bool buying = direction == DZ_DIRECTION_LONG;
    if (price_tick > 0.0 && std::isfinite(price_tick)) {
        const double aligned = execution_internal::align_to_step(plan.price, price_tick, !buying);
        if (aligned != plan.price) {
            plan.price = aligned;
            plan.adjustment = PriceAdjustment::Tick;
        }
    }

    if (!tick.valid) {
        return plan;
    }
    if (buying && std::isfinite(tick.upper_limit) && tick.upper_limit > 0.0 &&
        plan.price > tick.upper_limit) {
        plan.price = tick.upper_limit;
        plan.adjustment = PriceAdjustment::LimitUp;
    } else if (!buying && std::isfinite(tick.lower_limit) && tick.lower_limit > 0.0 &&
               plan.price < tick.lower_limit) {
        plan.price = tick.lower_limit;
        plan.adjustment = PriceAdjustment::LimitDown;
    }
    return plan;
}

/**
 * @brief 差量 `new_l − old_l`，溢出处饱和（`qty_i` 的计算）
 *
 * 两个输入都来自 `logical_position()`，其幅值饱和在 `LargeVolume` 上限；因此**符号相反**的
 * 两个极端值直接相减会溢出（那是有符号整数的 UB）。虽然 `±9.2e18` 手这种持仓不可能出现，
 * 但 UB 不能靠「量级上不可能」免责——饱和之后由 {@link order_volume} 在上限处拒单，行为确定。
 *
 * @param new_l 新的账户逻辑持仓
 * @param old_l 旧的账户逻辑持仓
 * @return 差量（饱和到 `LargeVolume` 的表示范围）
 */
[[nodiscard]] inline LargeVolume saturating_delta(LargeVolume new_l, LargeVolume old_l) noexcept {
    constexpr LargeVolume kMax = std::numeric_limits<LargeVolume>::max();
    constexpr LargeVolume kMin = std::numeric_limits<LargeVolume>::min();
    if (old_l > 0 && new_l < kMin + old_l) {
        return kMin;
    }
    if (old_l < 0 && new_l > kMax + old_l) {
        return kMax;
    }
    return new_l - old_l;
}

/**
 * @brief 账户逻辑持仓差量 → 委托手数（组件文档 §6：`qty_i = L_i(P_new) − L_i(P_old)`）
 *
 * 返回**非负**手数（方向由差量符号单独给出），或 `std::nullopt` 表示该差量不能下单：
 * 为 0（不发单，§6 明文要求跳过）、或幅值超出 `DzVolume` 上限（拒单并诊断——把 `int64`
 * 静默截成 `int32` 会发一张手数完全不同的单，比拒单危险得多）。
 *
 * 幅值比较在 `int64` 上直接做（`±std::numeric_limits<Volume>::max()`），不做取负：`INT64_MIN`
 * 取负是 UB，而逻辑持仓的幅值已经被缩放层饱和在 `Volume` 上限以内（见 `scaling.h`），因此
 * 这里的比较既安全又完整。
 *
 * @param delta 账户逻辑持仓差量（`L_i(new) − L_i(old)`，正 = 买、负 = 卖）
 * @return 委托手数（≥ 1）或 `std::nullopt`（不发单）
 */
[[nodiscard]] inline std::optional<Volume> order_volume(LargeVolume delta) noexcept {
    constexpr LargeVolume kMaxVolume = std::numeric_limits<Volume>::max();
    if (delta > kMaxVolume || delta < -kMaxVolume) {
        return std::nullopt;
    }
    const auto magnitude = static_cast<Volume>(delta < 0 ? -delta : delta);
    if (magnitude == 0) {
        return std::nullopt;
    }
    return magnitude;
}

/// @brief 差量的委托方向（正 = 买、负 = 卖；`qty_i != 0` 由调用方保证）
[[nodiscard]] inline DzDirection direction_of(LargeVolume delta) noexcept {
    return delta > 0 ? DZ_DIRECTION_LONG : DZ_DIRECTION_SHORT;
}

/// @brief 方向名（诊断用：`long` / `short`；本框架只会下这两个方向）
[[nodiscard]] inline const char* direction_name(DzDirection direction) noexcept {
    return direction == DZ_DIRECTION_LONG ? "long" : "short";
}

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_EXECUTION_H_
