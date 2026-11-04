#include "td/td_persist_rows.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <dztrader/data_type.h>
#include <dztrader/tdstore/records_store.h>

namespace dztrader::ctp {

namespace {

/// 外部单/手工单/同步单用保留策略标识表达（不再有 is_external 列；ADR 0041 §1）。
std::string order_strategy_id(const OrderRecord& r) {
    if (r.is_external != 0 && r.base.strategy_id[0] == '\0') {
        return std::string(DZ_STRATEGY_ID_EXTERNAL);
    }
    return std::string(r.base.strategy_id);
}

/// CTP 扩展字段 -> ext JSON（ADR 0041 §1：柜台差异不进列；登录日装载/重放过滤读 ext）。
std::string build_order_ext(const OrderRecord& r) {
    nlohmann::json ext = nlohmann::json::object();
    if (r.order_ref[0] != '\0') {
        ext[kOrderExtOrderRef] = std::string(r.order_ref);
    }
    if (r.external_order_id[0] != '\0') {
        ext[kOrderExtOrderSysId] = std::string(r.external_order_id);
    }
    if (r.volume_canceled != 0) {
        ext[kOrderExtVolumeCanceled] = r.volume_canceled;
    }
    if (r.update_time != 0) {
        ext[kOrderExtUpdateTime] = r.update_time;
    }
    if (r.error_id != 0) {
        ext[kOrderExtErrorId] = r.error_id;
    }
    if (r.error_msg[0] != '\0') {
        ext[kOrderExtErrorMessage] = std::string(r.error_msg);
    }
    return ext.dump();
}

}  // namespace

dztrader::db::Row make_order_row(const OrderRecord& r, std::string_view gateway) {
    // 列序 = tdstore::orders_schema()（schema_catalog.h）
    std::vector<dztrader::db::Value> v;
    v.reserve(19);
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.account_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.trading_day)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.order_id)));
    v.push_back(dztrader::tdstore::to_value(order_strategy_id(r)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.instrument_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.exchange_id)));
    v.push_back(dztrader::tdstore::to_value(r.base.direction));
    v.push_back(dztrader::tdstore::to_value(r.base.position_effect));
    v.push_back(dztrader::tdstore::to_value(r.base.price_type));
    v.push_back(dztrader::tdstore::to_value(r.base.status));
    v.push_back(dztrader::tdstore::to_value(r.base.price));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume_traded)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.remark)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.time)));
    v.push_back(dztrader::tdstore::to_value(r.base.avg_price));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.seq)));
    v.push_back(dztrader::tdstore::to_value(gateway));
    v.push_back(dztrader::tdstore::to_value(build_order_ext(r)));
    return dztrader::db::Row{std::move(v)};
}

dztrader::db::Row make_trade_row(const TradeRecord& r, std::string_view gateway) {
    // 列序 = tdstore::trades_schema()（schema_catalog.h）
    std::vector<dztrader::db::Value> v;
    v.reserve(15);
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.account_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.trading_day)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.trade_id)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.order_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.strategy_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.instrument_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.exchange_id)));
    v.push_back(dztrader::tdstore::to_value(r.base.direction));
    v.push_back(dztrader::tdstore::to_value(r.base.position_effect));
    v.push_back(dztrader::tdstore::to_value(r.base.price));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.time)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.seq)));
    v.push_back(dztrader::tdstore::to_value(gateway));
    v.push_back(dztrader::tdstore::to_value(std::string_view("{}")));  // ext: CTP 无逐笔扩展
    return dztrader::db::Row{std::move(v)};
}

}  // namespace dztrader::ctp
