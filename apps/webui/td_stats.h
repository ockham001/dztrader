#ifndef DZTRADER_WEBUI_TD_STATS_H_
#define DZTRADER_WEBUI_TD_STATS_H_

/**
 * @file td_stats.h
 * @brief dzweb 内存统计计数器（「未成交 / 总」4 值，契约 rest §2.9.5）。
 *
 * 纯逻辑：无 DB、无锁、无线程，全部在 dzweb IO 线程使用（与 REST/WS 连接回调同线程串行）。
 *
 * 状态只有三样（ADR 0043 决策 1/4 / ADR 0050，契约 td-data-sync §8）：
 * - 4 个计数器值；
 * - 「活动（未成交）委托表」；
 * - 每账户两个水位：`seq` 重放守卫（四表 `MAX(seq)`）与 `order_id` 首见判据（`MAX(order_id)`）。
 *
 * 未成交集合**有界且单向**（`Submitting → NotTraded → PartTraded → {AllTraded|Cancelled}`，
 * `Rejected` 也是终态，终态不回退），活动表只承担「未成交口径」的增减。**是否首见不能由
 * 活动表推断**：已终态委托仍会收到 `ORDER_REPORT`——每笔成交使均价变化时 td 以新 `seq`
 * 刷新该委托并再推一帧（契约 td-data-sync §3），此时它不在活动表，旧判据会把它当成新委托
 * 多计（ADR 0050 要消除的真实计算错误）。首见判据 = 每账户 `order_id` 单调水位：
 * `order_id > max_order_id` ⇒ 真新委托（计入 total / volume，水位推进）；否则 ⇒ 见过的
 * 委托（状态迁移 / 均价刷新），只按活动表规则调整未成交口径。`order_id` 由共享内存全局
 * 计数器单调分配、重启按交易库 `MAX(order_id)+1` 播种，同一委托的后续帧必然复用 ≤ 水位的值。
 *
 * `seq` 水位只由重算（`reset`）写入 DB 查得的每账户四表 `MAX(seq)`，增量 `apply` **不推进
 * 水位**——「跳过判断只准用 DB 查得的水位 W，永不使用内存见过的最大 seq」（契约 td-data-sync
 * §2 第 4 条 / §5）。帧 `seq ≤ 水位` 由调用方跳过（登录「重推最后一条」用原 seq，靠这条避免
 * 重复计数；安全性来自契约 td-data-sync §2 的 seq 前缀不变量）。
 *
 * `max_order_id` 与 `seq` 水位同型：初始值只由重算从 DB `MAX(order_id)` **覆盖**（不能用内存
 * 最大值代替，否则 DB 快照没纳入的未落库委托在重算后会被判成「已见过」而永久丢计），增量
 * 只对真新委托推进（账户无该条目时从 0 起）。判据还隐含「新委托首帧的到达顺序与 `order_id`
 * 分配顺序一致」这一常态前提：本地单在下单时取号、首帧要等 td 收到柜台回报才产生，多策略
 * 跨进程或本地/外部单并发造成的倒序窗口见 ADR 0050（已知少计边界）。
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dztrader::webui {

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
    std::map<std::string, uint64_t> watermarks;    ///< account_id -> 四表 MAX(seq)
    std::map<std::string, int64_t> max_order_ids;  ///< account_id -> orders 表 MAX(order_id)
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
    /// 全量重算结果覆盖（启动 / 账户 Ready / 在线集合变化 / 日切）。
    /// 同时重建活动表与每账户两个水位（`seq` / `order_id`）。
    void reset(const TdStatsSnapshot& snapshot);

    /// 一条 `ORDER_REPORT` 增量（调用方已按 `seq` 水位过滤；仅在线账户）。
    ///
    /// 首见判据 = 每账户 `order_id` 单调水位（ADR 0050）：`order_id > max_order_id` ⇒ 真新
    /// 委托（`total_orders += 1`、`total_volume += volume`，终态只计 total 不入活动表）；
    /// 否则 ⇒ 见过的委托（状态迁移 / 成交均价刷新帧），只按活动表规则调整未成交口径。
    /// 活动表内的委托只做「旧剩余量 → 新剩余量」的差量；不在活动表的已知委托（终态均价
    /// 刷新）无口径变化。同一状态重复应用 delta = 0（幂等）。
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
    /// account_id -> orders 表 MAX(order_id)（reset 从 DB 建立，增量只为真新委托推进）。
    std::map<std::string, int64_t> max_order_ids_;
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
