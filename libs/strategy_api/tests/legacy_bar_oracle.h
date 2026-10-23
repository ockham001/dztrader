#ifndef DZTRADER_STRATEGY_API_TESTS_LEGACY_BAR_ORACLE_H_
#define DZTRADER_STRATEGY_API_TESTS_LEGACY_BAR_ORACLE_H_

/**
 * @file legacy_bar_oracle.h
 * @brief issue #92 差分验收用的旧版 tick→bar 参考实现（oracle）
 *
 * 本头**只存在于测试代码**，不属于生产 seam，也不被生产代码包含。
 *
 * 它独立复刻 2023-08 前实盘版本的三段实现，不复用被测 builder 的任何代码：
 * - `dzLib::dataCenter::attributeParse::createStdTimeIDComMap()`：从 18:00 起逐分钟预计算
 *   1/3/5/15 分钟与 1 小时四档网格，每项携带标准 bar 起始时刻与日内排序 `id`；
 * - `dzLib::dataCenter::attributeParse::createTimeIDComMap()`：把交易小节的每个整分钟与
 *   小节末端分钟、以及集合竞价窗口写入以 `"HH:MM"` 字符串为键的查表；
 * - `dzLib::strategy::barGen<>::onTick(tick, isCallAuction, timeIDCom)`：查表后按
 *   `bot`（bar 起始时刻字符串）与 `id` 走「同桶更新 / 换桶开新根 / 跨日换根」状态机。
 *
 * 今日内 bar 的字段口径（ADR 0031 决策 6/9）：O/H/L/C 用 `last_price`（竞价 tick 覆盖为
 * O=H=L=C），`volume = tick.volume - bar 起始累计基准`、`turnover` 同构差分，
 * `open_interest` 取最后一笔，`last_tick_sec` 为最后一笔 tick 的交易日内秒。
 *
 * 日线（ADR 0031 决策 7）独立成 `LegacyDailyBarOracle`：直接用 tick 的当日累计字段。
 *
 * 与旧版的**显式差异**（差分断言白名单，见 bar_diff_test.cpp 的同名注释）：
 * 1. 旧版对 `callAuction2`（收盘竞价）同样把该分钟映射到后一分钟并置 `isCallAuction`，
 *    从而覆盖 O=H=L=C；ADR 0031 决策 5 将 Close 窗口定为普通 tick，故本 oracle 不建模
 *    Close 窗口的映射与覆盖（`create_legacy_lookup` 直接跳过 Close 窗口）；
 * 2. 旧版不支持长度超过 1 分钟或跨分钟键的竞价窗口；本 oracle 对不可表达的表 fail-fast，
 *    差分测试只用可表达的表（`legacy_table_compatible()` 前置校验）；
 * 3. 旧版 dispatcher 对日线也先查交易小节表（`strategyCreateBarCom::onTick`）；ADR 0031
 *    决策 7 明确日线不依赖小节表，故 `LegacyDailyBarOracle` 不做查表过滤，只复现日线数据口径；
 * 4. 被测 builder 新增的非法数据守卫（时间越界、交易日倒退、同秒内 tsec 倒退、累计量额倒退、
 *    非法价格）旧版没有；差分测试只把「预期被接受」的 tick 喂给 oracle（由 tick 流的
 *    `DiffDrop` 标签显式排除），guard 行为另由 #87 测试覆盖；
 * 5. 旧版没有 `turnover/cum_turnover/period_sec/is_final` 字段；`turnover` 按同一差分规则
 *    由 oracle 自行推导，其余三项按本仓 Bar 语义填充后逐字段比较。
 */
#include <dztrader/bar/types.h>
#include <dztrader/struct.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader::strategy_api::test {

/// @brief 旧版 `data::timeIDCom` 的周期槽位（顺序与旧版结构体字段一致）
enum class LegacyPeriodSlot : int32_t { Min1 = 0, Min3 = 1, Min5 = 2, Min15 = 3, Hour1 = 4 };

