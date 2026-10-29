/**
 * @file dependencies.h
 * @brief BarStrategy 框架的内部依赖缝与回放原语（issue #161；**内部面，不进公开承诺面**）
 *
 * 框架在运行期要碰三件**外部数据**：历史 K 线（归档域）、交易小节/竞价窗口表（platform.db）、
 * 合约元数据（platform.db）。三者都属于「平台侧数据源」，v1 里前两者尚未落地（[ADR
 * 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md) 决策 2/3、归档域
 * #4）——把它们的获取方式写死在框架里，会让回放/竞价/元数据失败路径只能 靠真实归档库或真实
 * platform.db 验证，等于没有测试。因此这里定义**内部接缝**：
 *
 *   - 生产入口 `run_bar_strategy()` 不暴露本头，默认实现见 {@link query_archive_history}
 *     与 {@link load_archive_metadata}（分别调 `dz_db_query_bar` / `dz_db_query_instruments`）；
 *   - 测试经 `bar_strategy_internal::run_bar_strategy_with(FrameworkDependencies)` 注入
 *     fake 实现（Seam B），不需要任何真实库。
 *
 * **为什么不是公开 API**：这些类型描述的是「数据从哪来」，属框架内部实现细节。归档域落地
 * 后 schema 会变（#4 的分页/LIMIT、`is_final` 列），公开出去就是承诺不能改。契约不变式：
 * 本头只被框架自身与测试包含，B 侧只经 `InstrumentContext` 的公开面接触框架。
 *
 * 本头只依赖标准库与 SDK 公开头，不依赖 spdlog/内部库（ADR 0020/0021）。
 */
#ifndef DZTRADER_BAR_STRATEGY_DEPENDENCIES_H_
#define DZTRADER_BAR_STRATEGY_DEPENDENCIES_H_

#include <dztrader/api.h>
#include <dztrader/bar/types.h>
#include <dztrader/bar_strategy/position.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/error.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dztrader {

/**
 * @brief 每日回卷的默认墙钟时刻（18:00，距午夜毫秒；组件文档 §4.4）
 *
 * 18:00 位于日盘收盘之后、夜盘开盘之前，是天然静默窗。生产入口恒用本值；测试经
 * `bar_strategy_internal::run_bar_strategy_with` 注入一个近未来的时刻以便确定性触发
 * （内部接缝，不进公开承诺面）。
 */
inline constexpr std::int32_t DEFAULT_ROLLBACK_CLOCK_MS = 18 * 60 * 60 * 1000;

/**
 * @brief v1 内置退化交易小节表（组件文档 §5.5）
 *
 * platform.db 的交易小节/竞价窗口数据源尚未落地（ADR 0018 决策 2/3），v1 用内置退化表：
 * 单小节 `[0, 86399]`（覆盖全部交易日内秒）、无竞价窗口。**这不是新数据源、也不是配置
 * 项**（ADR 0032 决策 8 不引入占位配置），只是数据源落地前框架内置的临时表，代码里只有
 * 一处构造，落地后删除。
 *
 * 三条已知限制（策略与迁移必须知晓）：竞价 tick 按普通 tick 处理（`ctx.is_auction()` 恒
 * false）、小节末端整点 tick 会开新 bar、无交易时段过滤。
 *
 * @return 合法的退化表（`TradingSessionTable::valid()` 为 true）
 */
[[nodiscard]] inline bar::TradingSessionTable degraded_trading_session_table() noexcept {
    bar::TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = bar::TradingSession{.begin_sec = 0, .end_sec = 86399};
    table.auction_count = 0;
    return table;
}

/**
 * @brief 判定一张小节表是否为内置退化表（组件文档 §5.5）
 *
 * 退化表 = 恰好一个小节 `[0, 86399]` 且无竞价窗口。启动诊断据此声明口径，而不是靠调用方
 * 额外传一个布尔标志——标志与实际表可能不一致，那样打印出的诊断就是假的。
 *
 * @param table 待判定的表
 * @return true = 内置退化口径
 */
[[nodiscard]] inline bool is_degraded_session_table(
    const bar::TradingSessionTable& table) noexcept {
    return table.session_count == 1 && table.auction_count == 0 &&
           table.sessions[0].begin_sec == 0 && table.sessions[0].end_sec == 86399;
}

