/**
 * @file bar_diff_streams.h
 * @brief issue #92 差分验收用的确定性 tick 流生成器
 *
 * 本头**只存在于测试代码**，不属于生产 seam，也不被生产代码包含。它为
 * 「新 IntradayBarBuilder / DailyBarBuilder ↔ 旧版查表语义 oracle（legacy_bar_oracle.h）」
 * 的逐根差分对比提供两份等价输入：
 * 1. 喂给新 builder 的完整流：含守卫 tick，每笔携带显式 `DiffDrop` 标签与期望丢弃计数，
 *    使测试能精确断言 `stats()`；
 * 2. 喂给 oracle 的干净子序列：`accepted_ticks()` 只保留 `expect == DiffDrop::None` 的 tick。
 *
 * 全部生成函数确定性：同一 seed 同一输出（只用 std::mt19937 与固定分布参数，不使用
 * std::random_device / 时间）。干净流（含竞价流、边界流）满足新 builder 的全部守卫口径：
 * 价格为正且有限、time ∈ 0..86399、date 为正、同一交易日内 tsec 单调不减、当日累计量额
 * 单调不减，因此 `expect == DiffDrop::None` 与「builder 接受」严格一一对应。
 */
#ifndef DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_STREAMS_H_
#define DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_STREAMS_H_

#include "bar_test_data.h"

#include <dztrader/bar/types.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace dztrader::strategy_api::test {

/// @brief 该 tick 预期被新 builder 的哪个守卫丢弃（None = 正常接受）
enum class DiffDrop : uint8_t {
    None,          ///< 正常接受（干净 tick 或同秒重复）
    OutOfSession,  ///< 不在任何交易小节/Open 竞价窗口内（含 time 越界）
    OutOfOrder,    ///< 交易日倒退或交易日内秒倒退
    BadVolume,     ///< 同一交易日内累计量或累计额倒退
};

/// @brief 流中的一笔 tick + 其期望归宿
struct DiffTick {
    DzTick tick{};                     ///< 行情快照
    DiffDrop expect = DiffDrop::None;  ///< 期望归宿（None = 应被接受）
};

/// @brief 一条差分 tick 流；期望丢弃计数用于精确断言 builder.stats()
struct DiffStream {
    std::vector<DiffTick> ticks;  ///< 按时间顺序的 tick 序列
    uint64_t out_of_session = 0;  ///< expect == OutOfSession 的 tick 数
    uint64_t out_of_order = 0;    ///< expect == OutOfOrder 的 tick 数
    uint64_t bad_volume = 0;      ///< expect == BadVolume 的 tick 数

    /// @brief 只取 expect == None 的 tick（喂给 oracle 的干净子序列）
    [[nodiscard]] std::vector<DzTick> accepted_ticks() const;
};

/// @brief 干净子序列：按流中原顺序保留 expect == None 的 tick
inline std::vector<DzTick> DiffStream::accepted_ticks() const {
    std::vector<DzTick> accepted;
    accepted.reserve(ticks.size());
    for (const DiffTick& item : ticks) {
        if (item.expect == DiffDrop::None) {
            accepted.push_back(item.tick);
        }
    }
    return accepted;
}

