#include "td_domain_service.h"

#include "td_orders_query.h"
#include "td_row_json.h"

#include <dztrader/date_time/date.h>
#include <dztrader/struct.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>

namespace dztrader::webui {

namespace {

/// 交易行 WS 事件名（契约 webui-ws §2.5 四类交易事件；本服务转发委托、成交与持仓）。
constexpr const char* kOrderReportEvent = "td_order_report";
constexpr const char* kTradeReportEvent = "td_trade_report";
constexpr const char* kPositionInfoEvent = "td_position_info";

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

/// TRADE_REPORT 帧 → 契约 rest §2.9.3 成交行内部形态（字段拷贝，无需持 FrameView）。
TdTradeRecord to_trade_record(const DzTradeReport& report) {
    TdTradeRecord row;
    row.account_id = std::string(report.account_id);
    row.date = date_text(report.date);
    row.trade_id = std::string(report.trade_id);
    row.order_id = report.order_id;
    row.strategy_id = std::string(report.strategy_id);
    row.instrument_id = std::string(report.instrument_id);
    row.exchange_id = std::string(report.exchange_id);
    row.direction = static_cast<int>(report.direction);
    row.position_effect = static_cast<int>(report.position_effect);
    row.price = report.price;
    row.volume = static_cast<int>(report.volume);
    row.time = static_cast<int>(report.time);
    row.seq = report.seq;
    return row;
}

/// POSITION_INFO 帧 → 契约 rest §2.9.4 持仓行内部形态（字段拷贝，无需持 FrameView）。
TdPositionRecord to_position_record(const DzPositionInfo& info) {
    TdPositionRecord row;
    row.account_id = std::string(info.account_id);
    row.date = date_text(info.date);
    row.instrument_id = std::string(info.instrument_id);
    row.exchange_id = std::string(info.exchange_id);
    row.direction = static_cast<int>(info.direction);
    row.volume = static_cast<int>(info.volume);
    row.frozen_volume = static_cast<int>(info.frozen_volume);
    row.today_volume = static_cast<int>(info.today_volume);
    row.yd_volume = static_cast<int>(info.yd_volume);
    row.price = info.price;
    row.seq = info.seq;
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
    router.register_raw(DZ_FRAME_TRADE_REPORT, [this](const shm::FrameView& view) {
        const DzTradeReport report = view.payload<DzTradeReport>();
        post([this, report]() { on_trade_report(report); });
    });
    router.register_raw(DZ_FRAME_POSITION_INFO, [this](const shm::FrameView& view) {
        const DzPositionInfo info = view.payload<DzPositionInfo>();
        post([this, info]() { on_position_info(info); });
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

void TdDomainService::set_stats_query(std::shared_ptr<TdStatsQuery> query) {
    stats_query_ = std::move(query);
    // 注入后立即重算一次（契约 rest §2.9.5「启动」触发点）。不放在 ctor：ctor 时查询
    // 尚未注入，只会得到 not_ready。
    reconcile_stats();
}

void TdDomainService::set_reconcile_scheduler(
    std::function<void(double interval, std::function<void()> task)> scheduler) {
    if (!scheduler) {
        return;
    }
    // 契约 rest §2.9.5：周期对账（建议 30s，实现可调）。任务即全部重算路径的唯一入口。
    scheduler(kTdStatsReconcileIntervalSec, [this]() { reconcile_stats(); });
}

void TdDomainService::reconcile_stats() {
    if (!stats_query_) {
        // 未装配统计查询（API-only / 测试）：统计恒 not_ready，不影响委托列表。
        stats_ready_ = false;
        return;
    }
    const auto snapshot = stats_query_->recompute(online_accounts(), current_trading_day());
    if (!snapshot.has_value()) {
        // 契约 rest §2.9.5：计数器就绪后即使交易库暂时不可读也不降级——保持既有计数器
        // 与 ready 状态，等下一次对账。
        return;
    }
    stats_.reset(*snapshot);
    stats_ready_ = true;
}

nlohmann::json TdDomainService::stats_body() const {
    return td_stats_body(stats_ready_, current_trading_day(), online_accounts(), stats_.values());
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
            // 契约 rest §2.9.5 重算触发点：账户 Ready——**无条件**，不跟 data_changed 的
            // 「有变化才广播」条件合并。Ready 是**重同步点**而非优化机会：td 崩溃重启时
            // 来不及推 Offline，重登后 `was_online` 仍为 true、日切也未发生，若按「有变化才
            // 重算」就会漏掉这次重同步，让计数器停留在重启前的旧值直到下一次周期对账
            // （测试锁定：RepeatedReadyResyncsBecauseTdRestartSkipsOffline）。
            reconcile_stats();
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
                // 契约 rest §2.9.5 重算触发点：在线集合变化（含 Offline / LoggingIn 下线）。
                // 无条件不是必需的：账户确实离开了在线集合才有口径变化，重复下线帧无害
                // 但也无需重算（口径未变）。
                reconcile_stats();
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
    // 契约 rest §2.9.5 增量来源 + td-data-sync §2 第 4 条：跳过判断只准用**重算查得的 DB 水位**
    // （`seq <= watermark` 的帧已由重算纳入；登录「重推最后一条」用原 seq，靠这条避免重复计数）。
    // 注意 apply 不推进水位——水位只由 reconcile_stats() 的全量重算写入。
    if (report.seq > stats_.watermark(account)) {
        stats_.apply(account, static_cast<int>(report.status), report.order_id,
                     static_cast<int>(report.volume), static_cast<int>(report.volume_traded));
    }
    const TdOrderRecord record = to_record(report);
    const int decimals = price_decimals_for(record.instrument_id, record.price);
    nlohmann::json row = td_order_row_json(record, decimals);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空（account_id 在 payload 内）。
    ws_.broadcast(kOrderReportEvent, "", row);
    push_recent(account, kOrderReportEvent, std::move(row));
}

void TdDomainService::on_trade_report(const DzTradeReport& report) {
    const std::string account(report.account_id);
    // 契约 td-data-sync §8：只有在线（Ready）账户的帧转发为 WS 交易事件。
    // 不在线 = 该帧不属于当前列表口径（可能来自尚未 Ready 的重放），丢弃即可——
    // 登录完成后 Ready 会触发前端重拉 REST 基线。
    if (account.empty() || online_date_.find(account) == online_date_.end()) {
        return;
    }
    // 成交不计入统计计数器（契约 rest §2.9.5：4 值全部来自**委托**行），因此这里
    // 不碰 stats_；统计只由 ORDER_REPORT 增量与全量重算推进。
    const TdTradeRecord record = to_trade_record(report);
    const int decimals = price_decimals_for(record.instrument_id, record.price);
    nlohmann::json row = td_trade_row_json(record, decimals);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空（account_id 在 payload 内）。
    ws_.broadcast(kTradeReportEvent, "", row);
    // 契约 td-data-sync §8：同一账户环同时容纳委托与成交事件，容量共享。
    push_recent(account, kTradeReportEvent, std::move(row));
}

void TdDomainService::on_position_info(const DzPositionInfo& info) {
    const std::string account(info.account_id);
    // 契约 td-data-sync §8：只有在线（Ready）账户的帧转发为 WS 交易事件。
    // 持仓帧在登录查询阶段产生（Ready 之前）——那些帧不进补发环，前端靠 Ready 后的
    // REST 基线拿到同一批数据；盘中增量帧（Ready 之后）正常转发。
    if (account.empty() || online_date_.find(account) == online_date_.end()) {
        return;
    }
    // 持仓不参与统计计数器（契约 rest §2.9.5：4 值全部来自委托行），因此这里不碰 stats_。
    const TdPositionRecord record = to_position_record(info);
    const int decimals = price_decimals_for(record.instrument_id, record.price);
    nlohmann::json row = td_position_row_json(record, decimals);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空（account_id 在 payload 内）。
    ws_.broadcast(kPositionInfoEvent, "", row);
    // 契约 td-data-sync §8：同一账户环同时容纳委托/成交/持仓事件，容量共享。
    push_recent(account, kPositionInfoEvent, std::move(row));
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

void TdDomainService::push_recent(const std::string& account_id,
                                  const char* type,
                                  nlohmann::json row) {
    auto& ring = recent_[account_id];
    if (ring.size() >= kTdRecentCapacityPerAccount) {
        ring.pop_front();
    }
    ring.push_back({{"type", type}, {"data", std::move(row)}});
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
