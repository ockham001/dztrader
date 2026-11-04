#ifndef DZTRADER_WEBUI_TD_STATS_H_
#define DZTRADER_WEBUI_TD_STATS_H_

/**
 * @file td_stats.h
 * @brief dzweb 内存统计计数器（「未成交 / 总」4 值，契约 rest §2.9.5）。
 *
 * 纯逻辑：无 DB、无锁、无线程，全部在 dzweb IO 线程使用（与 REST/WS 连接回调同线程串行）。
 *
 * 状态只有两样（ADR 0043 决策 1/4，契约 td-data-sync §8）：
 * - 4 个计数器值；
 * - 「活动（未成交）委托表」+ 每账户 `seq` 水位。
 *
 * 未成交集合**有界且单向**（`Submitting → NotTraded → PartTraded → {AllTraded|Cancelled}`，
 * `Rejected` 也是终态，终态不回退），终态委托不会再收到 `ORDER_REPORT`，因此不需要为终态
 * 保留任何去重状态：`order_id` 在活动表 ⇒ 已计入 total；不在 ⇒ 视为新委托。
 *
 * 水位只由重算（`reset`）写入 DB 查得的每账户四表 `MAX(seq)`，增量 `apply` **不推进水位**
 * ——「跳过判断只准用 DB 查得的水位 W，永不使用内存见过的最大 seq」（契约 td-data-sync §2
 * 第 4 条 / §5）。帧 `seq ≤ 水位` 由调用方跳过（登录「重推最后一条」用原 seq，靠这条避免
 * 重复计数；安全性来自契约 td-data-sync §2 的 seq 前缀不变量）。
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dztrader::webui {

/// 统计重算/对账周期（契约 rest §2.9.5「建议 30s，实现可调」）。
inline constexpr double kTdStatsReconcileIntervalSec = 30.0;

/// 4 个计数器值（契约 rest §2.9.5）。
struct TdStatsValues {
    int64_t total_orders = 0;     ///< 当前交易日在线账户的全部委托笔数（含撤单、废单）
    int64_t total_volume = 0;     ///< 上述委托的 volume 之和
    int64_t unfilled_orders = 0;  ///< status ∈ 未成交集合的笔数
    int64_t unfilled_volume = 0;  ///< 上述委托的 (volume − volume_traded) 之和
};

/// 一条活动（未成交）委托的重算种子。
struct TdStatsActiveOrder {
    std::string account_id;
    int64_t order_id = 0;
    int status = 0;  ///< DzOrderStatus
    int volume = 0;
    int volume_traded = 0;
};

/// 重算快照（`TdStatsQuery` 产出，`TdStatsCounter::reset` 消费）。
struct TdStatsSnapshot {
    TdStatsValues values;
    std::vector<TdStatsActiveOrder> active;
    std::map<std::string, uint64_t> watermarks;  ///< account_id -> 四表 MAX(seq)
};

/// 未成交状态判定（契约 rest §2.9.5：Submitting / NotTraded / PartTraded）。
///
/// **唯一实现**——禁止在别处再写一份 status 集合。需要枚举集合的场景（如重算 SQL 的
/// `IN (?,?,?)` 占位符）用 {@link td_unfilled_statuses}，它与本函数同源：两者都由
/// `td_stats.cpp` 里的同一张表派生，且有无遗漏由 `td_stats_test.cpp` 的穷举用例锁定。
bool td_status_unfilled(int status);

/// 未成交状态集合（契约 rest §2.9.5），供 SQL 占位符绑定用。
///
/// 与 {@link td_status_unfilled} **同源**（同一张表），不是第二份定义：本函数返回的
/// 每个元素都让 `td_status_unfilled` 为真，反之为假——`td_stats_test.cpp` 穷举
/// `DzOrderStatus` 全部取值断言二者等价。
std::span<const int> td_unfilled_statuses();

/// dzweb 内存统计计数器（只在 IO 线程使用，无锁）。
class TdStatsCounter {
public:
    /// 全量重算结果覆盖（启动 / Ready / 在线集合变化 / 日切 / 周期对账）。
    void reset(const TdStatsSnapshot& snapshot);

    /// 一条 `ORDER_REPORT` 增量（调用方已按水位过滤；仅在线账户）。
    ///
    /// 活动表内的委托只做「旧剩余量 → 新剩余量」的差量；不在活动表的一律视为新委托
    /// （`total_orders += 1`、`total_volume += volume`），终态新委托只计 total 不入活动表。
    /// 同一状态重复应用 delta = 0（幂等）。
    void apply(
        const std::string& account_id, int status, int64_t order_id, int volume, int volume_traded);

    [[nodiscard]] const TdStatsValues& values() const noexcept;

    /// 该账户的四表 `MAX(seq)`；无记录 = 0。
    [[nodiscard]] uint64_t watermark(const std::string& account_id) const;

private:
    /// 活动表条目（只存未成交委托）。
    struct ActiveEntry {
        int status = 0;
        int volume = 0;
        int volume_traded = 0;
    };

    TdStatsValues values_;
    /// account_id -> order_id -> 活动条目。
    std::map<std::string, std::map<int64_t, ActiveEntry>> active_;
    /// account_id -> 四表 MAX(seq)（只由 reset 写入）。
    std::map<std::string, uint64_t> watermarks_;
};

/// `GET /api/td/stats` 响应体（契约 rest §2.9.5）：`ready == false` →
/// `{"available":false,"reason":"not_ready"}`；否则
/// `{available,trading_day,online_accounts,stats:{4 值}}`。
nlohmann::json td_stats_body(bool ready,
                             const std::optional<std::string>& trading_day,
                             const std::vector<std::string>& online_accounts,
                             const TdStatsValues& values);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_STATS_H_