namespace bar_diff_streams_internal {

/// @brief 交易日内秒上限（23:59:59）
inline constexpr int32_t MAX_TSEC = 86399;

/// @brief 单日价格与累计量额游走状态
struct WalkState {
    double price = 100.0;             ///< 最新价（恒正、有限）
    int64_t cum_volume = 0;           ///< 当日累计成交量
    double cum_turnover = 0.0;        ///< 当日累计成交额
    DzLargeVolume open_interest = 0;  ///< 持仓量
};

/// @brief [lo, hi] 内均匀分布随机整数（lo >= hi 时直接返回 lo）
inline int32_t random_int(std::mt19937& rng, int32_t lo, int32_t hi) {
    if (lo >= hi) {
        return lo;
    }
    std::uniform_int_distribution<int32_t> dist(lo, hi);
    return dist(rng);
}

/// @brief 当日累计成交量收窄到 DzVolume 域（测试规模下不会触顶）
inline DzVolume clamp_volume(int64_t volume) {
    constexpr int64_t MAX_VOLUME = std::numeric_limits<DzVolume>::max();
    return static_cast<DzVolume>(std::min(volume, MAX_VOLUME));
}

/// @brief 休息时段（首小节之前、相邻小节之间、末小节之后）的闭区间
struct RestGap {
    int32_t begin_sec;  ///< 休息时段起点（交易日内秒）
    int32_t end_sec;    ///< 休息时段终点（交易日内秒）
};

/// @brief 汇总交易小节表里的全部休息时段（按时间升序）
inline std::vector<RestGap> rest_gaps(const dztrader::bar::TradingSessionTable& table) {
    std::vector<RestGap> gaps;
    int32_t cursor = 0;
    for (int32_t i = 0; i < table.session_count; ++i) {
        const dztrader::bar::TradingSession& session = table.sessions[i];
        if (session.begin_sec > cursor) {
            gaps.push_back(RestGap{cursor, session.begin_sec - 1});
        }
        cursor = session.end_sec + 1;
    }
    if (cursor <= MAX_TSEC) {
        gaps.push_back(RestGap{cursor, MAX_TSEC});
    }
    return gaps;
}

/// @brief tsec 所属交易小节下标（不在任何小节内返回 -1）
inline int32_t session_index_at(int32_t tsec, const dztrader::bar::TradingSessionTable& table) {
    for (int32_t i = 0; i < table.session_count; ++i) {
        const dztrader::bar::TradingSession& session = table.sessions[i];
        if (tsec >= session.begin_sec && tsec <= session.end_sec) {
            return i;
        }
    }
    return -1;
}

/// @brief tsec 是否落在某个 Open 竞价窗口内（端点含）
inline bool in_open_auction(int32_t tsec, const dztrader::bar::TradingSessionTable& table) {
    for (int32_t i = 0; i < table.auction_count; ++i) {
        const dztrader::bar::AuctionWindow& window = table.auctions[i];
        if (window.kind == dztrader::bar::AuctionKind::Open && tsec >= window.begin_sec &&
            tsec <= window.end_sec) {
            return true;
        }
    }
    return false;
}

/// @brief 在 [lo, hi] 内生成 count 个随机时刻并升序排列（同秒重复保留）
inline std::vector<int32_t> sorted_random_times(std::mt19937& rng,
                                                int32_t lo,
                                                int32_t hi,
                                                std::size_t count) {
    std::vector<int32_t> times;
    times.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        times.push_back(random_int(rng, lo, hi));
    }
    std::sort(times.begin(), times.end());
    return times;
}

/// @brief 交易日首笔 tick 前的游走初值：价格 100.0..200.0、累计量额从正基准起算
inline WalkState start_day(std::mt19937& rng) {
    WalkState state;
    state.price = 100.0 + static_cast<double>(random_int(rng, 0, 1000)) * 0.1;
    state.cum_volume = static_cast<int64_t>(random_int(rng, 1000, 2000));
    state.cum_turnover = state.price * static_cast<double>(state.cum_volume);
    state.open_interest = static_cast<DzLargeVolume>(random_int(rng, 0, 1000));
    return state;
}

/**
 * @brief 推进一笔干净 tick：价格正数游走、当日累计量额只增不减、持仓量非负递增
 *
 * @param state 单日游走状态（原地推进）
 * @param rng 随机源
 * @param tsec 交易日内秒（调用方保证在同一交易日内单调不减）
 * @param date 交易日
 * @return expect == None 的干净 tick
 */
inline DiffTick clean_tick(WalkState& state, std::mt19937& rng, int32_t tsec, DzDate date) {
    state.price += static_cast<double>(random_int(rng, -20, 20)) * 0.05;
    state.price = std::max(state.price, 1.0);
    state.price = std::min(state.price, 1.0e6);
    const int32_t volume_delta = random_int(rng, 0, 500);
    state.cum_volume += static_cast<int64_t>(volume_delta);
    state.cum_turnover += state.price * static_cast<double>(volume_delta + 1);
    state.open_interest += static_cast<DzLargeVolume>(random_int(rng, 0, 2));
    DzTick tick =
        make_tick(dztrader::bar::TradingTime::to_clock(tsec), state.price,
                  clamp_volume(state.cum_volume), state.cum_turnover, state.open_interest, date);
    return DiffTick{tick, DiffDrop::None};
}

/// @brief 按固定步长推进一笔干净 tick（边界流用，无随机）
inline DiffTick deterministic_tick(WalkState& state, int32_t tsec, DzDate date) {
    state.price += 0.5;
    state.cum_volume += 10;
    state.cum_turnover += state.price * 10.0;
    state.open_interest += 1;
    DzTick tick =
        make_tick(dztrader::bar::TradingTime::to_clock(tsec), state.price,
                  clamp_volume(state.cum_volume), state.cum_turnover, state.open_interest, date);
    return DiffTick{tick, DiffDrop::None};
}