namespace bar_strategy_internal {

// ============================================================
// 历史预热回放的纯原语（组件文档 §5.3）
// ============================================================

/// @brief 每根归档 bar 拆成的合成 tick 数（4 个：开盘 / 极值 / 极值 / 收盘）
inline constexpr std::int32_t REPLAY_TICKS_PER_BAR = 4;

/// @brief 合成 tick 的单 tick 成本估算（builder 计算路径；组件文档 §5.3 的 0.1–0.4µs/回调）
inline constexpr double REPLAY_SECONDS_PER_BUILDER_TICK = 0.1e-6;

/// @brief 合成 tick 引发的单次 B 回调成本估算（组件文档 §5.3）
inline constexpr double REPLAY_SECONDS_PER_STRATEGY_CALLBACK = 0.3e-6;

/**
 * @brief 把一根归档 bar 拆成 4 个合成 tick（回放的唯一 tick 合成实现）
 *
 * 口径（组件文档 §5.3）：
 *   - **价格顺序**：涨 / 平（`close >= open`）`O→L→H→C`，跌（`close < open`）`O→H→L→C`。
 *     首笔播种 `open`，随后依次命中 `low`/`high`，末笔落在 `close`——因此 builder 产出的
 *     `open/high/low/close` 与归档 bar 逐字段一致；
 *   - **时刻**：限定在归档记录的最后一笔真实 tick 之前（`last_tick_sec` 为末点，前三个在
 *     `[begin, last_tick_sec]` 内均匀分布），既不跨桶也不撞小节末端（后者会被 builder 归到
 *     前一根）。这样第 4 个 tick 的 `last_tick_sec` 恰好等于归档值，且 forming bar 的回放
 *     不会把时刻推到未到达的桶末——否则同桶的缓冲 tick 会被 builder 当乱序丢掉，续接失效；
 *   - **量/额/持仓**：按 `k/4` 线性累计，用归档的**当日累计**字段反推：第 k 个 tick 的
 *     `cum = bar.cum - bar.value + bar.value*k/4`。这样第 4 个 tick 的累计恰好等于归档值，
 *     builder 差分出的 bar 量额与归档一致；跨交易日时归档的当日累计从 0 起算，因此也能
 *     正确还原（builder 换日时把基准重置为 0）；
 *   - **持仓量**：4 个 tick 都取归档 bar 的 `open_interest`（归档只有最后一笔的值）。
 *
 * @param bar 归档 bar（`period_sec >= 5`、`begin_sec` 是周期整数倍、`cum >= bar 内量额`；
 *            由框架的历史数据清洗保证，纯函数不再校验）
 * @return 4 个合成 tick（价格/时刻/累计量额/OI 均已填好；`instrument_id` 留空——builder
 *         不读该字段，调用方已按实例绑定合约）
 */
[[nodiscard]] inline std::array<DzTick, REPLAY_TICKS_PER_BAR> make_replay_ticks(
    const bar::Bar& bar) noexcept {
    std::array<DzTick, REPLAY_TICKS_PER_BAR> ticks{};

    // 价格顺序：上涨与平盘都先探低点再探高点；下跌先探高点。首笔必须在 H/L 之前播开，
    // 否则 open 会被后续极值覆盖（builder 的 begin_bar 用首笔价格播种 O=H=L=C）。
    std::array<double, REPLAY_TICKS_PER_BAR> prices{};
    if (bar.close < bar.open) {
        prices = {bar.open, bar.high, bar.low, bar.close};
    } else {
        prices = {bar.open, bar.low, bar.high, bar.close};
    }

    // 合成 tick 的时刻以归档的 `last_tick_sec` 为末点：forming bar 的缓冲续接 tick 必须
    // 晚于它才不会被 builder 当乱序丢掉（回放若铺到桶末，续接必然被丢）；已收 bar 的
    // last_tick_sec 通常就在桶末附近，因此与「铺满整桶」等价。
    const std::int32_t period = bar.period_sec;
    const std::int32_t span = std::clamp(bar.last_tick_sec - bar.begin_sec, 0, period - 1);
    const std::array<std::int32_t, REPLAY_TICKS_PER_BAR> offsets{
        span / 4,
        span / 2,
        span * 3 / 4,
        span,
    };

    const std::int64_t base_volume = bar.cum_volume - bar.volume;
    const double base_turnover = bar.cum_turnover - bar.turnover;
    for (std::int32_t i = 0; i < REPLAY_TICKS_PER_BAR; ++i) {
        const std::int32_t step = i + 1;
        DzTick& tick = ticks[static_cast<std::size_t>(i)];
        tick.date = bar.trading_day;
        tick.time =
            bar::TradingTime::to_clock(bar.begin_sec + offsets[static_cast<std::size_t>(i)]);
        tick.last_price = prices[static_cast<std::size_t>(i)];
        // 第 4 个 tick 的累计必须精确等于归档值：`volume * 4 / 4` 是整数运算，无舍入。
        tick.volume = static_cast<DzVolume>(base_volume + bar.volume * step / 4);
        tick.turnover = base_turnover + bar.turnover * static_cast<double>(step) / 4.0;
        tick.open_interest = bar.open_interest;
    }
    return ticks;
}

/**
 * @brief 回放启动成本估算（组件文档 §5.3：> 60s 与 > 300s 两级 WARN 的判据）
 *
 * 估算口径与文档一致：`builder_ticks × 0.1µs + strategy_callbacks × 0.3µs`。真值随策略
 * 复杂度变化（B 的代码不在框架估算范围内），这里的用途是「启动前让运维知道大概量级」，
 * 因此宁可偏保守（把每根回放 bar 的 6 次回调都算上）也不做精确建模。
 *
 * @param builder_ticks        合成 tick 总数（= 历史根数 × 4）
 * @param strategy_callbacks   B 回调总数（= 回放根数 × 6；静默填窗根数不回调 B）
 * @return 预计秒数（双精度；异常大的输入会饱和在 double 范围内，不需要额外守卫）
 */
[[nodiscard]] inline double estimate_replay_seconds(std::int64_t builder_ticks,
                                                    std::int64_t strategy_callbacks) noexcept {
    return static_cast<double>(builder_ticks) * REPLAY_SECONDS_PER_BUILDER_TICK +
           static_cast<double>(strategy_callbacks) * REPLAY_SECONDS_PER_STRATEGY_CALLBACK;
}

// ============================================================
// 接缝 1：历史 bar 来源（默认 dz_db_query_bar 解码；归档域未实现时返回 Unavailable）
// ============================================================

/**
 * @brief 一次历史 bar 查询（A 每品种查一次；`warmup` 二选一已由配置层解析）
 *
 * `warmup_bars` 与 `warmup_from` 恰有一个有值（`EffectiveConfig` 的保证）。查询深度按预热
 * 配置裁剪：根数模式取最近 N 根，起始日期模式取该日期之后的全部。
 */
struct HistoryQuery {
    std::string_view instrument_id;           ///< 合约代码（平台唯一键）
    std::int32_t period_sec = 60;             ///< bar 周期（来自配置）
    std::int32_t max_lookback = 0;            ///< 可读深度（仅用于诊断/回推起始日期）
    std::optional<std::int32_t> warmup_bars;  ///< 预热根数（与 warmup_from 二选一）
    std::optional<DzDate> warmup_from;        ///< 预热起始交易日（含；距纪元天数）
};

/// @brief 历史加载结局：三者必须区分（见 {@link HistoryQueryResult}）
enum class HistoryStatus : std::uint8_t {
    Loaded,       ///< 拿到历史 bar（可能为空列表 = 该区间无成交）
    Unavailable,  ///< 数据源不可用/未实现 → 冷启动降级（WARN，不阻断启动）
    Failed,       ///< 数据源可用但查询失败 → 同样降级（WARN），带错误消息
};

/// @brief 历史加载结果（`bars` 升序：最早一根在前，即该品种的锚点）
struct HistoryQueryResult {
    HistoryStatus status = HistoryStatus::Unavailable;
    std::vector<bar::Bar> bars;  ///< 归档 bar（升序；`Loaded` 时有效）
    std::string message;         ///< 诊断消息（`Unavailable` / `Failed` 时非空）
};

/// @brief 历史 bar 来源（内部接缝；空 = 使用 {@link query_archive_history}）
using HistoryBarLoader = std::function<HistoryQueryResult(const HistoryQuery&)>;

/**
 * @brief 默认历史 bar 来源：`dz_db_query_bar` + 列名解码（归档域 #4 落地前恒 Unavailable）
 *
 * 归档域（tick/bar 历史存储与查询）尚未落地：`dz_db_query_bar` 恒返回 NULL 且
 * `dz_errcode()` 为 `DZ_EC_NOT_IMPLEMENTED`（[ADR
 * 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md) 决策 1 之后保留的声明、[ADR
 * 0019](../adr/0019-strategy-sdk-db-backend-scope.md) 的未实现语义）。 因此**v1 的默认实现必然走到
 * Unavailable**：历史是增强项，按「无历史」降级冷启动，不是错误。
 *
 * 查询成功时的解码口径（**暂定契约，随归档域 spec #4 定稿**）：结果集列名 = `bar::Bar` 的
 * 字段名（`trading_day` / `begin_sec` / `last_tick_sec` / `period_sec` / `open` / `high` /
 * `low` / `close` / `volume` / `turnover` / `open_interest` / `cum_volume` / `cum_turnover` /
 * `is_final`；字段语义由 [ADR 0031](../adr/0031-tick-bar-synthesis-component.md) 决策 9 冻结）。
 * `last_tick_sec` 缺失时按桶内最后一秒补；`is_final` 缺失时按「最后一根视为 forming」处理
 * 并在 `message` 里声明（组件文档 §5.3）。必需列缺失 = Failed（schema 与解码不匹配，属于
 * 归档域落地时必须同步修的接缝）。
 *
 * @param db    已打开的库句柄（nullptr = 库不可用 → Unavailable）
 * @param query 查询参数
 * @return 加载结果（`bars` 已按 `warmup_bars` 裁剪为最近 N 根）
 */
[[nodiscard]] inline HistoryQueryResult query_archive_history(DzDatabase* db,
                                                              const HistoryQuery& query);

// ============================================================
// 接缝 2：合约元数据来源（默认 dz_db_query_instruments 解码）
// ============================================================

/// @brief 合约元数据加载结局（四者必须区分，见 {@link MetadataResult}）
enum class MetadataStatus : std::uint8_t {
    Loaded,             ///< 必需字段齐全 → 实例按元数据执行
    Missing,            ///< 数据源可用但查不到该合约 → 该实例创建失败
    Invalid,            ///< 行存在但必需字段缺失/非法 → 该实例创建失败
    SourceUnavailable,  ///< 数据源不可用 → 降级（InstrumentSpec 全 0 + 已知限制诊断）
};

/// @brief 合约元数据结果（`spec` 仅在 `Loaded` 时有意义）
struct MetadataResult {
    MetadataStatus status = MetadataStatus::SourceUnavailable;
    InstrumentSpec spec;
    std::string message;  ///< 诊断消息（非 `Loaded` 时非空）
};

/// @brief 合约元数据来源（内部接缝；空 = 使用 {@link load_archive_metadata}）
using InstrumentMetadataLoader = std::function<MetadataResult(std::string_view)>;

/**
 * @brief 默认合约元数据来源：`dz_db_query_instruments` 解码 volume_multiple/price_tick/exchange_id
 *
 * 必需字段（组件文档 §3/§8）：`volume_multiple > 0`、`price_tick > 0`、`exchange_id` 非空。
 * 缺任一 → `Invalid`，A 据此把该实例判为创建失败（进程与其它品种继续）——静默用 0 乘数会让
 * 浮动盈亏恒为 0、止损永不触发；静默跳过 tick 对齐会让挂单价与交易所口径不一致。
 *
 * 库不可达（v1 无 platform.db 的常态）→ `SourceUnavailable`：这是**部署级**降级，A 打印
 * 已知限制诊断后按零元数据继续（先纸上跑通策略）。与 `Missing` 区分开，避免「没装平台库」
 * 被误判成「合约配错了」。
 *
 * @param db            已打开的库句柄（nullptr = 库不可用 → SourceUnavailable）
 * @param instrument_id 合约代码
 * @return 元数据结果
 */
[[nodiscard]] inline MetadataResult load_archive_metadata(DzDatabase* db,
                                                          std::string_view instrument_id);

// ============================================================
// 三个内部依赖缝的聚合（A 的构造参数；生产入口用默认实现）
// ============================================================

/**
 * @brief BarStrategy 框架的内部依赖（测试注入点；不进公开承诺面）
 *
 *   - `session_table`：交易小节/竞价窗口表（v1 = 内置退化表，platform.db 数据源随
 *     [ADR 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md) 决策 2/3 落地）；
 *   - `history` / `metadata`：空 = 默认归档实现（见上）；测试注入 fake 后不需要真实归档库
 *     或 platform.db 就能验证回放、冷启动降级、元数据失败路径（Seam B）；
 *   - `rollback_clock_ms`：18:00 回卷定时器的墙钟时刻（测试换成近未来时刻以确定性触发）。
 */
struct FrameworkDependencies {
    bar::TradingSessionTable session_table = degraded_trading_session_table();
    HistoryBarLoader history;           ///< 空 = {@link query_archive_history}
    InstrumentMetadataLoader metadata;  ///< 空 = {@link load_archive_metadata}
    std::int32_t rollback_clock_ms = DEFAULT_ROLLBACK_CLOCK_MS;
};

// ============================================================
// 默认实现的内部辅助
// ============================================================

namespace dependencies_internal {

/// @brief 默认历史查询在 `bars` 模式下回推起始日期时多给的天数（吸收周末/节假日）
inline constexpr std::int32_t HISTORY_QUERY_BACKOFF_MARGIN_DAYS = 5;

/// @brief 结果集关闭守卫（`DzResultSet` 是 C 句柄，异常路径也必须关）
class ResultSetGuard {
public:
    explicit ResultSetGuard(DzResultSet* results) noexcept
        : results_(results) {}
    ~ResultSetGuard() {
        if (results_ != nullptr) {
            dz_resultset_close(results_);
        }
    }
    ResultSetGuard(const ResultSetGuard&) = delete;
    ResultSetGuard& operator=(const ResultSetGuard&) = delete;
    ResultSetGuard(ResultSetGuard&&) = delete;
    ResultSetGuard& operator=(ResultSetGuard&&) = delete;

private:
    DzResultSet* results_ = nullptr;
};

/// @brief 按列名查列索引（不存在返回 nullopt；不依赖结果集的列序）
[[nodiscard]] inline std::optional<std::uint32_t> find_column(DzResultSet* results,
                                                              std::string_view name) {
    const std::uint32_t count = dz_resultset_column_count(results);
    for (std::uint32_t index = 0; index < count; ++index) {
        const char* column = dz_resultset_column_name(results, index);
        if (column != nullptr && std::string_view(column) == name) {
            return index;
        }
    }
    return std::nullopt;
}

/// @brief 读一列为 double（INT64 列自动宽化；类型不符时返回 0，由调用方按字段校验拒掉）
[[nodiscard]] inline double column_double(DzResultSet* results, std::uint32_t index) {
    if (dz_resultset_column_type(results, index) == DZ_COL_TYPE_INT64) {
        return static_cast<double>(dz_resultset_get_int64(results, index));
    }
    return dz_resultset_get_float64(results, index);
}

/// @brief 读一列为 int64（FLOAT64 列自动收窄，用于归档 bar 的整数字段）
[[nodiscard]] inline std::int64_t column_int64(DzResultSet* results, std::uint32_t index) {
    if (dz_resultset_column_type(results, index) == DZ_COL_TYPE_FLOAT64) {
        return static_cast<std::int64_t>(dz_resultset_get_float64(results, index));
    }
    return dz_resultset_get_int64(results, index);
}

/// @brief 读一列为 string（NULL/缺列返回空串）
[[nodiscard]] inline std::string column_string(DzResultSet* results, std::uint32_t index) {
    const char* value = dz_resultset_get_string(results, index);
    return value == nullptr ? std::string{} : std::string{value};
}

/// @brief 解码一行归档 bar 所需的列索引（首次遍历前解析一次）
struct BarColumns {
    std::uint32_t trading_day = 0;
    std::uint32_t begin_sec = 0;
    std::uint32_t open = 0;
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    std::uint32_t close = 0;
    std::uint32_t volume = 0;
    std::uint32_t turnover = 0;
    std::uint32_t open_interest = 0;
    std::uint32_t cum_volume = 0;
    std::uint32_t cum_turnover = 0;
    std::optional<std::uint32_t> last_tick_sec;  ///< 缺失时按桶内最后一秒补
    std::optional<std::uint32_t> period_sec;     ///< 缺失时用查询周期
    std::optional<std::uint32_t> is_final;       ///< 缺失时按「最后一根 forming」补
};

/// @brief 解析必需列；缺任一返回 nullopt（调用方转 Failed + schema 诊断）
[[nodiscard]] inline std::optional<BarColumns> resolve_bar_columns(DzResultSet* results) {
    BarColumns columns;
    struct Required {
        std::string_view name;
        std::uint32_t* slot;
    };
    const Required required[] = {
        {"trading_day", &columns.trading_day},
        {"begin_sec", &columns.begin_sec},
        {"open", &columns.open},
        {"high", &columns.high},
        {"low", &columns.low},
        {"close", &columns.close},
        {"volume", &columns.volume},
        {"turnover", &columns.turnover},
        {"open_interest", &columns.open_interest},
        {"cum_volume", &columns.cum_volume},
        {"cum_turnover", &columns.cum_turnover},
    };
    for (const Required& column : required) {
        const auto index = find_column(results, column.name);
        if (!index.has_value()) {
            return std::nullopt;
        }
        *column.slot = *index;
    }
    columns.last_tick_sec = find_column(results, "last_tick_sec");
    columns.period_sec = find_column(results, "period_sec");
    columns.is_final = find_column(results, "is_final");
    return columns;
}

}  // namespace dependencies_internal

/**
 * @brief 默认合约元数据来源实现（见 {@link load_archive_metadata} 的契约说明）
 */
[[nodiscard]] inline MetadataResult load_archive_metadata(DzDatabase* db,
                                                          std::string_view instrument_id) {
    MetadataResult result;
    if (db == nullptr) {
        result.status = MetadataStatus::SourceUnavailable;
        result.message = "database unavailable | source=platform_db";
        return result;
    }
    const std::string id(instrument_id);
    DzResultSet* results = dz_db_query_instruments(
        db, id.c_str(), "instrument_id,exchange_id,volume_multiple,price_tick");
    if (results == nullptr) {
        result.status = MetadataStatus::SourceUnavailable;
        result.message = dz_errmsg();
        return result;
    }
    const dependencies_internal::ResultSetGuard guard(results);
    if (dz_resultset_status(results) != 0) {
        result.status = MetadataStatus::SourceUnavailable;
        result.message = dz_errmsg();
        return result;
    }
    if (!dz_resultset_next(results)) {
        result.status = MetadataStatus::Missing;
        result.message = "instrument metadata not found | instrument=" + id;
        return result;
    }
    const auto exchange_column = dependencies_internal::find_column(results, "exchange_id");
    const auto multiple_column = dependencies_internal::find_column(results, "volume_multiple");
    const auto tick_column = dependencies_internal::find_column(results, "price_tick");
    if (!exchange_column.has_value() || !multiple_column.has_value() || !tick_column.has_value()) {
        result.status = MetadataStatus::Invalid;
        result.message = "instrument metadata columns missing | instrument=" + id;
        return result;
    }
    result.spec.exchange_id = dependencies_internal::column_string(results, *exchange_column);
    result.spec.volume_multiple = dependencies_internal::column_double(results, *multiple_column);
    result.spec.price_tick = dependencies_internal::column_double(results, *tick_column);

    if (result.spec.exchange_id.empty()) {
        result.status = MetadataStatus::Invalid;
        result.message =
            "instrument metadata invalid | instrument=" + id + " field=exchange_id reason=empty";
        return result;
    }
    if (!(result.spec.volume_multiple > 0.0) || !std::isfinite(result.spec.volume_multiple)) {
        result.status = MetadataStatus::Invalid;
        result.message = "instrument metadata invalid | instrument=" + id +
                         " field=volume_multiple reason=not_positive";
        return result;
    }
    if (!(result.spec.price_tick > 0.0) || !std::isfinite(result.spec.price_tick)) {
        result.status = MetadataStatus::Invalid;
        result.message = "instrument metadata invalid | instrument=" + id +
                         " field=price_tick reason=not_positive";
        return result;
    }
    result.status = MetadataStatus::Loaded;
    return result;
}

/**
 * @brief 默认历史 bar 来源实现（见 {@link query_archive_history} 的契约说明）
 */
[[nodiscard]] inline HistoryQueryResult query_archive_history(DzDatabase* db,
                                                              const HistoryQuery& query) {
    HistoryQueryResult result;
    if (db == nullptr) {
        result.status = HistoryStatus::Unavailable;
        result.message = "database unavailable | source=archive";
        return result;
    }
    // 起始日期：`warmup_from` 直接用；`bars` 模式按每日 bar 数下限回推（多给几天余量吸收
    // 周末/节假日，查询后再裁剪到最近 N 根）；两者都没有时按 0 = 不限定（理论不可达）。
    DzDate start_date = 0;
    if (query.warmup_from.has_value()) {
        start_date = *query.warmup_from;
    } else if (query.warmup_bars.value_or(0) > 0) {
        const std::int64_t seconds =
            static_cast<std::int64_t>(*query.warmup_bars) * query.period_sec;
        const std::int64_t days =
            (seconds + 86399) / 86400 + dependencies_internal::HISTORY_QUERY_BACKOFF_MARGIN_DAYS;
        const Date today = DateTime::local_now().date();
        start_date =
            static_cast<DzDate>(today.days_since_epoch() - static_cast<std::int32_t>(days));
    }
    const std::string id(query.instrument_id);
    DzResultSet* results =
        dz_db_query_bar(db, id.c_str(), query.period_sec, DZ_ADJUST_NONE, start_date, 0);
    if (results == nullptr) {
        const std::int32_t code = dz_errcode();
        if (code == DZ_EC_NOT_IMPLEMENTED) {
            // 归档域未实现是 v1 的**正常状态**：按「无历史」降级，调用方打 WARN 后冷启动。
            result.status = HistoryStatus::Unavailable;
            result.message = std::string(dz_errmsg()) + " status=not_implemented";
        } else {
            result.status = HistoryStatus::Failed;
            result.message = dz_errmsg();
        }
        return result;
    }
    const dependencies_internal::ResultSetGuard guard(results);
    if (dz_resultset_status(results) != 0) {
        result.status = HistoryStatus::Failed;
        result.message = dz_errmsg();
        return result;
    }
    const auto columns = dependencies_internal::resolve_bar_columns(results);
    if (!columns.has_value()) {
        // schema 与解码不匹配：归档域落地时必须与契约同步修这里（不是「无历史」）
        result.status = HistoryStatus::Failed;
        result.message =
            "archive bar schema mismatch | instrument=" + id + " reason=required_columns_absent";
        return result;
    }
    if (!columns->is_final.has_value()) {
        result.message = "archive bar is_final column absent | assumed_last_bar_forming";
    }
    while (dz_resultset_next(results)) {
        bar::Bar bar{};
        bar.trading_day =
            static_cast<DzDate>(dependencies_internal::column_int64(results, columns->trading_day));
        bar.begin_sec = static_cast<std::int32_t>(
            dependencies_internal::column_int64(results, columns->begin_sec));
        bar.period_sec = columns->period_sec.has_value()
                             ? static_cast<std::int32_t>(dependencies_internal::column_int64(
                                   results, *columns->period_sec))
                             : query.period_sec;
        bar.last_tick_sec = columns->last_tick_sec.has_value()
                                ? static_cast<std::int32_t>(dependencies_internal::column_int64(
                                      results, *columns->last_tick_sec))
                                : bar.begin_sec + bar.period_sec - 1;
        bar.open = dependencies_internal::column_double(results, columns->open);
        bar.high = dependencies_internal::column_double(results, columns->high);
        bar.low = dependencies_internal::column_double(results, columns->low);
        bar.close = dependencies_internal::column_double(results, columns->close);
        bar.volume = dependencies_internal::column_int64(results, columns->volume);
        bar.turnover = dependencies_internal::column_double(results, columns->turnover);
        bar.open_interest = dependencies_internal::column_int64(results, columns->open_interest);
        bar.cum_volume = dependencies_internal::column_int64(results, columns->cum_volume);
        bar.cum_turnover = dependencies_internal::column_double(results, columns->cum_turnover);
        bar.is_final = columns->is_final.has_value()
                           ? dependencies_internal::column_int64(results, *columns->is_final) != 0
                           : true;
        result.bars.push_back(bar);
    }
    if (!columns->is_final.has_value() && !result.bars.empty()) {
        // 缺失 is_final 的约定：最后一根视为 forming（组件文档 §5.3），其余按已收
        result.bars.back().is_final = false;
    }
    result.status = HistoryStatus::Loaded;
    // 裁剪为最近 N 根（查询层未按 LIMIT 时在这里兜底；幂等）
    if (query.warmup_bars.has_value() &&
        result.bars.size() > static_cast<std::size_t>(*query.warmup_bars)) {
        const auto excess = static_cast<std::ptrdiff_t>(
            result.bars.size() - static_cast<std::size_t>(*query.warmup_bars));
        result.bars.erase(result.bars.begin(), result.bars.begin() + excess);
    }
    return result;
}

}  // namespace bar_strategy_internal

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_DEPENDENCIES_H_
