#include "td/td_persist_rows.h"

#include <string_view>
#include <utility>
#include <vector>

#include <dztrader/tdstore/records_store.h>

namespace dztrader::ctp {

dztrader::db::Row make_order_row(const OrderRecord& r) {
    // 字段序 = tdstore orders_schema (schema_catalog.cpp); 枚举字符按整数码绑定保持现状.
    std::vector<dztrader::db::Value> v;
    v.reserve(24);
    v.push_back(dztrader::tdstore::null_value());  // id (自增)
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.account_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.trading_day)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.order_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.order_ref)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.external_order_id)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.is_external)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.instrument_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.exchange_id)));
    v.push_back(dztrader::tdstore::to_value(r.base.direction));
    v.push_back(dztrader::tdstore::to_value(r.base.position_effect));
    v.push_back(dztrader::tdstore::to_value(r.base.price_type));
    v.push_back(dztrader::tdstore::to_value(r.base.status));
    v.push_back(dztrader::tdstore::to_value(r.base.price));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume_traded)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.volume_canceled)));
    v.push_back(dztrader::tdstore::to_value(r.insert_time));
    v.push_back(dztrader::tdstore::to_value(r.update_time));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.error_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.error_msg)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.strategy_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.remark)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.seq)));
    return dztrader::db::Row{std::move(v)};
}

dztrader::db::Row make_trade_row(const TradeRecord& r) {
    // 字段序 = tdstore trades_schema (schema_catalog.cpp); 枚举字符按整数码绑定保持现状.
    std::vector<dztrader::db::Value> v;
    v.reserve(16);
    v.push_back(dztrader::tdstore::null_value());  // id (自增)
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.account_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.trading_day)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.trade_id)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.order_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.instrument_id)));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.exchange_id)));
    v.push_back(dztrader::tdstore::to_value(r.base.direction));
    v.push_back(dztrader::tdstore::to_value(r.base.position_effect));
    v.push_back(dztrader::tdstore::to_value(r.base.price));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.volume)));
    v.push_back(dztrader::tdstore::to_value(r.trade_time));
    v.push_back(dztrader::tdstore::to_value(r.trade_date));
    v.push_back(dztrader::tdstore::to_value(r.commission));
    v.push_back(dztrader::tdstore::to_value(std::string_view(r.base.strategy_id)));
    v.push_back(dztrader::tdstore::to_value(static_cast<int64_t>(r.base.seq)));
    return dztrader::db::Row{std::move(v)};
}

}  // namespace dztrader::ctp