/// @brief 上一笔已接受 tick 所在 Open 竞价窗口的归属小节下标（不在窗口内返回 -1）
inline int32_t open_auction_session_at(int32_t tsec,
                                       const dztrader::bar::TradingSessionTable& table) {
    for (int32_t i = 0; i < table.auction_count; ++i) {
        const dztrader::bar::AuctionWindow& window = table.auctions[i];
        if (window.kind == dztrader::bar::AuctionKind::Open && tsec >= window.begin_sec &&
            tsec <= window.end_sec) {
            return window.session_index;
        }
    }
    return -1;
}

/**
 * @brief 注入一笔 OutOfOrder tick：同一交易日内 tsec 严格小于上一笔已接受 tick
 *
 * 注入 tick 仍落在某个交易小节内、价格合法，且时间不在任何 Open 竞价窗口内；builder 的
 * 第 2 个守卫（tsec 倒退）先于时段/量额守卫命中，故只会使 out_of_order 加一，不改变
 * 任何 bar/累计状态，也不影响后续干净 tick 的合法性。
 *
 * @param rng 随机源
 * @param table 交易小节表
 * @param previous 上一笔已接受 tick（守卫注入基准）
 * @param stream 目标流（成功时追加并累计计数）
 * @return true = 已注入；false = 当前位置没有可行的小节内更早时刻（跳过本次注入）
 */
inline bool inject_out_of_order(std::mt19937& rng,
                                const dztrader::bar::TradingSessionTable& table,
                                const DiffTick& previous,
                                DiffStream& stream) {
    const int32_t previous_tsec = dztrader::bar::TradingTime::from_clock(previous.tick.time);
    if (previous_tsec <= 0) {
        return false;
    }
    int32_t low = 0;
    int32_t high = previous_tsec - 1;
    const int32_t owner = session_index_at(previous_tsec, table);
    if (owner >= 0) {
        const dztrader::bar::TradingSession& session = table.sessions[owner];
        if (previous_tsec > session.begin_sec) {
            low = session.begin_sec;
        } else {
            // 上一笔已接受 tick 就在小节起点：退回前一小节的更早时刻
            if (owner == 0) {
                return false;
            }
            low = table.sessions[owner - 1].begin_sec;
            high = table.sessions[owner - 1].end_sec;
        }
    } else {
        // 上一笔已接受 tick 落在 Open 竞价窗口内：退回其归属小节之前的小节
        const int32_t auction_owner = open_auction_session_at(previous_tsec, table);
        if (auction_owner <= 0) {
            return false;
        }
        low = table.sessions[auction_owner - 1].begin_sec;
        high = std::min(high, table.sessions[auction_owner - 1].end_sec);
    }
    if (low > high) {
        return false;
    }
    for (int32_t attempt = 0; attempt < 32; ++attempt) {
        const int32_t tsec = (attempt == 0) ? low : random_int(rng, low, high);
        if (in_open_auction(tsec, table)) {
            continue;  // 契约要求注入 tick 仍落在小节内
        }
        DzTick tick = previous.tick;
        tick.time = dztrader::bar::TradingTime::to_clock(tsec);
        tick.date = previous.tick.date;
        stream.ticks.push_back(DiffTick{tick, DiffDrop::OutOfOrder});
        ++stream.out_of_order;
        return true;
    }
    return false;
}

/**
 * @brief 注入一笔 BadVolume tick：与上一笔已接受 tick 同秒，累计量额严格更小
 *
 * 同秒保证不触发 out_of_order 守卫；时段沿用上一笔已接受 tick（必然合法），价格也合法，
 * 故只有第 6 个守卫（累计量额倒退）命中，只使 bad_volume 加一。
 *
 * @param rng 随机源
 * @param previous 上一笔已接受 tick（守卫注入基准）
 * @param stream 目标流（成功时追加并累计计数）
 * @return true = 已注入；false = 上一笔累计量太小，无法构造严格更小的累计量（跳过）
 */
inline bool inject_bad_volume(std::mt19937& rng, const DiffTick& previous, DiffStream& stream) {
    if (previous.tick.volume < 2) {
        return false;
    }
    DzTick tick = previous.tick;
    const int32_t drop = random_int(rng, 1, previous.tick.volume);
    tick.volume = static_cast<DzVolume>(previous.tick.volume - drop);
    tick.turnover = previous.tick.turnover - 1.0;
    stream.ticks.push_back(DiffTick{tick, DiffDrop::BadVolume});
    ++stream.bad_volume;
    return true;
}

