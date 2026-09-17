#include <dztrader/tdstore/instrument_store.h>

#include <algorithm>
#include <iterator>
#include <variant>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::tdstore {

namespace {

/// 承诺字段声明 (白名单 + 列元信息类型).
struct ColumnSpec {
    std::string_view name;
    dztrader::db::legacy::ColumnType type;
};

/// §4 定稿 23 列 + update_day/updated_at = 25 个可选项 (schema 序, 顺序固定).
constexpr ColumnSpec kInstrumentColumns[] = {
    {"instrument_id", dztrader::db::legacy::ColumnType::String},
    {"exchange_id", dztrader::db::legacy::ColumnType::String},
    {"symbol", dztrader::db::legacy::ColumnType::String},
    {"name", dztrader::db::legacy::ColumnType::String},
    {"product_class", dztrader::db::legacy::ColumnType::Int64},
    {"product_code", dztrader::db::legacy::ColumnType::String},
    {"settle_cycle", dztrader::db::legacy::ColumnType::Int64},
    {"currency", dztrader::db::legacy::ColumnType::String},
    {"base_asset", dztrader::db::legacy::ColumnType::String},
    {"is_inverse", dztrader::db::legacy::ColumnType::Int64},
    {"volume_multiple", dztrader::db::legacy::ColumnType::Float64},
    {"volume_step", dztrader::db::legacy::ColumnType::Float64},
    {"price_tick", dztrader::db::legacy::ColumnType::Float64},
    {"min_limit_order_volume", dztrader::db::legacy::ColumnType::Int64},
    {"max_limit_order_volume", dztrader::db::legacy::ColumnType::Int64},
    {"min_market_order_volume", dztrader::db::legacy::ColumnType::Int64},
    {"max_market_order_volume", dztrader::db::legacy::ColumnType::Int64},
    {"listed_date", dztrader::db::legacy::ColumnType::Int64},
    {"delisted_date", dztrader::db::legacy::ColumnType::Int64},
    {"option_type", dztrader::db::legacy::ColumnType::Int64},
    {"option_strike", dztrader::db::legacy::ColumnType::Float64},
    {"underlying_id", dztrader::db::legacy::ColumnType::String},
    {"underlying_multiple", dztrader::db::legacy::ColumnType::Float64},
    {"update_day", dztrader::db::legacy::ColumnType::String},
    {"updated_at", dztrader::db::legacy::ColumnType::Int64},
};

constexpr size_t kColumnCount = std::size(kInstrumentColumns);

const ColumnSpec* find_column(std::string_view name) {
    for (const auto& spec : kInstrumentColumns) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

constexpr const char* kUpsertInstrumentSql =
    "INSERT OR REPLACE INTO instruments ("
    "    instrument_id, exchange_id, symbol, name, product_class, product_code, settle_cycle,"
    "    currency, base_asset, is_inverse, volume_multiple, volume_step, price_tick,"
    "    min_limit_order_volume, max_limit_order_volume, min_market_order_volume,"
    "    max_market_order_volume, listed_date, delisted_date, option_type, option_strike,"
    "    underlying_id, underlying_multiple, update_day, updated_at"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,?,?,?,  ?,?,?,?,?,?,  ?,?,?,?,?,?)";

}  // namespace

const std::vector<std::string>& instrument_columns() {
    static const std::vector<std::string> kColumns = [] {
        std::vector<std::string> columns;
        columns.reserve(kColumnCount);
        for (const auto& spec : kInstrumentColumns) {
            columns.emplace_back(spec.name);
        }
        return columns;
    }();
    return kColumns;
}

InstrumentUpserter::InstrumentUpserter(dztrader::db::legacy::Database& db)
    : stmt_(db.prepare(kUpsertInstrumentSql)) {}

void InstrumentUpserter::upsert(const InstrumentRecord& r) {
    stmt_->reset();
    int i = 1;
    stmt_->bind(i++, r.instrument_id);
    stmt_->bind(i++, r.exchange_id);
    stmt_->bind(i++, r.symbol);
    stmt_->bind(i++, r.name);
    stmt_->bind(i++, static_cast<int64_t>(r.product_class));
    stmt_->bind(i++, r.product_code);
    stmt_->bind(i++, static_cast<int64_t>(r.settle_cycle));
    stmt_->bind(i++, r.currency);
    stmt_->bind(i++, r.base_asset);
    stmt_->bind(i++, static_cast<int64_t>(r.is_inverse));
    stmt_->bind(i++, r.volume_multiple);
    stmt_->bind(i++, r.volume_step);
    stmt_->bind(i++, r.price_tick);
    stmt_->bind(i++, r.min_limit_order_volume);
    stmt_->bind(i++, r.max_limit_order_volume);
    stmt_->bind(i++, r.min_market_order_volume);
    stmt_->bind(i++, r.max_market_order_volume);
    stmt_->bind(i++, static_cast<int64_t>(r.listed_date));
    stmt_->bind(i++, static_cast<int64_t>(r.delisted_date));
    stmt_->bind(i++, static_cast<int64_t>(r.option_type));
    stmt_->bind(i++, r.option_strike);
    stmt_->bind(i++, r.underlying_id);
    stmt_->bind(i++, r.underlying_multiple);
    stmt_->bind(i++, r.update_day);
    stmt_->bind(i++, r.updated_at);
    stmt_->execute();
}

void upsert_instrument(dztrader::db::legacy::Database& db, const InstrumentRecord& r) {
    InstrumentUpserter upserter(db);
    upserter.upsert(r);
}

dztrader::db::legacy::QueryResult query_instruments(dztrader::db::legacy::Database& db,
                                                    std::string_view instrument_id,
                                                    std::span<const std::string> fields) {
    // 1) 校验请求字段 (白名单 + 重复), 得到选中列 (顺序 = 请求顺序)
    std::vector<const ColumnSpec*> selected;
    if (fields.empty()) {
        selected.reserve(kColumnCount);
        for (const auto& spec : kInstrumentColumns) {
            selected.push_back(&spec);
        }
    } else {
        selected.reserve(fields.size());
        for (const auto& field : fields) {
            const ColumnSpec* spec = find_column(field);
            if (spec == nullptr) {
                throw dztrader::Exception(DZ_EC_INVALID_PARAM, "unknown field: {}", field);
            }
            const bool duplicated =
                std::any_of(selected.begin(), selected.end(),
                            [&field](const ColumnSpec* s) { return s->name == field; });
            if (duplicated) {
                throw dztrader::Exception(DZ_EC_INVALID_PARAM, "duplicate field: {}", field);
            }
            selected.push_back(spec);
        }
    }

    // 2) 拼 SQL (列名只来自白名单, 无注入面)
    std::string sql = "SELECT ";
    for (size_t i = 0; i < selected.size(); ++i) {
        if (i > 0) {
            sql += ',';
        }
        sql += selected[i]->name;
    }
    sql += " FROM instruments";
    std::vector<dztrader::db::legacy::BindValue> params;
    if (!instrument_id.empty()) {
        sql += " WHERE instrument_id = ?";
        params.emplace_back(std::string(instrument_id));
    }
    sql += " ORDER BY instrument_id";

    // 3) 取值 + 自填列元信息 (不依赖首行: 0 行时列元信息仍正确)
    dztrader::db::legacy::QueryResult result = db.query(sql, params);
    result.columns.clear();
    result.columns.reserve(selected.size());
    for (const ColumnSpec* spec : selected) {
        result.columns.push_back(
            dztrader::db::legacy::ColumnMeta{spec->type, std::string(spec->name)});
    }
    return result;
}

}  // namespace dztrader::tdstore
