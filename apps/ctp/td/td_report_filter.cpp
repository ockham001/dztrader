#include "td/td_report_filter.h"

#include <string>
#include <utility>

namespace dztrader::ctp {

ReportFilter::ReportFilter(std::string account_id) : account_id_(std::move(account_id)) {}

ReportFilter ReportFilter::load(const std::vector<OrderRecord>& orders,
                                const std::vector<TradeRecord>& trades) {
    ReportFilter f{""};
    for (const auto& o : orders) {
        f.accept_order(o);
    }
    for (const auto& t : trades) {
        f.accept_trade(t);
    }
    return f;
}

bool ReportFilter::same_order_fields(const OrderRecord& a, const OrderRecord& b) noexcept {
    return a.update_time == b.update_time && a.base.volume_traded == b.base.volume_traded &&
           a.volume_canceled == b.volume_canceled && a.base.status == b.base.status;
}

ReportFilter::Verdict ReportFilter::check_order(const OrderRecord& rec,
                                                bool* outdated_warn) const {
    if (outdated_warn) {
        *outdated_warn = false;
    }
    auto it = orders_.find(rec.base.order_id);
    if (it == orders_.end()) {
        return Verdict::kForward;  // order_id 不在基准 → 新单转发
    }
    const OrderRecord& base = it->second;
    if (same_order_fields(rec, base)) {
        return Verdict::kSkip;
    }
    if (rec.update_time < base.update_time) {
        if (outdated_warn) {
            *outdated_warn = true;
        }
        return Verdict::kSkip;  // 时间回退 = 异常旧数据, 防御吞
    }
    return Verdict::kForward;  // update_time 相等或更新且字段不等 → 转发 (同秒翻转)
}

void ReportFilter::accept_order(const OrderRecord& rec) {
    orders_[rec.base.order_id] = rec;
    latest_order_ = rec.base;
    has_order_ = true;
}

ReportFilter::Verdict ReportFilter::check_trade(const TradeRecord& rec) const {
    auto key = std::make_pair(std::string(rec.trading_day), std::string(rec.base.trade_id));
    return trade_keys_.find(key) != trade_keys_.end() ? Verdict::kSkip : Verdict::kForward;
}

void ReportFilter::accept_trade(const TradeRecord& rec) {
    trade_keys_.emplace(rec.trading_day, rec.base.trade_id);
    latest_trade_ = rec.base;
    has_trade_ = true;
}

const DzOrderReport* ReportFilter::find_latest_order() const noexcept {
    return has_order_ ? &latest_order_ : nullptr;
}

const DzTradeReport* ReportFilter::find_latest_trade() const noexcept {
    return has_trade_ ? &latest_trade_ : nullptr;
}

std::vector<OrderRecord> ReportFilter::active_orders(int32_t trading_day) const {
    std::vector<OrderRecord> result;
    for (const auto& [id, rec] : orders_) {
        if (rec.base.date != trading_day) continue;
        if (rec.base.status == DZ_ORDER_ALL_TRADED || rec.base.status == DZ_ORDER_CANCELLED ||
            rec.base.status == DZ_ORDER_REJECTED) {
            continue;
        }
        result.push_back(rec);
    }
    return result;
}

void ReportFilter::replay_hit_order(const OrderRecord& rec) {
    // Task 4 内仅作命中追踪占位 (本类无对外状态变更);
    // Task 5 在此转发给 order_ref_map_ 修复 (kSkip 时 rec 已带正确 order_id).
    (void)rec;
}

}  // namespace dztrader::ctp