/**
 * @brief 注入一笔 OutOfSession tick：休息时段内、且不在任何 Open 竞价窗口内
 *
 * 注入时刻严格晚于上一笔已接受 tick（不触发 out_of_order）且严格早于同一交易日的下一笔
 * 干净 tick（不破坏后续干净 tick 的单调性）；time 仍落在 0..86399，故只有时段守卫命中。
 *
 * @param rng 随机源
 * @param table 交易小节表
 * @param previous 上一笔已接受 tick（守卫注入基准）
 * @param next_same_day 同一交易日的下一笔干净 tick（无则传 nullptr，表示本 tick 是当日最后一笔）
 * @param stream 目标流（成功时追加并累计计数）
 * @return true = 已注入；false = 没有满足条件的休息时刻（跳过）
 */
inline bool inject_out_of_session(std::mt19937& rng,
                                  const dztrader::bar::TradingSessionTable& table,
                                  const DiffTick& previous,
                                  const DiffTick* next_same_day,
                                  DiffStream& stream) {
    const int32_t previous_tsec = dztrader::bar::TradingTime::from_clock(previous.tick.time);
    if (previous_tsec >= MAX_TSEC) {
        return false;
    }
    int32_t upper = MAX_TSEC;
    if (next_same_day != nullptr) {
        upper = dztrader::bar::TradingTime::from_clock(next_same_day->tick.time) - 1;
    }
    const int32_t lower = previous_tsec + 1;
    if (lower > upper) {
        return false;
    }
    const std::vector<RestGap> gaps = rest_gaps(table);
    for (const RestGap& gap : gaps) {
        const int32_t lo = std::max(gap.begin_sec, lower);
        const int32_t hi = std::min(gap.end_sec, upper);
        if (lo > hi) {
            continue;
        }
        for (int32_t attempt = 0; attempt < 32; ++attempt) {
            const int32_t tsec = (attempt == 0) ? lo : random_int(rng, lo, hi);
            if (session_index_at(tsec, table) >= 0 || in_open_auction(tsec, table)) {
                continue;
            }
            DzTick tick = previous.tick;
            tick.time = dztrader::bar::TradingTime::to_clock(tsec);
            tick.date = previous.tick.date;
            stream.ticks.push_back(DiffTick{tick, DiffDrop::OutOfSession});
            ++stream.out_of_session;
            return true;
        }
    }
    return false;
}

/**
 * @brief 日线 OutOfOrder 注入：同一交易日内 time 严格倒退，或 date 小于当前交易日
 *
 * 两种形态都只命中 builder 的第 2 个守卫（交易日/交易日内秒倒退），且注入点必然是「上一笔
 * 已接受 tick 之后」，故此时 builder 必然已有当前 bar，两种形态都会被丢弃。
 *
 * @param rng 随机源
 * @param previous 上一笔已接受 tick（守卫注入基准）
 * @param prefer_date_reversal true = 优先用「date 小于当前交易日」形态（覆盖该分支）
 * @param stream 目标流（成功时追加并累计计数）
 * @return true = 已注入；false = 无法构造（date 已到 1，理论上不会发生）
 */
inline bool daily_inject_out_of_order(std::mt19937& rng,
                                      const DiffTick& previous,
                                      bool prefer_date_reversal,
                                      DiffStream& stream) {
    DzTick tick = previous.tick;
    const int32_t previous_tsec = dztrader::bar::TradingTime::from_clock(previous.tick.time);
    const bool can_rewind_time = previous_tsec > 0;
    const bool can_rewind_date = previous.tick.date > 1;
    if (prefer_date_reversal && can_rewind_date) {
        tick.date = previous.tick.date - 1;
    } else if (can_rewind_time) {
        tick.time = dztrader::bar::TradingTime::to_clock(random_int(rng, 0, previous_tsec - 1));
    } else if (can_rewind_date) {
        tick.date = previous.tick.date - 1;
    } else {
        return false;
    }
    stream.ticks.push_back(DiffTick{tick, DiffDrop::OutOfOrder});
    ++stream.out_of_order;
    return true;
}

