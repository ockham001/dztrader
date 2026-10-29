/**
 * @file position.h
 * @brief 策略虚拟持仓台账与开平意图语义（ADR 0030，组件文档 §3/§6/§6.1）
 *
 * B 的唯一持仓口径是**策略逻辑持仓**：四个交易意图函数（`buy` / `sell` / `sell_short` /
 * `buy_to_cover`）按传入限价**即时虚拟成交**——不等待成交回报、不跟踪在途、不建成交台账
 * （ADR 0030 决策 1）。本头只放**纯状态与纯转移**：`PositionState`（净持仓 + Tier 1）与
 * `apply_intent()`（开平规则 + 同漏斗的 Tier 1 归属计算）。它不写 stderr、不碰引擎句柄、
 * 不持账户表，因此可以独立单元测试（issue #153 的「接缝 C（纯单元）」）。
 *
 * 为什么意图语义放在这里而不是 `InstrumentContext`：四函数的开平规则是**一张表**
 * （`buy`/`sell_short` 反向时目标化为 `P = ±n`；`sell`/`buy_to_cover` 只减不增、超出钳制；
 * `volume == 0` 是模式值），而 Tier 1 更新是同一次漏斗里的开平归属计算（跨越 0 时拆成
 * 平仓段 + 开仓段）。两者放在一起，测试能对全表逐格断言，不必搭帧通道；诊断分级留在
 * ctx（它才知道品种代码），把「状态怎么变」与「怎么报」分开。
 *
 * 内部类型（`position_internal` 命名空间）不进公开承诺面：B 只经 `InstrumentContext`
 * 的四个意图方法与 Tier 1 查询接触本头的状态，不构造 `PositionState`、不调用 `apply_intent`。
 */
#ifndef DZTRADER_BAR_STRATEGY_POSITION_H_
#define DZTRADER_BAR_STRATEGY_POSITION_H_

#include <dztrader/data_type.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace dztrader {

/**
 * @brief 合约属性（组件文档 §3；`dz_db_query_instruments` 的解码子集）
 *
 * 三项都由合约元数据来源（#161 的内部依赖缝，默认实现见 `bar_strategy/dependencies.h`）填充：
 * `volume_multiple`（合约乘数，Tier 1 盈亏换算，#159）、`price_tick`（最小变动价位，下单价格
 * 对齐，#160）、`exchange_id`（交易所，仅诊断/展示）。元数据源不可用时三项保持 0/空——此时
 * 盈亏恒为 0、价格不做 tick 对齐，A 在启动诊断里显式声明该已知限制（组件文档 §6）。
 */
struct InstrumentSpec {
    std::string exchange_id;     ///< 交易所（仅诊断/展示；AUTO 下单不需要）
    double volume_multiple = 0;  ///< 合约乘数（> 0；浮动盈亏换算成钱）
    double price_tick = 0;       ///< 最小变动价位（> 0；下单价格对齐；0 = 元数据不可用）
};

/**
 * @brief 策略虚拟持仓 + Tier 1 状态（组件文档 §3/§6.1）
 *
 * 只在「交易意图 → 虚拟成交」这一个漏斗里更新（`apply_intent`），B 只读（经 ctx 的查询
 * 方法）。`market_position()` 由 `net_volume` 符号派生、**不存字段**——存一份方向会让两个
 * 字段有机会互相矛盾，而净持仓是唯一真相。
 *
 * 字段默认值即空仓状态：`net_volume == 0`、极值与均价 0、`bars_since_*` 的参考 bar 为 -1。
 */
struct PositionState {
    LargeVolume net_volume = 0;        ///< 虚拟净持仓（多正空负；B 看到的持仓）
    double avg_entry_price = 0;        ///< 当前持仓加权平均建仓价（部分平仓不变、反手重置）
    std::int32_t current_entries = 0;  ///< 当前持仓建仓段数（每次开仓/加仓 +1；无仓 0）
    LargeVolume max_contracts = 0;     ///< 本实例历史最大持仓手数（|P| 峰值，≥0，永不回落）
    std::int32_t max_entries = 0;      ///< 本实例历史最大建仓段数（≥0，永不回落）

