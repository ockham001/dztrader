#include "td_domain_service.h"

#include "td_orders_query.h"

#include <dztrader/date_time/date.h>
#include <dztrader/struct.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>

namespace dztrader::webui {

namespace {

/// 委托行 WS 事件名（契约 webui-ws §2.5）。
constexpr const char* kOrderReportEvent = "td_order_report";

/// `DzDate`（距纪元天数）→ "YYYYMMDD"；非法回落空串。
std::string date_text(int32_t date) {
    try {
        const dztrader::Date day{date};
        if (day.year() < 1970 || day.year() > 9999) {
            return {};
        }
        return std::format("{:04d}{:02d}{:02d}", day.year(), day.month(), day.day());
    } catch (...) {
        return {};
    }
}

/// ORDER_REPORT 帧 → 契约 rest §2.9.3 委托行内部形态（字段拷贝，无需持 FrameView）。
TdOrderRecord to_record(const DzOrderReport& report) {
    TdOrderRecord row;
    row.account_id = std::string(report.account_id);
    row.date = date_text(report.date);
    row.order_id = report.order_id;
    row.strategy_id = std::string(report.strategy_id);
    row.instrument_id = std::string(report.instrument_id);
    row.exchange_id = std::string(report.exchange_id);
    row.direction = static_cast<int>(report.direction);
    row.position_effect = static_cast<int>(report.position_effect);
    row.price_type = static_cast<int>(report.price_type);
    row.status = static_cast<int>(report.status);
    row.price = report.price;
    row.volume = static_cast<int>(report.volume);
    row.volume_traded = static_cast<int>(report.volume_traded);
    row.remark = std::string(report.remark);
    row.time = static_cast<int>(report.time);
    row.avg_price = report.avg_price;
    row.seq = report.seq;
    return row;
}

}  // namespace

TdDomainService::TdDomainService(FrameRouter& router,
                                 WsBroadcaster& ws,
                                 DataChangeNotifier& notifier)
    : router_(router),
      ws_(ws),
      notifier_(notifier) {
    register_handlers(router_);
}

void TdDomainService::register_handlers(FrameRouter& router) {
    // 交易帧与登录状态帧均为 struct payload（basic 广播帧），必须 register_raw：
    // handler 在监听线程同步执行、FrameView 有效期内解析，值拷贝后经 poster 投递 IO 线程。
    router.register_raw(DZ_FRAME_ORDER_REPORT, [this](const shm::FrameView& view) {
        const DzOrderReport report = view.payload<DzOrderReport>();
        post([this, report]() { on_order_report(report); });
    });
    router.register_raw(DZ_FRAME_ACCOUNT_STATUS, [this](const shm::FrameView& view) {
        const DzAccountStatus status = view.payload<DzAccountStatus>();
        post([this, status]() { on_account_status(status); });
    });
}

void TdDomainService::post(std::function<void()> task) { router_.poster()(std::move(task)); }

void TdDomainService::set_trade_db_path_provider(std::function<std::string()> provider) {
    trade_db_path_ = std::move(provider);
    tick_cache_.clear();
}

void TdDomainService::on_account_status(const DzAccountStatus& status) {
    const std::string account(status.account_id);
    if (account.empty()) {
        return;
    }
    switch (status.state) {
        case DZ_ACCOUNT_READY: {
            const bool was_online = online_date_.find(account) != online_date_.end();
            const std::optional<std::string> previous_day = current_trading_day();
            online_date_[account] = status.date;
            // 登录完成 = td 已装载合约（instruments）；此前的「查不到 tick」负缓存可能
            // 只是在库/合约尚未就绪时拍的快照，必须失效，否则该合约精度长期停在兜底口径。
            tick_cache_.clear();
            // Ready/Offline 翻转、日切、在线集合变化 → 前端清空并重拉基线
            // （契约 webui-ws §2.3 / td-data-sync §8）。
            const std::optional<std::string> current_day = current_trading_day();
            if (!was_online || previous_day != current_day) {
                notifier_.broadcast_data_changed(kTdDataChangedScope);
                SPDLOG_INFO("td online set changed | account={} online={} day={}", account,
                            online_date_.size(), current_day.value_or(""));
            }
            break;
        }
        case DZ_ACCOUNT_OFFLINE:
        case DZ_ACCOUNT_LOGGING_IN:
            // 契约 account-status：Ready = 在线；Offline/LoggingIn 下线。
            // 补发环只在 Offline 清（契约 td-data-sync §8：「Offline 清环」）——
            // LoggingIn 是登录过程中的过渡态，重连成功后环内容仍可用于补发。
            if (online_date_.erase(account) > 0) {
                if (status.state == DZ_ACCOUNT_OFFLINE) {
                    recent_.erase(account);
                }
                notifier_.broadcast_data_changed(kTdDataChangedScope);
                SPDLOG_INFO("td account left online set | account={} state={} online={}", account,
                            status.state == DZ_ACCOUNT_OFFLINE ? "offline" : "logging_in",
                            online_date_.size());
            }
            break;
        default:
            break;
    }
}

void TdDomainService::on_order_report(const DzOrderReport& report) {
    const std::string account(report.account_id);
    // 契约 td-data-sync §8：只有在线（Ready）账户的帧转发为 WS 交易事件。
    // 不在线 = 该帧不属于当前列表口径（可能来自尚未 Ready 的重放），丢弃即可——
    // 登录完成后 Ready 会触发前端重拉 REST 基线。
    if (account.empty() || online_date_.find(account) == online_date_.end()) {
        return;
    }
    const TdOrderRecord record = to_record(report);
    const int decimals = price_decimals_for(record.instrument_id, record.price);
    nlohmann::json row = td_order_row_json(record, decimals);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空（account_id 在 payload 内）。
    ws_.broadcast(kOrderReportEvent, "", row);
    push_recent(account, std::move(row));
}

int TdDomainService::price_decimals_for(const std::string& instrument_id, double price) {
    if (instrument_id.empty()) {
        return td_price_decimals(0.0, price);
    }
    auto it = tick_cache_.find(instrument_id);
    if (it == tick_cache_.end()) {
        // 契约 rest §2.9.6：查不到合约 / 库不可用 → 走价格小数位兜底（缓存负结果，
        // 避免每帧都打库）。
        std::optional<double> tick;
        if (trade_db_path_) {
            const std::string path = trade_db_path_();
            if (!path.empty()) {
                tick = td_query_price_tick(path, /*busy_timeout_ms=*/200, instrument_id);
            }
        }
        it = tick_cache_.emplace(instrument_id, tick).first;
    }
    return td_price_decimals(it->second.value_or(0.0), price);
}

void TdDomainService::push_recent(const std::string& account_id, nlohmann::json row) {
    auto& ring = recent_[account_id];
    if (ring.size() >= kTdRecentCapacityPerAccount) {
        ring.pop_front();
    }
    ring.push_back({{"type", kOrderReportEvent}, {"data", std::move(row)}});
}

nlohmann::json TdDomainService::recent_events() const {
    nlohmann::json events = nlohmann::json::array();
    for (const auto& [account_id, ring] : recent_) {
        // 契约 webui-ws §2.5：内容 = **各在线账户**补发环，按入环先后从旧到新；
        // 不同账户之间顺序不承诺。LoggingIn 期间环保留（重连 Ready 后可继续补发），
        // 但该账户不在线集合 → 不进入本次载荷。
        if (online_date_.find(account_id) == online_date_.end()) {
            continue;
        }
        for (const auto& event : ring) {
            events.push_back(event);
        }
    }
    return {{"events", std::move(events)}};
}

std::vector<std::string> TdDomainService::online_accounts() const {
    std::vector<std::string> accounts;
    accounts.reserve(online_date_.size());
    for (const auto& [account_id, date] : online_date_) {
        (void)date;
        accounts.push_back(account_id);  // std::map 已按 account_id 升序
    }
    return accounts;
}

std::optional<int32_t> TdDomainService::current_trading_day_epoch() const {
    std::optional<int32_t> latest;
    for (const auto& [account_id, date] : online_date_) {
        (void)account_id;
        if (!latest.has_value() || date > *latest) {
            latest = date;
        }
    }
    return latest;
}

std::optional<std::string> TdDomainService::current_trading_day() const {
    const std::optional<int32_t> latest = current_trading_day_epoch();
    if (!latest.has_value()) {
        return std::nullopt;
    }
    std::string text = date_text(*latest);
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

}  // namespace dztrader::webui