/// @brief 日线 BadVolume 注入：同一交易日内 time 不倒退，累计量额严格更小
inline bool daily_inject_bad_volume(std::mt19937& rng,
                                    const DiffTick& previous,
                                    DiffStream& stream) {
    if (previous.tick.volume < 2) {
        return false;
    }
    DzTick tick = previous.tick;
    const int32_t drop = random_int(rng, 1, previous.tick.volume);
    tick.volume = static_cast<DzVolume>(previous.tick.volume - drop);
    tick.turnover = previous.tick.turnover - 1.0;
    stream.ticks.push_back(DiffTick{tick, DiffDrop::BadVolume});
    ++stream.bad_volume;
    return true;
}

/// @brief 日线 OutOfSession 注入：time 越界（-1 / 86400），必然计入 out_of_session
inline bool daily_inject_out_of_session(uint32_t sequence,
                                        const DiffTick& previous,
                                        DiffStream& stream) {
    DzTick tick = previous.tick;
    tick.time = (sequence % 2U == 0U) ? -1 : 86400;
    stream.ticks.push_back(DiffTick{tick, DiffDrop::OutOfSession});
    ++stream.out_of_session;
    return true;
}

/**
 * @brief 日线价格游走一步（正数、有限）
 * @param rng 随机源
 * @param price 当前最新价（原地更新）
 * @return 新的最新价
 */
inline double daily_next_price(std::mt19937& rng, double price) {
    double next = price + static_cast<double>(random_int(rng, -20, 20)) * 0.05;
    next = std::max(next, 1.0);
    next = std::min(next, 1.0e6);
    return next;
}

}  // namespace bar_diff_streams_internal

/// @brief 日内周期随机干净流：每个交易日按 table 的小节顺序生成
///
/// 时刻在同一交易日内单调不减且全部落在某个小节内（端点含）；价格为正数随机游走；
/// volume/turnover 当日累计单调不减（volume 增量 0..500），open_interest 非负递增。
/// 全部 tick 的 expect 为 None，三个期望丢弃计数为 0。用于 1/3/5/15/60 分钟差分。
///
/// @param seed 随机种子（同一 seed 输出完全一致）
/// @param table 交易小节表（只使用 sessions，忽略竞价窗口）
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @param ticks_per_session 每个交易日每个小节的 tick 数
/// @return 干净 tick 流
inline DiffStream make_random_clean_stream(uint32_t seed,
                                           const dztrader::bar::TradingSessionTable& table,
                                           int32_t trading_day_count,
                                           std::size_t ticks_per_session) {
    DiffStream stream;
    std::mt19937 rng(seed);
    for (int32_t day = 0; day < trading_day_count; ++day) {
        const DzDate date = TRADING_DAY + day;
        bar_diff_streams_internal::WalkState state = bar_diff_streams_internal::start_day(rng);
        for (int32_t i = 0; i < table.session_count; ++i) {
            const dztrader::bar::TradingSession& session = table.sessions[i];
            // 当日首小节的 begin 秒留给边界流：随机流从 begin+1 起，使守卫流的 OutOfOrder
            // 注入在任何参数组合下都能找到同小节内更早的时刻（否则 tps=1 时可能无解）。
            const int32_t begin_sec = (i == 0 && session.begin_sec < session.end_sec)
                                          ? session.begin_sec + 1
                                          : session.begin_sec;
            const std::vector<int32_t> times = bar_diff_streams_internal::sorted_random_times(
                rng, begin_sec, session.end_sec, ticks_per_session);
            for (const int32_t tsec : times) {
                stream.ticks.push_back(
                    bar_diff_streams_internal::clean_tick(state, rng, tsec, date));
            }
        }
    }
    return stream;
}