/// @brief 旧版预计算的周期档位数量
inline constexpr int32_t LEGACY_PERIOD_SLOT_COUNT = 5;

/**
 * @brief 周期秒数 → 旧版周期槽位下标
 * @param period_sec 周期秒数
 * @return 槽位下标；不是旧版预计算档位（1/3/5/15 分钟、1 小时）时返回 -1
 */
[[nodiscard]] constexpr int32_t legacy_period_slot(int32_t period_sec) noexcept {
    switch (period_sec) {
        case 60:
            return static_cast<int32_t>(LegacyPeriodSlot::Min1);
        case 180:
            return static_cast<int32_t>(LegacyPeriodSlot::Min3);
        case 300:
            return static_cast<int32_t>(LegacyPeriodSlot::Min5);
        case 900:
            return static_cast<int32_t>(LegacyPeriodSlot::Min15);
        case 3600:
            return static_cast<int32_t>(LegacyPeriodSlot::Hour1);
        default:
            return -1;
    }
}

/**
 * @brief 等价旧版 `data::timeIDCom`：一个整分钟时间键对应的各周期 bar 起始与排序 id
 *
 * `begin_sec` 以交易日内秒表示（由旧版的标准 bar 起始字符串换算），`id` 是旧版逐分钟递增的
 * `value` 计数器在「该周期最近一个桶边界分钟」处的取值（旧版用它判断桶是否更晚）。
 */
struct LegacyMinuteEntry {
    int32_t begin_sec[LEGACY_PERIOD_SLOT_COUNT] = {};  ///< 各周期标准 bar 起始（交易日内秒）
    int32_t id[LEGACY_PERIOD_SLOT_COUNT] = {};         ///< 各周期桶边界的旧版递增 id
    bool is_call_auction = false;                      ///< 旧版 timeIDCom::isCallAuction
};

/// @brief 交易日内秒 → 旧版查表键 `"HH:MM"`（墙钟小时:分钟）
[[nodiscard]] inline std::string legacy_minute_key(DzTime clock) {
    const int32_t hour = clock / 3600;
    const int32_t minute = (clock % 3600) / 60;
    // 24 小时制 "HH:MM" 最多 5 字符；缓冲留足余量让编译器无法报截断
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return std::string(buffer);
}

/// @brief 交易日内秒 → 旧版查表键 `"HH:MM"`
[[nodiscard]] inline std::string legacy_minute_key_from_tsec(int32_t tsec) {
    return legacy_minute_key(bar::TradingTime::to_clock(tsec));
}

/**
 * @brief 等价旧版 `attributeParse::createStdTimeIDComMap()`
 *
 * 旧版把一天拆成两段墙钟区间（18:00–23:59 与 00:00–17:59）逐分钟预计算：
 * - 1 分钟：每分钟都是一个桶起点，id = 当前 `value`；
 * - 3/5/15 分钟：沿用旧版的计数初值（3/5/15）与「计数满则回到 0 并取当前分钟」逻辑；
 * - 1 小时：`t.hours()` 变化的那一分钟，取 `"HH:00:00"` 为起点，id = 当前 `value`。
 *
 * @return 以 `"HH:MM"` 为键的标准分钟网格（1440 项）
 */
