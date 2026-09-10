#ifndef DZTRADER_CTP_TD_REPORT_FILTER_H_
#define DZTRADER_CTP_TD_REPORT_FILTER_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "td/td_schema.h"

namespace dztrader::ctp {

namespace detail {

/// 成交键 (trading_day, trade_id) 的哈希器.
/// 注: 本工具链 libstdc++ 未提供 std::hash<std::pair>, 故自定义.
struct TradeKeyHash {
    size_t operator()(const std::pair<std::string, std::string>& key) const noexcept {
        return std::hash<std::string>{}(key.first) ^ (std::hash<std::string>{}(key.second) << 1);
    }
};

}  // namespace detail

/// 重放过滤器: 登录/重连时 CTP 私有流重放去重 (设计 §4.1).
///
/// 基准 = 该账户 DB 全量装载 (load) 或运行期逐条接受 (accept_order/accept_trade) 的快照.
/// 对比键:
/// - 委托: order_id 定位基准行, 对比集 {update_time, volume_traded, volume_canceled, status}.
///   完全相等 → kSkip (重放重复); 不相等且 rec.update_time < base.update_time → kSkip + 过期告警 (防御);
///   否则 → kForward (含同秒 status/volume 翻转, 必须转发).
/// - 成交: 键 (trading_day, trade_id) 存在性 (跨日同 trade_id 必须 kForward).
///
/// check_* 不更新基准 (转发与否由调用方决定后调 accept_*); check_* 不修改对象状态.
class ReportFilter {
public:
    enum class Verdict {
        kForward,  ///< 应转发 (新数据 / 同秒翻转)
        kSkip,     ///< 应吞 (重复 / 过期防御)
    };

    /// 构造空基准过滤器.
    explicit ReportFilter(std::string account_id);

    /// 从 DB 全量装载构造基准 (orders/trades 为该账户全部历史行).
    /// 装载即视为已 accept: 重放同一批行全 kSkip.
    static ReportFilter load(const std::vector<OrderRecord>& orders,
                             const std::vector<TradeRecord>& trades);

    /// 委托去重: 按 order_id 定位基准行, 对比集 {update_time, volume_traded, volume_canceled, status}.
    /// 完全相等 → kSkip; 不相等但 rec.update_time < base.update_time → 设 *outdated_warn=true 且 kSkip;
    /// 否则 kForward. 不更新基准. 基准无该 order_id → kForward.
    [[nodiscard]] Verdict check_order(const OrderRecord& rec, bool* outdated_warn = nullptr) const;

    /// 更新委托基准 (转发路径调用, 主线程即时更新).
    void accept_order(const OrderRecord& rec);

    /// 成交去重: 键 (trading_day, trade_id) 存在性. 命中 → kSkip, 否则 kForward.
    [[nodiscard]] Verdict check_trade(const TradeRecord& rec) const;

    /// 更新成交基准 (转发路径调用).
    void accept_trade(const TradeRecord& rec);

    /// 最近 accept 的委托 (登录完成协议"最后一条重推", Task 6).
    /// 基准为空返回 nullptr.
    [[nodiscard]] const DzOrderReport* find_latest_order() const noexcept;

    /// 最近 accept 的成交. 基准为空返回 nullptr.
    [[nodiscard]] const DzTradeReport* find_latest_trade() const noexcept;

    /// 指定交易日的活动委托 (base.date == trading_day 且非终态).
    /// 终态 = ALL_TRADED/CANCELLED/REJECTED; 无排序保证 (Task 5 种入用).
    [[nodiscard]] std::vector<OrderRecord> active_orders(int32_t trading_day) const;

    /// 对比命中 (kSkip) 时回填钩子 (Task 5 据此转发给 order_ref_map_ 修复).
    /// 本任务内为占位: kSkip 时 rec 已带正确 order_id, 无对外状态变更.
    void replay_hit_order(const OrderRecord& rec);

    const std::string& account_id() const noexcept { return account_id_; }

private:
    /// 是否字段集完全相等 (对比集 {update_time, volume_traded, volume_canceled, status}).
    static bool same_order_fields(const OrderRecord& a, const OrderRecord& b) noexcept;

    std::string account_id_;
    /// order_id -> 基准委托行.
    std::unordered_map<int64_t, OrderRecord> orders_;
    /// 成交键集: (trading_day, trade_id).
    std::unordered_set<std::pair<std::string, std::string>, detail::TradeKeyHash> trade_keys_;
    /// 最近 accept 的委托/成交 (SHM 帧格式, 供 find_latest_* 返回).
    DzOrderReport latest_order_{};
    DzTradeReport latest_trade_{};
    bool has_order_ = false;
    bool has_trade_ = false;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_REPORT_FILTER_H_