/// @brief 在随机干净流基础上注入守卫 tick
///
/// 每笔干净 tick 之后按固定概率注入（并累计期望计数）：
/// - OutOfOrder：同一交易日内 tsec 严格小于上一笔已接受 tick（仍在小节内、价格合法）；
/// - BadVolume：与上一笔已接受 tick 同秒，累计量额严格小于当日累计基准；
/// - OutOfSession：落在小节之间休息时段（或首小节之前、末小节之后）且不在任何 Open
///   竞价窗口内，时刻晚于上一笔已接受 tick、早于同一交易日的下一笔干净 tick；
/// 另外按固定概率复制上一笔已接受 tick（同秒重复，expect = None）。
///
/// 三类守卫各有一次「补偿式覆盖」：某类还没命中过时，在首个可行位置强行注入，因此只要流
/// 非空且表允许，三项期望计数均 > 0，不依赖概率。注入 tick 只命中对应守卫一次：不改变
/// 任何 bar/累计状态，也不改变后续干净 tick 的合法性，故 builder.stats() 应与返回流的三项
/// 期望计数完全相等。
///
/// @param seed 随机种子（同一 seed 输出完全一致）
/// @param table 交易小节表
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @param ticks_per_session 每个交易日每个小节的干净 tick 数
/// @return 带守卫 tick 的流
inline DiffStream make_random_guarded_stream(uint32_t seed,
                                             const dztrader::bar::TradingSessionTable& table,
                                             int32_t trading_day_count,
                                             std::size_t ticks_per_session) {
    const DiffStream clean =
        make_random_clean_stream(seed, table, trading_day_count, ticks_per_session);
    DiffStream stream;
    std::mt19937 rng(seed ^ 0x9E3779B9U);
    // 三类守卫各至少命中一次（保证差分测试的守卫断言不会因参数退化而空转）
    bool out_of_order_covered = false;
    bool bad_volume_covered = false;
    bool out_of_session_covered = false;
    uint32_t rotation = 0;
    for (std::size_t i = 0; i < clean.ticks.size(); ++i) {
        const DiffTick& current = clean.ticks[i];
        stream.ticks.push_back(current);
        const DiffTick* next_same_day = nullptr;
        if (i + 1 < clean.ticks.size() && clean.ticks[i + 1].tick.date == current.tick.date) {
            next_same_day = &clean.ticks[i + 1];
        }
        if (rng() % 4U == 0U) {
            stream.ticks.push_back(DiffTick{current.tick, DiffDrop::None});
        }
        // 补偿式覆盖：某类守卫还没命中过时，在首个可行位置强行注入，不依赖概率
        if (!bad_volume_covered &&
            bar_diff_streams_internal::inject_bad_volume(rng, current, stream)) {
            bad_volume_covered = true;
        }
        if (!out_of_order_covered &&
            bar_diff_streams_internal::inject_out_of_order(rng, table, current, stream)) {
            out_of_order_covered = true;
        }
        if (!out_of_session_covered && bar_diff_streams_internal::inject_out_of_session(
                                           rng, table, current, next_same_day, stream)) {
            out_of_session_covered = true;
        }
        // 随机密度：按固定概率再注入一笔（类型轮转；不可行时改试下一个类型）
        if (rng() % 2U == 0U) {
            for (uint32_t candidate = 0; candidate < 3U; ++candidate) {
                const uint32_t type = (rotation + candidate) % 3U;
                bool injected = false;
                if (type == 0U) {
                    injected =
                        bar_diff_streams_internal::inject_out_of_order(rng, table, current, stream);
                } else if (type == 1U) {
                    injected = bar_diff_streams_internal::inject_bad_volume(rng, current, stream);
                } else {
                    injected = bar_diff_streams_internal::inject_out_of_session(
                        rng, table, current, next_same_day, stream);
                }
                if (injected) {
                    break;
                }
            }
            ++rotation;
        }
    }
    return stream;
}

/// @brief 竞价场景干净流：每个交易日先给每个 kind == Open 的竞价窗口发 3 笔 tick
///
/// 竞价 tick 的时刻取窗口 begin、中点、end（端点含），随后发该小节内的普通 tick（复用干净
/// 流的小节时刻逻辑）。每天的时刻集合整体升序（防御窗口与前一交易小节重叠的表），量额
/// 累计在成交顺序上单调。全部 tick 的 expect 为 None。
///
/// 仅适用于长度 <= 60s 且 begin 对齐分钟的 Open 窗口（旧版查表只覆盖单个分钟键）；调用方
/// 必须自行保证表满足该可表达性约束（由 legacy_table_compatible() 前置校验）。
///
/// @param seed 随机种子（同一 seed 输出完全一致）
/// @param table 交易小节与竞价窗口表
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @param ticks_per_session 每个交易日每个小节的普通 tick 数
/// @return 干净 tick 流
inline DiffStream make_auction_clean_stream(uint32_t seed,
                                            const dztrader::bar::TradingSessionTable& table,
                                            int32_t trading_day_count,
                                            std::size_t ticks_per_session) {
    DiffStream stream;
    std::mt19937 rng(seed);
    for (int32_t day = 0; day < trading_day_count; ++day) {
        const DzDate date = TRADING_DAY + day;
        std::vector<int32_t> times;
        for (int32_t i = 0; i < table.session_count; ++i) {
            for (int32_t a = 0; a < table.auction_count; ++a) {
                const dztrader::bar::AuctionWindow& window = table.auctions[a];
                if (window.kind != dztrader::bar::AuctionKind::Open || window.session_index != i) {
                    continue;
                }
                times.push_back(window.begin_sec);
                times.push_back(window.begin_sec + (window.end_sec - window.begin_sec) / 2);
                times.push_back(window.end_sec);
            }
            const std::vector<int32_t> session_times =
                bar_diff_streams_internal::sorted_random_times(
                    rng, table.sessions[i].begin_sec, table.sessions[i].end_sec, ticks_per_session);
            times.insert(times.end(), session_times.begin(), session_times.end());
        }
        std::sort(times.begin(), times.end());
        bar_diff_streams_internal::WalkState state = bar_diff_streams_internal::start_day(rng);
        for (const int32_t tsec : times) {
            stream.ticks.push_back(bar_diff_streams_internal::clean_tick(state, rng, tsec, date));
        }
    }
    return stream;
}

