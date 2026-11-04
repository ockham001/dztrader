#include "td_stats.h"

#include "td_read_db.h"

#include <dztrader/data_type.h>

#include <string>
#include <utility>

namespace dztrader::webui {

bool td_status_unfilled(int status) {
    return status == DZ_ORDER_SUBMITTING || status == DZ_ORDER_NOT_TRADED ||
           status == DZ_ORDER_PART_TRADED;
}

void TdStatsCounter::reset(const TdStatsSnapshot& snapshot) {
    values_ = snapshot.values;
    active_.clear();
    for (const TdStatsActiveOrder& order : snapshot.active) {
        // 快照的活动表由查询按未成交状态过滤；此处再守一道，避免非未成交行混入
        // （活动表的唯一判据是 td_status_unfilled，不在别处复制 status 集合）。
        if (!td_status_unfilled(order.status)) {
            continue;
        }
        active_[order.account_id][order.order_id] = ActiveEntry{
            .status = order.status, .volume = order.volume, .volume_traded = order.volume_traded};
    }
    watermarks_ = snapshot.watermarks;
}

void TdStatsCounter::apply(
    const std::string& account_id, int status, int64_t order_id, int volume, int volume_traded) {
    const bool unfilled = td_status_unfilled(status);
    auto account_it = active_.find(account_id);
    if (account_it != active_.end()) {
        auto entry_it = account_it->second.find(order_id);
        if (entry_it != account_it->second.end()) {
            // 已在活动表 ⇒ 该委托已计入 total：只调整未成交口径的差量。
            // 旧贡献 = 旧剩余量（活动表条目按定义属于未成交集合）；新贡献 = 未成交 ? 新剩余量 : 0。
            // 注意离开集合时必须扣掉**旧**剩余量（新帧的 volume_traded 可能已含成交），
            // 用「新剩余量」会在「部分成交后撤单」时留下幽灵剩余量。
            const ActiveEntry& previous = entry_it->second;
            const int64_t previous_remaining =
                static_cast<int64_t>(previous.volume) - previous.volume_traded;
            const int64_t next_remaining = static_cast<int64_t>(volume) - volume_traded;
            values_.unfilled_volume +=
                (unfilled ? next_remaining : int64_t{0}) - previous_remaining;
            if (previous.status != status) {
                values_.unfilled_orders += (unfilled ? 1 : 0) - 1;
            }
            if (unfilled) {
                entry_it->second =
                    ActiveEntry{.status = status, .volume = volume, .volume_traded = volume_traded};
            } else {
                account_it->second.erase(entry_it);
                if (account_it->second.empty()) {
                    active_.erase(account_it);
                }
            }
            return;
        }
    }

    // 不在活动表 ⇒ 新委托（契约 rest §2.9.5：total 含撤单、废单）。
    values_.total_orders += 1;
    values_.total_volume += volume;
    if (!unfilled) {
        return;
    }
    values_.unfilled_orders += 1;
    values_.unfilled_volume += static_cast<int64_t>(volume) - volume_traded;
    active_[account_id][order_id] =
        ActiveEntry{.status = status, .volume = volume, .volume_traded = volume_traded};
}

void TdStatsCounter::clear() {
    values_ = TdStatsValues{};
    active_.clear();
    watermarks_.clear();
}

const TdStatsValues& TdStatsCounter::values() const noexcept { return values_; }

uint64_t TdStatsCounter::watermark(const std::string& account_id) const {
    const auto it = watermarks_.find(account_id);
    return it == watermarks_.end() ? 0 : it->second;
}

nlohmann::json td_stats_body(bool ready,
                             const std::optional<std::string>& trading_day,
                             const std::vector<std::string>& online_accounts,
                             const TdStatsValues& values) {
    if (!ready) {
        // 契约 rest §2.9.5：唯一降级是计数器未就绪（与 §2.9.8 的库降级区分）。
        return td_degraded_body(kTdReasonNotReady);
    }
    return {
        {"available", true},
        {"trading_day",
         trading_day.has_value() ? nlohmann::json(*trading_day) : nlohmann::json(nullptr)},
        {"online_accounts", online_accounts},
        {"stats",
         {{"total_orders", values.total_orders},
          {"total_volume", values.total_volume},
          {"unfilled_orders", values.unfilled_orders},
          {"unfilled_volume", values.unfilled_volume}}},
    };
}

}  // namespace dztrader::webui