[[nodiscard]] inline std::unordered_map<std::string, LegacyMinuteEntry> create_legacy_std_grid() {
    constexpr int32_t SLOT_MIN1 = static_cast<int32_t>(LegacyPeriodSlot::Min1);
    constexpr int32_t SLOT_MIN3 = static_cast<int32_t>(LegacyPeriodSlot::Min3);
    constexpr int32_t SLOT_MIN5 = static_cast<int32_t>(LegacyPeriodSlot::Min5);
    constexpr int32_t SLOT_MIN15 = static_cast<int32_t>(LegacyPeriodSlot::Min15);
    constexpr int32_t SLOT_HOUR1 = static_cast<int32_t>(LegacyPeriodSlot::Hour1);
    /// 旧版两段循环的墙钟区间：[18:00, 23:59:59) 与 [00:00, 17:59:59)
    constexpr DzTime LOOP_START[2] = {bar::TradingTime::ANCHOR_CLOCK_SEC, 0};
    constexpr DzTime LOOP_END[2] = {23 * 3600 + 59 * 60 + 59, 17 * 3600 + 59 * 60 + 59};

    std::unordered_map<std::string, LegacyMinuteEntry> grid;
    grid.reserve(1440);
    int32_t value = 0;
    for (int32_t loop = 0; loop < 2; ++loop) {
        // 旧版每个循环内独立初始化 3/5/15 分钟与小时的计数状态
        int32_t count_min3 = 3;
        int32_t count_min5 = 5;
        int32_t count_min15 = 15;
        int32_t begin_min3 = 0;
        int32_t begin_min5 = 0;
        int32_t begin_min15 = 0;
        int32_t id_min3 = 0;
        int32_t id_min5 = 0;
        int32_t id_min15 = 0;
        int32_t begin_hour1 = 0;
        int32_t id_hour1 = 0;
        int32_t last_hour = 9999;
        // 旧版 while (t < end) 逐分钟推进：步数由区间长度推出（第一段 360 个分钟键、第二段 1080）
        const int32_t minutes_per_loop =
            static_cast<int32_t>((LOOP_END[loop] - LOOP_START[loop]) / 60) + 1;
        for (int32_t step = 0; step < minutes_per_loop; ++step) {
            const DzTime clock = LOOP_START[loop] + static_cast<DzTime>(step) * 60;
            if (count_min3 < 2) {
                ++count_min3;
            } else {
                count_min3 = 0;
                begin_min3 = clock;
                id_min3 = value;
            }
            if (count_min5 < 4) {
                ++count_min5;
            } else {
                count_min5 = 0;
                begin_min5 = clock;
                id_min5 = value;
            }
            if (count_min15 < 14) {
                ++count_min15;
            } else {
                count_min15 = 0;
                begin_min15 = clock;
                id_min15 = value;
            }
            const int32_t hour = clock / 3600;
            if (hour != last_hour) {
                last_hour = hour;
                begin_hour1 = hour * 3600;
                id_hour1 = value;
            }

            LegacyMinuteEntry entry;
            entry.begin_sec[SLOT_MIN1] = bar::TradingTime::from_clock(clock);
            entry.id[SLOT_MIN1] = value;
            entry.begin_sec[SLOT_MIN3] = bar::TradingTime::from_clock(begin_min3);
            entry.id[SLOT_MIN3] = id_min3;
            entry.begin_sec[SLOT_MIN5] = bar::TradingTime::from_clock(begin_min5);
            entry.id[SLOT_MIN5] = id_min5;
            entry.begin_sec[SLOT_MIN15] = bar::TradingTime::from_clock(begin_min15);
            entry.id[SLOT_MIN15] = id_min15;
            entry.begin_sec[SLOT_HOUR1] = bar::TradingTime::from_clock(begin_hour1);
            entry.id[SLOT_HOUR1] = id_hour1;
            grid[legacy_minute_key(clock)] = entry;
            ++value;
        }
    }
    return grid;
}

/**
 * @brief 等价旧版 `attributeParse::createTimeIDComMap()`
 *
 * 对每个交易小节，从 `begin_sec` 起逐分钟把标准网格项写入查表，并把**小节末端分钟**映射到
 * 最后一个分钟项（旧版 `lastTimeIDCom`，即「小节末端时刻的 tick 归前一根」）。
 *
 * Open 竞价窗口按旧版 `callAuction1` 规则处理：窗口所在分钟映射到「窗口起点 + 60 秒」的
 * 分钟项，并置 `is_call_auction`（旧版即靠这一点让竞价 tick 归后一小节首根并覆盖 O=H=L=C）。
 * Close 窗口不参与映射（ADR 0031 决策 5 定为普通 tick，见文件头白名单第 1 条）。
 *
 * @param table 交易小节与竞价窗口表
 * @return 以 `"HH:MM"` 为键的旧版查表
 * @throws std::invalid_argument 表无法被旧版查表语义表达（小节分钟不在标准网格、竞价窗口跨
 *         分钟或映射目标不在小节表内）
 */