    // 当前持仓首次建仓 / 最近加仓 / 最近平仓（价格为传入限价，时间取对应 bar 的标准结束时间）
    double entry_price = 0;
    DzDate entry_date = DZ_DATE_NA;
    DzTime entry_time = 0;
    double last_entry_price = 0;
    DzDate last_entry_date = DZ_DATE_NA;
    DzTime last_entry_time = 0;
    double exit_price = 0;
    DzDate exit_date = DZ_DATE_NA;
    DzTime exit_time = 0;
    std::int64_t entry_bar = -1;       ///< 首次建仓 bar 序号（-1 = 当前无持仓，无参考）
    std::int64_t last_entry_bar = -1;  ///< 最近加仓 bar 序号（-1 = 当前无持仓，无参考）
    std::int64_t exit_bar = -1;        ///< 最近平仓 bar 序号（-1 = 从未平仓）

    double max_position_profit = 0;  ///< 当前持仓期间浮盈极值（bar 收盘更新；开仓/平仓归零）
    double max_position_loss = 0;    ///< 当前持仓期间浮亏极值（同上，≤ 0）

    /// @brief 净持仓方向：-1 / 0 / 1（由 `net_volume` 符号派生，不存字段）
    [[nodiscard]] constexpr std::int32_t market_position() const noexcept {
        if (net_volume > 0) {
            return 1;
        }
        return net_volume < 0 ? -1 : 0;
    }
};

namespace position_internal {

/**
 * @brief 交易意图的种类（四个公开方法各对应一格，决定开平规则）
 *
 * 方向（买/卖）不足以表达语义：`buy` 与 `buy_to_cover` 同为买方向，但前者反向时目标化为
 * `+n`、后者只减不增。因此台账按**意图种类**分派，而不是先折算成方向再猜开平。
 */
enum class Intent : std::uint8_t {
    Buy,         ///< `ctx.buy`
    Sell,        ///< `ctx.sell`
    SellShort,   ///< `ctx.sell_short`
    BuyToCover,  ///< `ctx.buy_to_cover`
};

/// @brief 意图在诊断里的 wire 名（`buy` / `sell` / `sell_short` / `buy_to_cover`）
[[nodiscard]] inline std::string_view intent_name(Intent intent) noexcept {
    switch (intent) {
        case Intent::Buy:
            return "buy";
        case Intent::Sell:
            return "sell";
        case Intent::SellShort:
            return "sell_short";
        case Intent::BuyToCover:
            return "buy_to_cover";
    }
    return "unknown";
}

/**
 * @brief 一笔交易意图发生时的 bar 时间戳（Tier 1 的进出场日期/时间/bar 序号来源）
 *
 * 时间取**该 bar 的标准结束时间**（`begin_sec + period_sec`，组件文档 §6.1），不是 tick
 * 时间：同一根 bar 内多次建仓/平仓都归到同一根 bar 的结束时间，口径与 TB 的 `Date`/`Time`
 * 一致，也让 `bars_since_*` 与 bar 序号自洽。
 */
struct BarStamp {
    DzDate date = DZ_DATE_NA;     ///< bar 所属交易日
    DzTime time = 0;              ///< bar 标准结束时间的墙钟秒
    std::int64_t bar_index = -1;  ///< bar 绝对序号（-1 = 尚无 bar）
};

/**
 * @brief 一次意图的记账结果（供 ctx 翻成诊断；纯数据、不分配）
 *
 * `net_before` / `net_after` 同时给出「是否真的变了」（`applied()`）与「变成多少」，让
 * 调用方不需要再读一次状态；四个标志位与组件文档 §6「参数与诊断」的分级一一对应。
 */
struct LedgerOutcome {
    LargeVolume net_before = 0;       ///< 记账前净持仓
    LargeVolume net_after = 0;        ///< 记账后净持仓
    bool negative_volume = false;     ///< 负手数：拒单（不记账）
    bool invalid_price = false;       ///< 价格非有限或 ≤ 0：拒单（不记账）
    bool direction_invalid = false;   ///< `n > 0` 但该方向无可减持仓：无操作 + WARN
    bool clamped = false;             ///< `n > |P|`：钳制到全平 + WARN
    bool zero_volume_closed = false;  ///< `volume == 0` 实际触发平仓：INFO

