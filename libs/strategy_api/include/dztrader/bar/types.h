/**
 * @file types.h
 * @brief tick→bar 合成组件的公开数据模型（ADR 0031 决策 9）
 *
 * 本头只包含标准库与 SDK 公开 POD 头，可被 SDK 消费者独立包含。
 * 所有类型均为定长值类型（无指针、无容器、无堆分配）：tick→bar 热路径不使用
 * 哈希与字符串。
 */
#ifndef DZTRADER_BAR_TYPES_H_
#define DZTRADER_BAR_TYPES_H_

#include <dztrader/data_type.h>

#include <cstdint>
#include <type_traits>

namespace dztrader::bar {

/// @brief 交易日内秒上限（23:59:59；即第 86400 秒之前一刻，与墙钟秒 0..86399 同域）
inline constexpr std::int32_t MAX_TRADING_SEC = 86399;

/**
 * @brief 交易日内时间坐标（tsec）与墙钟秒的换算
 *
 * 交易日切换点为 18:00：tsec = (墙钟秒 + 21600) % 86400，0..86399。
 * 同一交易日内 tsec 严格单调，夜盘（21:00 起）归属下一交易日。
 * 调用方保证输入在有效区间内（墙钟秒 0..86399、tsec 0..86399），
 * 这是纯换算函数，不做区间校验。
 */
struct TradingTime {
    /// 交易日起点（18:00）的墙钟秒数
    static constexpr int32_t ANCHOR_CLOCK_SEC = 64800;

    /**
     * @brief 墙钟秒 → 交易日内秒
     * @param clock 距午夜秒数（0..86399）
     * @return 交易日内秒（0..86399）
     */
    [[nodiscard]] static constexpr int32_t from_clock(DzTime clock) noexcept {
        constexpr int32_t SECS_PER_DAY = 86400;
        return (clock + SECS_PER_DAY - ANCHOR_CLOCK_SEC) % SECS_PER_DAY;
    }

    /**
     * @brief 交易日内秒 → 墙钟秒
     * @param tsec 交易日内秒（0..86399）
     * @return 距午夜秒数（0..86399）
     */
    [[nodiscard]] static constexpr DzTime to_clock(int32_t tsec) noexcept {
        constexpr int32_t SECS_PER_DAY = 86400;
        return (tsec + ANCHOR_CLOCK_SEC) % SECS_PER_DAY;
    }
};

/**
 * @brief 一根 K 线（定长 POD，trivially copyable）
 *
 * 标识 = (trading_day, begin_sec)；日线 period_sec = 0、begin_sec = 0（18:00 标记）。
 * 形成中的 bar 由 builder 通过 current_bar() 给出，is_final 区分已收/形成中。
 * last_tick_sec 记录最后接收 tick 的真实交易日内秒：竞价首根可早于 begin_sec，
 * 小节末端 tick 可等于 standard_end_sec()。
 * @note 字段名与语义在 ADR 0031 决策 9 定稿，后续不改。
 */
struct Bar {
    DzDate trading_day;           ///< 交易日（距纪元天数，夜盘归属下一交易日）
    int32_t begin_sec;            ///< 标准开始时间（交易日内秒，18:00 锚点整数网格桶起点）
    int32_t last_tick_sec;        ///< 最后接收 tick 的交易日内秒（竞价首根可能早于 begin_sec）
    int32_t period_sec;           ///< 周期秒数；日线为 0
    double open;                  ///< 开盘价（日内容为 bar 首笔 last_price；日线为当日累计开盘价）
    double high;                  ///< 最高价（日内容为 last_price 最大值；日线为当日累计最高价）
    double low;                   ///< 最低价（日内容为 last_price 最小值；日线为当日累计最低价）
    double close;                 ///< 收盘价（bar 最新 last_price）
    int64_t volume;               ///< 成交量（bar 起始累计基准之后的差分）
    double turnover;              ///< 成交额（bar 起始累计基准之后的差分）
    DzLargeVolume open_interest;  ///< 持仓量（bar 最后一笔 tick）
    int64_t cum_volume;           ///< 最后一笔 tick 的当日累计成交量（差分与续接基准）
    double cum_turnover;          ///< 最后一笔 tick 的当日累计成交额（差分与续接基准）
    bool is_final;                ///< true = 已收；false = 形成中

    /// @brief 标准结束时间（仅日内容周期有意义；日线 period_sec = 0 时返回 0）
    [[nodiscard]] constexpr int32_t standard_end_sec() const noexcept {
        return begin_sec + period_sec;
    }

    /// @brief 标准开始时间的墙钟秒（距午夜秒数）
    [[nodiscard]] constexpr DzTime begin_clock() const noexcept {
        return TradingTime::to_clock(begin_sec);
    }

