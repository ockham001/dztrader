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
    double open;                  ///< 开盘价（bar 首笔 last_price）
    double high;                  ///< 最高价（bar 内 last_price 最大值）
    double low;                   ///< 最低价（bar 内 last_price 最小值）
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
    Open,   ///< 开盘竞价：tick 归后一小节首桶，窗口内不回调
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
        constexpr int32_t MAX_TSEC = 86399;  // 17:59:59，交易日终点前 1 秒
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
 * @brief tick→bar 合成的丢弃计数（构造后从 0 累计）
 */
struct BarBuilderStats {
    uint64_t out_of_session = 0;  ///< 不在任何交易小节/Open 竞价窗口内（含 time 越界）的 tick 数
    uint64_t out_of_order = 0;    ///< 交易日非正/倒退、交易日内秒倒退的 tick 数
    uint64_t bad_volume = 0;      ///< 同交易日内累计量/额倒退（含 NaN/Inf）的 tick 数
};

/**
 * @brief 空 sink：三个 bar 回调均为空实现
 *
 * builder 的 Sink 模板参数默认使用本类型，表示不关心 bar 事件。
 */
struct NullBarSink {
    void on_bar_open(const Bar&) {}
    void on_bar_update(const Bar&) {}
    void on_bar_close(const Bar&) {}
};

}  // namespace dztrader::bar

#endif  // DZTRADER_BAR_TYPES_H_