[[nodiscard]] inline std::unordered_map<std::string, LegacyMinuteEntry> create_legacy_lookup(
    const bar::TradingSessionTable& table) {
    const auto std_grid = create_legacy_std_grid();
    std::unordered_map<std::string, LegacyMinuteEntry> lookup;
    for (int32_t i = 0; i < table.session_count; ++i) {
        const bar::TradingSession& session = table.sessions[i];
        LegacyMinuteEntry last_entry;
        bool has_last_entry = false;
        for (int32_t tsec = session.begin_sec; tsec < session.end_sec; tsec += 60) {
            const std::string key = legacy_minute_key_from_tsec(tsec);
            const auto found = std_grid.find(key);
            if (found == std_grid.end()) {
                throw std::invalid_argument(
                    "legacy lookup: session minute missing from std grid | key=" + key);
            }
            lookup[key] = found->second;
            last_entry = found->second;
            has_last_entry = true;
        }
        if (!has_last_entry) {
            throw std::invalid_argument("legacy lookup: session has no whole minute | begin_sec=" +
                                        std::to_string(session.begin_sec) +
                                        " end_sec=" + std::to_string(session.end_sec));
        }
        // 小节末端 tick 归前一根：旧版把小节结束时刻的分钟键映射到最后一个分钟项
        lookup[legacy_minute_key_from_tsec(session.end_sec)] = last_entry;
    }
    for (int32_t i = 0; i < table.auction_count; ++i) {
        const bar::AuctionWindow& auction = table.auctions[i];
        if (auction.kind != bar::AuctionKind::Open) {
            continue;  // Close 窗口按普通 tick 处理（白名单第 1 条）
        }
        if (auction.begin_sec / 60 != auction.end_sec / 60) {
            throw std::invalid_argument(
                "legacy lookup: open auction window spans minute keys | begin_sec=" +
                std::to_string(auction.begin_sec) + " end_sec=" + std::to_string(auction.end_sec));
        }
        const std::string target_key = legacy_minute_key_from_tsec(auction.begin_sec + 60);
        const auto target = lookup.find(target_key);
        if (target == lookup.end()) {
            throw std::invalid_argument(
                "legacy lookup: open auction target minute outside sessions | target=" +
                target_key);
        }
        LegacyMinuteEntry entry = target->second;
        entry.is_call_auction = true;
        lookup[legacy_minute_key_from_tsec(auction.begin_sec)] = entry;
    }
    return lookup;
}

/**
 * @brief 交易小节表能否被旧版查表语义表达（差分测试的用表前置条件）
 *
 * 要求：session 数 >= 1；每个 Open 竞价窗口落在单个分钟键内、且该分钟不属于任何交易小节
 * （旧版窗口在小节之前）。Close 窗口不影响判定（不参与映射）。
 *
 * @param table 交易小节与竞价窗口表
 * @param reason 不可表达时的原因（可为 nullptr）
 * @return true = 可表达
 */