    /// @brief 本次意图是否改变了逻辑持仓
    [[nodiscard]] constexpr bool applied() const noexcept { return net_after != net_before; }
};

/// @brief 净持仓绝对值（`INT64_MIN` 不可能是合法持仓，无需特判）
[[nodiscard]] constexpr LargeVolume abs_volume(LargeVolume volume) noexcept {
    return volume < 0 ? -volume : volume;
}

/**
 * @brief 一次意图的**目标**净持仓与诊断标志（`resolve_target()` 的返回类型）
 *
 * 只描述「该走到哪里」，不含任何 Tier 1 状态——均价/段数/极值怎么变由 `apply_ledger()` 决定。
 */
struct Resolution {
    LargeVolume target = 0;          ///< 目标净持仓
    bool direction_invalid = false;  ///< 该方向无可减持仓（`n > 0` 时）
    bool clamped = false;            ///< 手数超过逻辑持仓、被钳制到全平
};

/**
 * @brief 解析一次意图的目标净持仓（组件文档 §6 两张表的实现）
 *
 * 只算目标，不改状态——诊断分级需要知道「方向是否无效」「是否钳制」，而这些事实从
 * `(intent, volume, position)` 就能确定，与状态更新无关。分开之后 `apply_ledger()` 只处理
 * 「怎么从旧持仓走到新持仓」，两个关注点各自可读。
 *
 * @param intent   意图种类
 * @param volume   策略手数（`>= 0`；负数在调用方已被拒）
 * @param position 当前净持仓（多正空负）
 * @return 目标净持仓与诊断标志
 */
[[nodiscard]] inline Resolution resolve_target(Intent intent,
                                               Volume volume,
                                               LargeVolume position) noexcept {
    const LargeVolume n = static_cast<LargeVolume>(volume);
    Resolution result;
    result.target = position;

    // 四格子其实是**两对镜像**：`buy`/`sell_short` 是入口（反向时目标化 `±n`），
    // `sell`/`buy_to_cover` 是出口（只减不增、超出钳制）。每对的两格只差一个方向符号，
    // 因此把持仓换算到「本意图的方向空间」（`own = direction × position`，买方向/平多方向
    // 为正）后，规则写一次即可——两个方向各抄一遍的话，改一处漏一处不会有编译错误。
    const bool entry = intent == Intent::Buy || intent == Intent::SellShort;
    const LargeVolume direction =
        (intent == Intent::Buy || intent == Intent::Sell) ? LargeVolume{1} : LargeVolume{-1};
    const LargeVolume own = direction * position;

    if (entry) {
        if (volume == 0) {
            // 只平反向、不建新仓（MC 的 `Buy 0` / `SellShort 0` 口径）
            result.target = own < 0 ? 0 : position;
            return result;
        }
        // 反向（`own < 0`）时目标化为 `±n`：平掉对手仓再按 `n` 建仓，一步到位
        result.target = direction * (own < 0 ? n : own + n);
        return result;
    }

    if (volume == 0) {
        // 全平对应方向（TB 的 `Sell(0)` / `BuyToCover(0)` 口径）
        result.target = own > 0 ? 0 : position;
        return result;
    }
    if (own <= 0) {
        result.direction_invalid = true;  // 该方向无可减持仓：无操作 + WARN
        return result;
    }
    if (n > own) {
        result.clamped = true;  // 超出逻辑持仓：钳制到全平 + WARN
        result.target = 0;
        return result;
    }
    result.target = direction * (own - n);
    return result;
}

/**
 * @brief 把状态从 `old_net` 走到 `new_net`，并在同一漏斗里更新 Tier 1（组件文档 §6.1）
 *
 * 跨越 0 时拆成**平仓段 + 开仓段**：先按平仓段写 `exit_*`，再按开仓段重置均价/段数/极值。
 * 这是「反手」与「全平后再开」在同一次意图里能算对的原因——不拆的话反手会被当成一次
 * 巨额加仓，均价会横跨两个方向算出无意义的数。
 *
 * 四个分支与增量关系一一对应：全平（`new == 0`）、新开（跨 0 或从空仓）、加仓（同向变大）、
 * 部分平仓（同向变小）。部分平仓不动均价与段数是「加权均价」的定义决定的：卖出一部分
 * 多头并不改变剩余持仓的建仓成本。
 *
 * @param state    台账（就地更新）
 * @param old_net  记账前净持仓
 * @param new_net  记账后净持仓（调用方保证两者不同且 `new_net != 0` 或全平）
 * @param price    传入限价（虚拟成交价）
 * @param stamp    当前 bar 时间戳
 */
inline void apply_ledger(PositionState& state,
                         LargeVolume old_net,
                         LargeVolume new_net,
                         double price,
                         const BarStamp& stamp) noexcept {
    const LargeVolume old_size = abs_volume(old_net);
    const LargeVolume new_size = abs_volume(new_net);
    const bool same_sign = old_net != 0 && new_net != 0 && ((old_net > 0) == (new_net > 0));
    const LargeVolume closed =
        same_sign ? (old_size > new_size ? old_size - new_size : 0) : old_size;
    const LargeVolume opened =
        new_net == 0 ? 0 : (same_sign ? (new_size > old_size ? new_size - old_size : 0) : new_size);

    if (closed > 0) {
        state.exit_price = price;
        state.exit_date = stamp.date;
        state.exit_time = stamp.time;
        state.exit_bar = stamp.bar_index;
    }

    if (new_net == 0) {
        // 全平：均价/段数/浮盈浮亏极值归零。entry_* 的价格与时间保留最近一次建仓值
        // （与 TB 一致：无持仓时读到的仍是上一个建仓价），但 bars_since_* 用 -1 表示「无参考」。
        state.avg_entry_price = 0;
        state.current_entries = 0;
        state.entry_bar = -1;
        state.last_entry_bar = -1;
        state.max_position_profit = 0;
        state.max_position_loss = 0;
    } else if (!same_sign) {
        // 新开（从空仓或反手）：均价重置为该笔成交价，段数从 1 起，浮盈浮亏极值重置 0。
        state.avg_entry_price = price;
        state.current_entries = 1;
        state.entry_price = price;
        state.entry_date = stamp.date;
        state.entry_time = stamp.time;
        state.entry_bar = stamp.bar_index;
        state.last_entry_price = price;
        state.last_entry_date = stamp.date;
        state.last_entry_time = stamp.time;
        state.last_entry_bar = stamp.bar_index;
        state.max_position_profit = 0;
        state.max_position_loss = 0;
    } else if (opened > 0) {
        // 加仓：按新增手数加权摊薄均价，段数 +1，首次建仓的 entry_* 不动。
        const double old_weight = static_cast<double>(old_size);
        const double added_weight = static_cast<double>(opened);
        state.avg_entry_price = (state.avg_entry_price * old_weight + price * added_weight) /
                                static_cast<double>(new_size);
        ++state.current_entries;
        state.last_entry_price = price;
        state.last_entry_date = stamp.date;
        state.last_entry_time = stamp.time;
        state.last_entry_bar = stamp.bar_index;
    }
    // 其余情形 = 部分平仓：均价与段数都不变（只有 exit_* 更新，已在上方处理）。

    state.net_volume = new_net;
    if (new_size > state.max_contracts) {
        state.max_contracts = new_size;
    }
    if (state.current_entries > state.max_entries) {
        state.max_entries = state.current_entries;
    }
}

/**
 * @brief 一笔交易意图的完整记账（解析目标 → 更新台账），返回诊断依据
 *
 * 拒单（负手数 / 非法价格）时**状态完全不变**：记账的第一条不变式是「策略逻辑持仓只由
 * 合法意图推进」，任何被拒的意图都不能在 Tier 1 里留下痕迹（否则均价会被一个没成交的
 * 价格污染，且永远无法从台账里看出来）。
 *
 * `volume == 0` 且无可平仓时返回全 false 的结局（`applied()` 为 false）——这是唯一
 * **不产生任何诊断**的无操作路径，避免平仓语句逐 bar 刷屏（组件文档 §6）。
 *
 * @param state   台账（就地更新）
 * @param intent  意图种类
 * @param volume  策略手数
 * @param price   传入限价
 * @param stamp   当前 bar 时间戳
 * @return 记账结果（含诊断分级依据）
 */
[[nodiscard]] inline LedgerOutcome apply_intent(PositionState& state,
                                                Intent intent,
                                                Volume volume,
                                                double price,
                                                const BarStamp& stamp) noexcept {
    LedgerOutcome outcome;
    outcome.net_before = state.net_volume;
    outcome.net_after = state.net_volume;
    if (volume < 0) {
        outcome.negative_volume = true;
        return outcome;
    }

    // 先解析目标、**再**校验价格：`volume == 0` 且无可平仓是「无操作」，按组件文档 §6 必须
    // **不诊断**——若先校验价格，一次 `sell(0, 0.0)` 就会打出拒单诊断，违反这条明文要求
    // （B 的平仓语句通常逐 bar 无条件调用，刷屏代价最高）。无变化的意图本来就不会用到价格。
    const Resolution resolution = resolve_target(intent, volume, state.net_volume);
    outcome.direction_invalid = resolution.direction_invalid;
    outcome.clamped = resolution.clamped;
    if (resolution.target == state.net_volume) {
        return outcome;  // 无变化：无操作、不诊断
    }
    if (!std::isfinite(price) || price <= 0.0) {
        outcome.invalid_price = true;
        return outcome;
    }
    outcome.zero_volume_closed = volume == 0;
    apply_ledger(state, state.net_volume, resolution.target, price, stamp);
    outcome.net_after = state.net_volume;
    return outcome;
}

}  // namespace position_internal

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_POSITION_H_