/// @brief 边界干净流（确定性、无随机）：覆盖小节端点与休息后恢复
///
/// 每个交易日、每个小节内依次发：小节 begin（含两笔同秒重复，共 3 笔同秒 tick）、
/// begin+1s、end-1s、end（小节末端 tick）；第二个及之后的小节在 begin 处多一笔同秒
/// tick（休息后恢复）。全部 tick 的 expect 为 None，价格/量额按固定步长确定推进。
///
/// @param table 交易小节表（只使用 sessions，忽略竞价窗口）
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @return 干净 tick 流
inline DiffStream make_edge_clean_stream(const dztrader::bar::TradingSessionTable& table,
                                         int32_t trading_day_count) {
    DiffStream stream;
    for (int32_t day = 0; day < trading_day_count; ++day) {
        const DzDate date = TRADING_DAY + day;
        bar_diff_streams_internal::WalkState state;
        state.price = 100.0 + static_cast<double>(day);
        state.cum_volume = 1000;
        state.cum_turnover = state.price * 1000.0;
        state.open_interest = 0;
        for (int32_t i = 0; i < table.session_count; ++i) {
            const dztrader::bar::TradingSession& session = table.sessions[i];
            const DiffTick base =
                bar_diff_streams_internal::deterministic_tick(state, session.begin_sec, date);
            stream.ticks.push_back(base);
            stream.ticks.push_back(base);  // begin 处第 1 笔同秒重复
            stream.ticks.push_back(base);  // begin 处第 2 笔同秒重复
            if (i > 0) {
                stream.ticks.push_back(base);  // 休息后恢复的同秒 tick
            }
            int32_t cursor = session.begin_sec;
            const int32_t edge_times[3] = {session.begin_sec + 1, session.end_sec - 1,
                                           session.end_sec};
            for (const int32_t edge_tsec : edge_times) {
                const int32_t tsec = std::min(std::max(edge_tsec, cursor), session.end_sec);
                stream.ticks.push_back(
                    bar_diff_streams_internal::deterministic_tick(state, tsec, date));
                cursor = tsec;
            }
        }
    }
    return stream;
}

/// @brief 日线随机干净流：每日累计 OHLC 自洽、时间从 0 附近开始、量额当日累计单调
///
/// 每个交易日 date = TRADING_DAY + day，交易日之间时间重新从 0 附近开始；当日 open 固定，
/// high 单调不减且 >= max(open, last)，low 单调不增且 <= min(open, last)，last_price 正数
/// 游走；volume/turnover 当日累计单调不减。全部 tick 的 expect 为 None。
///
/// @param seed 随机种子（同一 seed 输出完全一致）
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @param ticks_per_day 每个交易日的 tick 数
/// @return 干净 tick 流
inline DiffStream make_daily_random_stream(uint32_t seed,
                                           int32_t trading_day_count,
                                           std::size_t ticks_per_day) {
    DiffStream stream;
    std::mt19937 rng(seed);
    for (int32_t day = 0; day < trading_day_count; ++day) {
        const DzDate date = TRADING_DAY + day;
        const double open_price =
            100.0 + static_cast<double>(bar_diff_streams_internal::random_int(rng, 0, 1000)) * 0.1;
        double last_price = open_price;
        double highest_price = open_price;
        double lowest_price = open_price;
        int64_t cum_volume =
            static_cast<int64_t>(bar_diff_streams_internal::random_int(rng, 500, 1500));
        double cum_turnover = open_price * static_cast<double>(cum_volume);
        DzLargeVolume open_interest =
            static_cast<DzLargeVolume>(bar_diff_streams_internal::random_int(rng, 0, 1000));
        int32_t tsec = bar_diff_streams_internal::random_int(rng, 0, 3600);
        for (std::size_t i = 0; i < ticks_per_day; ++i) {
            if (i > 0) {
                tsec += bar_diff_streams_internal::random_int(rng, 1, 3600);
                tsec = std::min(tsec, bar_diff_streams_internal::MAX_TSEC);
            }
            last_price = bar_diff_streams_internal::daily_next_price(rng, last_price);
            highest_price = std::max(highest_price, last_price);
            lowest_price = std::min(lowest_price, last_price);
            const int32_t volume_delta = bar_diff_streams_internal::random_int(rng, 0, 500);
            cum_volume += static_cast<int64_t>(volume_delta);
            cum_turnover += last_price * static_cast<double>(volume_delta + 1);
            open_interest +=
                static_cast<DzLargeVolume>(bar_diff_streams_internal::random_int(rng, 0, 2));
            DzTick tick = make_daily_tick(dztrader::bar::TradingTime::to_clock(tsec), open_price,
                                          highest_price, lowest_price, last_price,
                                          bar_diff_streams_internal::clamp_volume(cum_volume),
                                          cum_turnover, open_interest, date);
            stream.ticks.push_back(DiffTick{tick, DiffDrop::None});
        }
    }
    return stream;
}