[[nodiscard]] inline bool legacy_table_compatible(const bar::TradingSessionTable& table,
                                                  std::string* reason = nullptr) {
    const auto fail = [reason](const std::string& message) {
        if (reason != nullptr) {
            *reason = message;
        }
        return false;
    };
    if (!table.valid()) {
        return fail("table is not valid");
    }
    for (int32_t i = 0; i < table.session_count; ++i) {
        const bar::TradingSession& session = table.sessions[i];
        // 旧版以 HH:MM 字符串为键，只能表达整分钟对齐的小节（见文件头白名单第 2 条）
        if (session.begin_sec % 60 != 0 || session.end_sec % 60 != 0) {
            return fail(
                "session is not minute aligned | begin_sec=" + std::to_string(session.begin_sec) +
                " end_sec=" + std::to_string(session.end_sec));
        }
    }
    for (int32_t i = 0; i < table.auction_count; ++i) {
        const bar::AuctionWindow& auction = table.auctions[i];
        if (auction.kind != bar::AuctionKind::Open) {
            continue;
        }
        if (auction.begin_sec / 60 != auction.end_sec / 60) {
            return fail("open auction window spans minute keys | begin_sec=" +
                        std::to_string(auction.begin_sec) +
                        " end_sec=" + std::to_string(auction.end_sec));
        }
        for (int32_t s = 0; s < table.session_count; ++s) {
            const bar::TradingSession& session = table.sessions[s];
            if (auction.begin_sec >= session.begin_sec && auction.begin_sec <= session.end_sec) {
                return fail("open auction window overlaps a session");
            }
        }
        bool target_found = false;
        for (int32_t s = 0; s < table.session_count; ++s) {
            const bar::TradingSession& session = table.sessions[s];
            if (auction.begin_sec + 60 >= session.begin_sec &&
                auction.begin_sec + 60 <= session.end_sec) {
                target_found = true;
                break;
            }
        }
        if (!target_found) {
            return fail("open auction window target minute is outside every session");
        }
    }
    return true;
}

/**
 * @brief 旧版日内容周期 tick→bar 参考实现（等价 `barGen::onTick(tick, isCallAuction, timeIDCom)`）
 *
 * 每笔 tick 先按 `"HH:MM"` 查表：查不到即丢弃（旧版 dispatcher 的 `return`，等价非交易时段）。
 * 查表命中后按旧版状态机处理：
 * - 同一交易日的同一桶起点：行情字段更新（竞价 tick 覆盖 O=H=L=C）；
 * - 同一交易日更晚桶（`id >= 当前 id`）：先收旧 bar（`is_final = true`）再开新 bar，
 *   新 bar 的累计量额基准取上一笔已接受 tick 的累计值（旧版 `bov = 旧 bar.eov`）；
 * - 交易日变大：先收旧 bar，再开新 bar 且累计基准重置为 0（旧版 `bov = 0`）；
 * - 交易日变小或桶更早：静默丢弃（旧版 `diffDate > 0` / `id < 当前 id` 分支）。
 */
class LegacyBarOracle {
public:
    /**
     * @brief 构造 oracle
     * @param table 交易小节与竞价窗口表（必须可被旧版查表语义表达）
     * @param period_sec 周期秒数（只接受旧版预计算档位 60/180/300/900/3600）
     * @throws std::invalid_argument 周期不是旧版档位，或表不可表达
     */
    LegacyBarOracle(const bar::TradingSessionTable& table, int32_t period_sec)
        : lookup_(create_legacy_lookup(table)),
          period_sec_(period_sec),
          slot_(legacy_period_slot(period_sec)) {
        if (slot_ < 0) {
            throw std::invalid_argument(
                "legacy oracle: period is not a legacy grid period | period_sec=" +
                std::to_string(period_sec));
        }
    }

    /**
     * @brief 喂入一笔 tick（旧版查表 + 状态机）
     * @param tick 行情快照
     */
    void on_tick(const DzTick& tick) {
        const auto found = lookup_.find(legacy_minute_key(tick.time));
        if (found == lookup_.end()) {
            return;  // 旧版 dispatcher 查不到时间键即丢弃
        }
        const LegacyMinuteEntry& entry = found->second;
        const int32_t begin_sec = entry.begin_sec[slot_];
        const int32_t bucket_id = entry.id[slot_];
        if (!has_bar_) {
            begin_bar(tick, begin_sec, bucket_id);
            return;
        }
        if (tick.date == bar_.trading_day) {
            if (begin_sec == bar_.begin_sec) {
                if (entry.is_call_auction) {
                    update_auction_bar(tick);
                } else {
                    update_bar(tick);
                }
            } else if (bucket_id >= bucket_id_) {
                close_current_bar();
                begin_bar(tick, begin_sec, bucket_id);
            }
            // 同交易日更早的桶：旧版静默丢弃
            return;
        }
        if (tick.date > bar_.trading_day) {
            close_current_bar();
            begin_bar(tick, begin_sec, bucket_id);
        }
        // 交易日倒退：旧版静默丢弃
    }

