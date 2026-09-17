#include <dztrader/tdstore/records_store.h>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::tdstore {

namespace {

const dztrader::db::FieldSchema* find_instrument_field(std::string_view name) {
    for (const dztrader::db::FieldSchema& field : instruments_schema().fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

}  // namespace

dztrader::db::Value null_value() { return std::monostate{}; }

dztrader::db::Value to_value(std::string_view text) { return std::string(text); }

dztrader::db::Value to_value(int32_t value) { return static_cast<int64_t>(value); }

dztrader::db::Value to_value(int64_t value) { return value; }

dztrader::db::Value to_value(double value) { return value; }

dztrader::db::Value to_value(char value) {
    return int64_t{static_cast<unsigned char>(value)};
}

db::Row make_instrument_row(const InstrumentRecord& r) {
    return db::Row{std::vector<db::Value>{
        to_value(std::string_view(r.instrument_id)),
        to_value(std::string_view(r.exchange_id)),
        to_value(std::string_view(r.symbol)),
        to_value(std::string_view(r.name)),
        to_value(r.product_class),
        to_value(r.settle_cycle),
        to_value(int32_t{0}),  // settlement_method: 历史列默认值
        to_value(r.is_inverse),
        to_value(std::string_view(r.currency)),
        to_value(std::string_view(r.base_asset)),
        to_value(r.min_limit_order_volume),
        to_value(r.max_limit_order_volume),
        to_value(r.volume_multiple),
        to_value(r.price_tick),
        to_value(r.volume_step),
        to_value(r.listed_date),
        to_value(r.delisted_date),
        to_value(r.option_type),
        to_value(int32_t{0}),  // option_exercise_style: 历史列默认值
        to_value(std::string_view(r.underlying_id)),
        to_value(r.option_strike),
        to_value(std::string_view("")),  // option_series: 历史列默认值
        to_value(std::string_view(r.update_day)),
        to_value(std::string_view(r.product_code)),
        to_value(r.min_market_order_volume),
        to_value(r.max_market_order_volume),
        to_value(r.underlying_multiple),
        to_value(r.updated_at),
    }};
}

db::Row make_position_row(const DzPositionInfo& r, std::string_view trading_day) {
    return db::Row{std::vector<db::Value>{
        to_value(std::string_view(r.account_id)),
        to_value(trading_day),
        to_value(std::string_view(r.instrument_id)),
        to_value(std::string_view(r.exchange_id)),
        to_value(r.direction),  // int8_t 提升 -> int32_t 重载（'0' -> 48）
        to_value(r.volume),
        to_value(r.frozen_volume),
        to_value(r.today_volume),
        to_value(r.yd_volume),
        to_value(r.price),
        to_value(static_cast<int64_t>(r.seq)),
    }};
}

db::Row make_trading_account_row(const DzTradingAccount& r, std::string_view trading_day) {
    return db::Row{std::vector<db::Value>{
        to_value(std::string_view(r.account_id)),
        to_value(trading_day),
        to_value(r.balance),
        to_value(r.available),
        to_value(r.frozen),
        to_value(r.commission),
        to_value(r.margin),
        to_value(r.withdraw_quota),
        to_value(r.deposit),
        to_value(r.withdraw),
        to_value(static_cast<int64_t>(r.seq)),
    }};
}

void upsert_instruments(dztrader::db::Session& session,
                        std::span<const InstrumentRecord> records) {
    std::vector<db::Row> rows;
    rows.reserve(records.size());
    for (const InstrumentRecord& record : records) {
        rows.push_back(make_instrument_row(record));
    }
    session.upsert("instruments", rows);
}

const std::vector<std::string>& instrument_promised_fields() {
    static const std::vector<std::string> kFields = {
        "instrument_id",           "exchange_id",              "symbol",
        "name",                    "product_class",            "product_code",
        "settle_cycle",            "currency",                 "base_asset",
        "is_inverse",              "volume_multiple",          "volume_step",
        "price_tick",              "min_limit_order_volume",   "max_limit_order_volume",
        "min_market_order_volume", "max_market_order_volume",  "listed_date",
        "delisted_date",           "option_type",              "option_strike",
        "underlying_id",           "underlying_multiple",      "update_day",
        "updated_at"};
    return kFields;
}

dztrader::db::ResultSet query_instruments(dztrader::db::Session& session,
                                          std::string_view instrument_id,
                                          std::span<const std::string> fields) {
    const dztrader::db::ResourceSchema& schema = instruments_schema();

    std::vector<const dztrader::db::FieldSchema*> selected;
    if (fields.empty()) {
        selected.reserve(schema.fields.size());
        for (const dztrader::db::FieldSchema& field : schema.fields) {
            selected.push_back(&field);
        }
    } else {
        selected.reserve(fields.size());
        for (const std::string& name : fields) {
            const dztrader::db::FieldSchema* field = find_instrument_field(name);
            if (field == nullptr) {
                throw Exception(DZ_EC_INVALID_PARAM, "unknown field: {}", name);
            }
            const bool duplicated =
                std::any_of(selected.begin(), selected.end(), [&name](const auto* existing) {
                    return existing->name == name;
                });
            if (duplicated) {
                throw Exception(DZ_EC_INVALID_PARAM, "duplicate field: {}", name);
            }
            selected.push_back(field);
        }
    }

    dztrader::db::Filter filter;
    if (!instrument_id.empty()) {
        filter.add(dztrader::db::filters::eq("instrument_id", instrument_id));
    }
    const dztrader::db::ResultSet all = session.find(
        "instruments", filter,
        dztrader::db::FindOptions{.sort = {{"instrument_id", dztrader::db::SortOrder::Ascending}}});

    std::vector<dztrader::db::Column> columns;
    std::vector<size_t> indexes;
    columns.reserve(selected.size());
    indexes.reserve(selected.size());
    for (const dztrader::db::FieldSchema* field : selected) {
        const std::optional<size_t> index = all.column_index(field->name);
        if (!index.has_value()) {
            throw Exception(DZ_EC_INTERNAL, "instrument column missing | field={}", field->name);
        }
        columns.push_back(dztrader::db::Column{std::string(field->name), field->type});
        indexes.push_back(*index);
    }

    std::vector<db::Row> rows;
    rows.reserve(all.size());
    for (const db::Row& row : all.rows()) {
        std::vector<db::Value> values;
        values.reserve(indexes.size());
        for (size_t index : indexes) {
            values.push_back(row.values()[index]);
        }
        rows.emplace_back(std::move(values));
    }
    return dztrader::db::ResultSet(std::move(columns), std::move(rows));
}

}  // namespace dztrader::tdstore