    /// @brief 最后 tick 时间的墙钟秒（距午夜秒数）
    [[nodiscard]] constexpr DzTime last_tick_clock() const noexcept {
        return TradingTime::to_clock(last_tick_sec);
    }
};

static_assert(std::is_trivially_copyable_v<Bar>, "Bar must be trivially copyable");
static_assert(std::is_standard_layout_v<Bar>, "Bar must be standard layout");

/**
 * @brief 交易小节（交易日内秒坐标，端点含）
 */
struct TradingSession {
    int32_t begin_sec;  ///< 小节开始（交易日内秒）
    int32_t end_sec;    ///< 小节结束（交易日内秒，端点含）
};

/**
 * @brief 集合竞价窗口类型
 */
enum class AuctionKind : int32_t {
    Open,   ///< 开盘竞价：tick 归后一小节首桶，窗口内正常发事件（is_auction=true）
    Close,  ///< 收盘竞价：按小节内普通 tick 处理
};

/**
 * @brief 集合竞价窗口（交易日内秒坐标，端点含）
 *
 * 窗口长度由数据描述，不写死一分钟；一个交易日可有多个（容量见
 * TradingSessionTable::MAX_AUCTIONS）。
 */
struct AuctionWindow {
    AuctionKind kind;       ///< 竞价窗口类型
    int32_t session_index;  ///< 归属交易小节在 TradingSessionTable::sessions 中的下标
    int32_t begin_sec;      ///< 窗口开始（交易日内秒）
    int32_t end_sec;        ///< 窗口结束（交易日内秒，端点含）
};

/**
 * @brief 交易小节与集合竞价窗口表（定长 POD）
 *
 * 按 (exchange_id, product_code) 一张；交易所默认 + 品种覆盖。构造 tick→bar
 * 组件的调用方负责加载并传入本表，组件只接收 POD、不做 IO/解析。
 */
struct TradingSessionTable {
    static constexpr int32_t MAX_SESSIONS = 8;  ///< 交易小节容量
    static constexpr int32_t MAX_AUCTIONS = 8;  ///< 竞价窗口容量

    int32_t session_count = 0;                   ///< 有效交易小节数（1..MAX_SESSIONS）
    int32_t auction_count = 0;                   ///< 有效竞价窗口数（0..MAX_AUCTIONS）
    TradingSession sessions[MAX_SESSIONS] = {};  ///< 交易小节（按时间升序、互不重叠）
    AuctionWindow auctions[MAX_AUCTIONS] = {};   ///< 竞价窗口（按时间升序、互不重叠）