    /**
     * @brief 完整 bar 序列：已收 bar（按收盘顺序）+ 末尾形成中 bar（若有）
     * @return bar 序列
     */
    [[nodiscard]] std::vector<bar::Bar> bars() const {
        std::vector<bar::Bar> result = completed_;
        if (has_bar_) {
            result.push_back(bar_);
        }
        return result;
    }

private:
    /**
     * @brief 用本笔 tick 建立新 bar（旧版换桶/换日分支）
     *
     * 累计量额基准：同一交易日的新桶沿用上一笔已接受 tick 的累计值（旧版 `bov = 旧 bar.eov`），
     * 交易日变化重置为 0（旧版 `bov = 0`）。
     */
    void begin_bar(const DzTick& tick, int32_t begin_sec, int32_t bucket_id) {
        const bool same_day_basis = has_accepted_tick_ && tick.date == accepted_day_;
        const int64_t start_cum_volume = same_day_basis ? accepted_cum_volume_ : 0;
        const double start_cum_turnover = same_day_basis ? accepted_cum_turnover_ : 0.0;
        bar_ = bar::Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = begin_sec;
        bar_.period_sec = period_sec_;
        bar_.is_final = false;
        bar_start_cum_volume_ = start_cum_volume;
        bar_start_cum_turnover_ = start_cum_turnover;
        bucket_id_ = bucket_id;
        has_bar_ = true;
        // 新 bar 的首笔 tick 决定 O=H=L=C（竞价与普通 tick 的建根口径一致，仅同桶后续更新不同）
        bar_.open = tick.last_price;
        bar_.high = tick.last_price;
        bar_.low = tick.last_price;
        bar_.close = tick.last_price;
        update_market_data(tick);
    }

    /// @brief 同桶普通 tick：H/L 取极值、C 取最新价
    void update_bar(const DzTick& tick) {
        if (tick.last_price > bar_.high) {
            bar_.high = tick.last_price;
        }
        if (tick.last_price < bar_.low) {
            bar_.low = tick.last_price;
        }
        bar_.close = tick.last_price;
        update_market_data(tick);
    }

    /// @brief 同桶竞价 tick：旧版按 O=H=L=C=last_price 逐笔覆盖
    void update_auction_bar(const DzTick& tick) {
        bar_.open = tick.last_price;
        bar_.high = tick.last_price;
        bar_.low = tick.last_price;
        bar_.close = tick.last_price;
        update_market_data(tick);
    }

    /// @brief 更新量额差分、累计字段、持仓量与最后 tick 时间（普通/竞价共用）
    void update_market_data(const DzTick& tick) {
        const int64_t cum_volume = static_cast<int64_t>(tick.volume);
        bar_.last_tick_sec = bar::TradingTime::from_clock(tick.time);
        bar_.volume = cum_volume - bar_start_cum_volume_;
        bar_.turnover = tick.turnover - bar_start_cum_turnover_;
        bar_.cum_volume = cum_volume;
        bar_.cum_turnover = tick.turnover;
        bar_.open_interest = tick.open_interest;
        accepted_day_ = tick.date;
        accepted_cum_volume_ = cum_volume;
        accepted_cum_turnover_ = tick.turnover;
        has_accepted_tick_ = true;
    }

    /// @brief 收起当前 bar（旧版进入下一桶/下一交易日时不再回调形成中 bar）
    void close_current_bar() {
        bar_.is_final = true;
        completed_.push_back(bar_);
        has_bar_ = false;
    }