/// @brief 日线守卫流：在日线干净流上注入守卫 tick
///
/// 每笔干净 tick 之后按固定概率注入（并累计期望计数）：
/// - OutOfOrder：同一交易日内 time 严格小于上一笔已接受 tick，或 date 小于当前交易日；
/// - BadVolume：同一交易日内 time 不倒退，volume/turnover 严格小于当日累计基准；
/// - OutOfSession：time 越界（-1 或 86400）；
/// 另外按固定概率复制上一笔已接受 tick（同秒重复，expect = None）。日线 builder 不校验
/// 交易小节，故不注入休息时段形态。三类守卫同样各有一次补偿式覆盖，计数均 > 0。
///
/// @param seed 随机种子（同一 seed 输出完全一致）
/// @param trading_day_count 交易日数量（<= 0 时返回空流）
/// @param ticks_per_day 每个交易日的干净 tick 数
/// @return 带守卫 tick 的日线流
inline DiffStream make_daily_guarded_stream(uint32_t seed,
                                            int32_t trading_day_count,
                                            std::size_t ticks_per_day) {
    const DiffStream clean = make_daily_random_stream(seed, trading_day_count, ticks_per_day);
    DiffStream stream;
    std::mt19937 rng(seed ^ 0x5A5A5A5AU);
    // 三类守卫各至少命中一次（保证差分测试的守卫断言不会因参数退化而空转）
    bool out_of_order_covered = false;
    bool bad_volume_covered = false;
    bool out_of_session_covered = false;
    uint32_t rotation = 0;
    uint32_t out_of_session_seq = 0;
    for (const DiffTick& current : clean.ticks) {
        stream.ticks.push_back(current);
        if (rng() % 4U == 0U) {
            stream.ticks.push_back(DiffTick{current.tick, DiffDrop::None});
        }
        // 补偿式覆盖：某类守卫还没命中过时，在首个可行位置强行注入，不依赖概率
        if (!bad_volume_covered &&
            bar_diff_streams_internal::daily_inject_bad_volume(rng, current, stream)) {
            bad_volume_covered = true;
        }
        if (!out_of_order_covered &&
            bar_diff_streams_internal::daily_inject_out_of_order(rng, current, false, stream)) {
            out_of_order_covered = true;
        }
        if (!out_of_session_covered && bar_diff_streams_internal::daily_inject_out_of_session(
                                           out_of_session_seq, current, stream)) {
            out_of_session_covered = true;
        }
        ++out_of_session_seq;
        // 随机密度：按固定概率再注入一笔（类型轮转；不可行时改试下一个类型）
        if (rng() % 2U == 0U) {
            for (uint32_t candidate = 0; candidate < 3U; ++candidate) {
                const uint32_t type = (rotation + candidate) % 3U;
                bool injected = false;
                if (type == 0U) {
                    injected = bar_diff_streams_internal::daily_inject_out_of_order(
                        rng, current, (rotation % 2U) == 0U, stream);
                } else if (type == 1U) {
                    injected =
                        bar_diff_streams_internal::daily_inject_bad_volume(rng, current, stream);
                } else {
                    injected = bar_diff_streams_internal::daily_inject_out_of_session(
                        rotation, current, stream);
                }
                if (injected) {
                    break;
                }
            }
            ++rotation;
        }
    }
    return stream;
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_STREAMS_H_