    /**
     * @brief 校验表内容是否合法
     *
     * 要求：session_count ∈ [1, MAX_SESSIONS]；session 按时间升序、互不重叠、
     * 0 ≤ begin < end ≤ 86399；auction_count ∈ [0, MAX_AUCTIONS]；auction 按时间
     * 升序、互不重叠、session_index 有效、窗口端点合法、kind 是 Open/Close；
     * Open 窗口完全位于目标 session 之前（end < session.begin_sec）；Close 窗口
     * 位于目标 session 内。
     *
     * @return true = 表合法；false = 表非法，调用方应 fail-fast
     */
    [[nodiscard]] constexpr bool valid() const noexcept {
        constexpr int32_t MAX_TSEC = MAX_TRADING_SEC;  // 17:59:59，交易日终点前 1 秒
        if (session_count < 1 || session_count > MAX_SESSIONS) {
            return false;
        }
        if (auction_count < 0 || auction_count > MAX_AUCTIONS) {
            return false;
        }
        for (int32_t i = 0; i < session_count; ++i) {
            const TradingSession& session = sessions[i];
            if (session.begin_sec < 0 || session.begin_sec >= session.end_sec ||
                session.end_sec > MAX_TSEC) {
                return false;
            }
            if (i > 0 && session.begin_sec <= sessions[i - 1].end_sec) {
                return false;
            }
        }
        for (int32_t i = 0; i < auction_count; ++i) {
            const AuctionWindow& auction = auctions[i];
            if (auction.begin_sec < 0 || auction.begin_sec > auction.end_sec ||
                auction.end_sec > MAX_TSEC) {
                return false;
            }
            if (auction.session_index < 0 || auction.session_index >= session_count) {
                return false;
            }
            if (i > 0 && auction.begin_sec <= auctions[i - 1].end_sec) {
                return false;
            }
            const TradingSession& session = sessions[auction.session_index];
            if (auction.kind == AuctionKind::Open) {
                if (auction.end_sec >= session.begin_sec) {
                    return false;
                }
            } else if (auction.kind == AuctionKind::Close) {
                if (auction.begin_sec < session.begin_sec || auction.end_sec > session.end_sec) {
                    return false;
                }
            } else {
                // 枚举域校验：加载器把非法整数强转成 AuctionKind 时在此拒绝
                return false;
            }
        }
        return true;
    }
};

static_assert(std::is_trivially_copyable_v<TradingSessionTable>,
              "TradingSessionTable must be trivially copyable");

/**
 * @brief 一笔 tick 的目标 bar 桶（bar 边界分桶判定的结论）
 *
 * 由 {@link resolve_tick_bucket} 唯一产出；`in_session == false` 表示该 tick 不属于任何
 * 交易小节/Open 竞价窗口（应丢弃并计入 `out_of_session`）。
 */
struct TickBucket {
    bool in_session;    ///< true = 命中交易小节或 Open 竞价窗口
    int32_t begin_sec;  ///< 目标 bar 桶起点（交易日内秒；`in_session == false` 时无意义）
    bool open_auction;  ///< true = 命中 Open 竞价窗口（归后一小节首桶、事件相位为 true）
};

/**
 * @brief 交易日内秒 → 目标 bar 桶（tick→bar 分桶判定的**唯一实现**）
 *
 * `IntradayBarBuilder` 的边界判定与 BarStrategy 框架的「缓冲 tick 续接」过滤都用本函数。
 * 两处若各写一份「tick 属于哪根 bar」，过滤会按一套规则丢、builder 按另一套收，续接静默
 * 漂移。判定口径与 [ADR 0031](../adr/0031-tick-bar-synthesis-component.md) 一致：
 *
 *   1. **Open 竞价窗口命中优先于小节判定**（[ADR
 * 0032](../adr/0032-auction-window-strategy-events.md)）： tick
 * 归后一小节首桶（`floor(session.begin_sec / period_sec) * period_sec`）； Close
 * 竞价窗口不特殊处理，落到小节判定；
 *   2. **小节末端 tick 归前一根**：`tsec == session.end_sec` 时分桶用 `end_sec - 1`；
 *   3. 不在任何小节/Open 窗口内 → `in_session == false`。
 *
 * @param tsec       交易日内秒（0..86399，由 `TradingTime::from_clock` 得到）
 * @param table      交易小节与竞价窗口表（须 `valid()`）
 * @param period_sec bar 周期秒数（正数；桶起点 = 18:00 锚点整数网格）
 * @return 目标桶（`in_session == false` 时表示应丢弃）
 */
[[nodiscard]] constexpr TickBucket resolve_tick_bucket(int32_t tsec,
                                                       const TradingSessionTable& table,
                                                       int32_t period_sec) noexcept {
    // 1. Open 竞价窗口命中优先于小节判定；窗口按时间升序，越过即停。
    for (int32_t i = 0; i < table.auction_count; ++i) {
        const AuctionWindow& window = table.auctions[i];
        if (tsec < window.begin_sec) {
            break;
        }
        if (tsec <= window.end_sec) {
            if (window.kind != AuctionKind::Open) {
                break;  // Close 窗口按普通小节内 tick 处理（ADR 0031 决策 5）
            }
            const TradingSession& session = table.sessions[window.session_index];
            return TickBucket{true, (session.begin_sec / period_sec) * period_sec, true};
        }
    }
    // 2. 交易小节：小节末端 tick 归前一根（端点含）。
    for (int32_t i = 0; i < table.session_count; ++i) {
        const TradingSession& session = table.sessions[i];
        if (tsec < session.begin_sec) {
            break;  // 小节按时间升序，后续小节开始更晚
        }
        if (tsec <= session.end_sec) {
            const int32_t bucket_tsec = (tsec == session.end_sec) ? tsec - 1 : tsec;
            return TickBucket{true, (bucket_tsec / period_sec) * period_sec, false};
        }
    }
    return TickBucket{false, 0, false};
}

/**
 * @brief tick→bar 合成的丢弃计数（构造后从 0 累计）
 */
struct BarBuilderStats {
    uint64_t out_of_session = 0;  ///< 不在任何交易小节/Open 竞价窗口内（含 time 越界）的 tick 数
    uint64_t out_of_order = 0;    ///< 交易日非正/倒退、交易日内秒倒退的 tick 数
    uint64_t bad_volume = 0;      ///< 同交易日内累计量/额倒退（含 NaN/Inf）的 tick 数
};

/**
 * @brief Sink 契约（三个可选 bar 回调）与空实现
 *
 * builder 的 Sink 模板参数要求下列回调，签名统一为 `(const Bar&, bool is_auction)`；
 * 每个回调都是可选的，未实现视为空操作：
 * - `void on_bar_open(const Bar&, bool is_auction)`
 * - `void on_bar_update(const Bar&, bool is_auction)`
 * - `void on_bar_close(const Bar&, bool is_auction)`
 *
 * 签名必须与上述完全一致（含 `bool` 参数）：签名不匹配的实现视为未实现、静默忽略。
 *
 * `is_auction` 语义：**触发本次回调的 tick 是否处于 Open 竞价窗口**，是回调时刻的环境
 * 相位，不是被交出 bar 的属性（同一根 bar 可在窗口内开始、在连续交易中继续更新）。
 * 仅 Open 窗口标记；Close 窗口按普通 tick 处理、不标记；日线回调恒为 false（日线不分竞价）。
 * 组件只按 tick 判定：回放合成 tick 若落在 Open 窗口内同样标记 true，是否把回放期相位交给
 * 最终用户由调用方决定（BarStrategy 框架在回放期抑制）。
 *
 * 本类型三个回调均为空实现，是 builder 的默认 Sink，表示不关心 bar 事件。
 */
struct NullBarSink {
    void on_bar_open(const Bar&, bool) {}
    void on_bar_update(const Bar&, bool) {}
    void on_bar_close(const Bar&, bool) {}
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_TYPES_H_