    std::unordered_map<std::string, LegacyMinuteEntry> lookup_;  ///< 旧版 "HH:MM" 查表
    int32_t period_sec_;                                         ///< 周期秒数
    int32_t slot_;                                               ///< 周期在旧版 timeIDCom 的槽位
    bar::Bar bar_{};                                             ///< 当前形成中 bar
    std::vector<bar::Bar> completed_;                            ///< 已收 bar
    int32_t bucket_id_ = 0;                                      ///< 当前 bar 的旧版排序 id
    int64_t bar_start_cum_volume_ = 0;                           ///< 本 bar 起始累计成交量基准
    double bar_start_cum_turnover_ = 0.0;                        ///< 本 bar 起始累计成交额基准
    bool has_bar_ = false;                                       ///< 是否已有形成中 bar
    bool has_accepted_tick_ = false;                             ///< 是否已接受过 tick
    DzDate accepted_day_ = 0;                                    ///< 最后一笔已接受 tick 的交易日
    int64_t accepted_cum_volume_ = 0;     ///< 最后一笔已接受 tick 的累计成交量
    double accepted_cum_turnover_ = 0.0;  ///< 最后一笔已接受 tick 的累计成交额
};

/**
 * @brief 旧版日线 tick→bar 参考实现（等价 `barGen::onTick(tick, isCallAuction)` 的日线口径）
 *
 * 直接使用 tick 的当日累计字段：`open = open_price`、`high = highest_price`、
 * `low = lowest_price`、`close = last_price`、`volume = cum_volume = tick.volume`、
 * `turnover = cum_turnover = tick.turnover`、`open_interest` 取最后一笔。
 * `begin_sec = 0`（18:00 标记）、`period_sec = 0`。
 *
 * 交易日变大时先收旧日线（`is_final = true`）再开新日线；交易日倒退静默丢弃。
 * 不做交易小节表查表过滤（见文件头白名单第 3 条）。
 */
class LegacyDailyBarOracle {
public:
    /// @brief 构造 oracle
    LegacyDailyBarOracle() = default;

    /**
     * @brief 喂入一笔 tick
     * @param tick 行情快照
     */
    void on_tick(const DzTick& tick) {
        if (!has_bar_) {
            begin_bar(tick);
            return;
        }
        if (tick.date == bar_.trading_day) {
            update_bar(tick);
        } else if (tick.date > bar_.trading_day) {
            close_current_bar();
            begin_bar(tick);
        }
        // 交易日倒退：旧版静默丢弃
    }

    /**
     * @brief 完整日线序列：已收日线 + 末尾形成中日线（若有）
     * @return 日线序列
     */
    [[nodiscard]] std::vector<bar::Bar> bars() const {
        std::vector<bar::Bar> result = completed_;
        if (has_bar_) {
            result.push_back(bar_);
        }
        return result;
    }

private:
    /// @brief 用本笔 tick 开新日线（当日累计字段直接映射，起点基准恒为 0）
    void begin_bar(const DzTick& tick) {
        bar_ = bar::Bar{};
        bar_.trading_day = tick.date;
        bar_.begin_sec = 0;
        bar_.period_sec = 0;
        bar_.is_final = false;
        has_bar_ = true;
        update_bar(tick);
    }

    /// @brief 用本笔 tick 的当日累计字段更新日线全部行情字段
    void update_bar(const DzTick& tick) {
        bar_.open = tick.open_price;
        bar_.high = tick.highest_price;
        bar_.low = tick.lowest_price;
        bar_.close = tick.last_price;
        bar_.last_tick_sec = bar::TradingTime::from_clock(tick.time);
        bar_.volume = static_cast<int64_t>(tick.volume);
        bar_.turnover = tick.turnover;
        bar_.cum_volume = static_cast<int64_t>(tick.volume);
        bar_.cum_turnover = tick.turnover;
        bar_.open_interest = tick.open_interest;
    }

    /// @brief 收起当前日线
    void close_current_bar() {
        bar_.is_final = true;
        completed_.push_back(bar_);
        has_bar_ = false;
    }

    bar::Bar bar_{};                   ///< 当前形成中日线
    std::vector<bar::Bar> completed_;  ///< 已收日线
    bool has_bar_ = false;             ///< 是否已有形成中日线
};

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_LEGACY_BAR_ORACLE_H_
