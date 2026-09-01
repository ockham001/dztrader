/**
 * @file td_ingest.h
 * @brief SDK TD 数据 ingest 门: 账户级 seq 水位过滤、断档、倒退重置、成交去重
 *
 * 纯逻辑核心 (spec §5.1/5.2/5.4/5.5), 无 shm/db 依赖, 可独立测试。
 * Task 9 (dzweb 后端 ingest) 复用本 gate。
 */
#ifndef DZTRADER_STRATEGY_API_TD_INGEST_H_
#define DZTRADER_STRATEGY_API_TD_INGEST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dztrader {

/// TD 数据 ingest 门 (纯逻辑, 无 shm/db 依赖)。
///
/// 语义 (spec §5.1/5.2/5.4/5.5, 契约 strategy "SDK ingest 过滤职责"):
///   - W 过滤: seq ≤ W (DB 查得水位) 或 ≤ last_applied (自身已应用) 的帧跳过
///   - 断档: 首帧 seq > W+1 时标记 gap(from=W+1, to=seq-1), 由调用方查库回补
///   - 倒退: detect_reset(seq < last_applied) → 调用方重建 DB 水位后 reset_account
///   - 成交去重: (account_id, trading_day, trade_id) 二道防线, 交易日切换清理
///
/// 单线程契约: 与 dz_next_event 事件循环同线程, 无需加锁。
class TdIngestGate {
public:
    /// admit 裁决
    enum class Verdict { kApply, kSkip };

    /// 断档区间 (调用方据此查库回补 [from, to])
    struct Gap {
        std::string account_id;
        uint64_t from = 0;
        uint64_t to = 0;
    };

    /// 设账户水位 W (启动时从 TD 库 MAX(seq) 查得)。未设 W 的账户等价于 W=0。
    void set_watermark(const std::string& account_id, uint64_t w);

    /// 帧准入。裁决规则 (铁律: 跳过判定只用 DB 水位 W 与自身 last_applied):
    ///   - 首帧 (该账户无 last_applied) 且 seq > W+1 → 标记 gap(from=W+1, to=seq-1), 应用本帧
    ///   - seq ≤ last_applied → kSkip (已应用防重)
    ///   - seq ≤ W → kSkip (快照已含)
    ///   - 其余 → 推进 last_applied → kApply
    /// 首帧 seq == W+1 或 seq ≤ W 不记 gap; 断档只报一次 (take_pending_gap 取走后清空)。
    Verdict admit(const std::string& account_id, uint64_t seq);

    /// 取走待处理断档 (有则返回并清除, 无返回 nullopt)。每次 admit 至多产生一个 gap。
    [[nodiscard]] std::optional<Gap> take_pending_gap();

    /// seq 倒退检测: seq < last_applied[acct] → true (数据被重置, spec §5.5)。
    [[nodiscard]] bool detect_reset(const std::string& account_id, uint64_t seq);

    /// 账户重置: 清 last_applied/applied_trades, 设新水位 (调用方重查 DB 得 new_w)。
    void reset_account(const std::string& account_id, uint64_t new_w);

    /// 成交去重二道防线: 键 (account_id, trading_day, trade_id) 存在性。
    /// 新键返回 true; 已存在返回 false。交易日切换清理见 on_trading_day_changed。
    bool admit_trade(const std::string& account_id, const char* trading_day, const char* trade_id);

    /// 交易日切换: 丢弃该账户全部旧日去重段 (集合增长有界, spec §5.4)。
    void on_trading_day_changed(const std::string& account_id, const char* new_trading_day);

private:
    /// 去重段键: account_id + '\x1f' + trading_day (内嵌 day 天然跨日隔离)
    static std::string trade_segment_key(const std::string& account_id, const char* trading_day);

    /// account_id -> {W, last_applied}
    struct AccountState {
        uint64_t watermark = 0;
        bool has_watermark = false;  ///< 是否设过水位 (未设 = 该账户无 DB 快照, 不过滤全放行)
        uint64_t last_applied = 0;
        bool has_last_applied = false;  ///< 是否已见过该账户首帧
    };
    std::unordered_map<std::string, AccountState> accounts_;

    /// 待处理断档 (至多一条)
    std::optional<Gap> pending_gap_;

    /// 成交去重: (account_id, trading_day) 段 -> trade_id 集合
    std::unordered_map<std::string, std::unordered_set<std::string>> trade_segments_;
    /// 各账户最近 admit_trade 的交易日 (admit_trade 检测到变化时自动清旧段)
    std::unordered_map<std::string, std::string> account_days_;
};

}  // namespace dztrader

#endif  // DZTRADER_STRATEGY_API_TD_INGEST_H_
