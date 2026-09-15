#ifndef DZTRADER_TDSTORE_SCHEMA_H_
#define DZTRADER_TDSTORE_SCHEMA_H_

#include <dztrader/db/migration.h>

namespace dztrader::tdstore {

/// TD schema 当前版本。
/// v4: instruments 列改名 (product→product_class, min/max_order_volume→min/max_limit_order_volume,
///     expiry_date→delisted_date) + 新增 product_code/min_market_order_volume/max_market_order_volume/
///     underlying_multiple/updated_at。
constexpr int kTdSchemaVersion = 4;

/// 注册全部 TD migration (调用方先 add 后 apply)。
void apply_td_migrations(dztrader::db::MigrationManager& mgr);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_SCHEMA_H_
