#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include <dztrader/core/exception.h>
#include <dztrader/db/connection.h>
#include <dztrader/db/database_sqlite.h>
#include <dztrader/db/migration.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/instrument_store.h>
#include <dztrader/tdstore/records.h>
#include <dztrader/tdstore/schema.h>

namespace dztrader::tdstore {
namespace {

/// 全字段记录 (值回读断言基准)
InstrumentRecord make_record(const std::string& instrument_id) {
    InstrumentRecord r{};
    r.instrument_id = instrument_id;
    r.exchange_id = "SHFE";
    r.symbol = instrument_id;
    r.name = "螺纹钢";
    r.product_class = DZ_PRODUCT_FUTURES;
    r.product_code = "rb";
    r.settle_cycle = -1;
    r.currency = "CNY";
    r.base_asset = "";
    r.is_inverse = 0;
    r.volume_multiple = 10;
    r.volume_step = 1;
    r.price_tick = 0.5;
    r.min_limit_order_volume = 1;
    r.max_limit_order_volume = 500;
    r.min_market_order_volume = 1;
    r.max_market_order_volume = 500;
    r.listed_date = 20000;
    r.delisted_date = 21000;
    r.option_type = 0;
    r.option_strike = 0;
    r.underlying_id = "";
    r.underlying_multiple = 0;
    r.update_day = "20260915";
    r.updated_at = 1757923200000;
    return r;
}

class InstrumentStoreTest : public ::testing::Test {
protected:
    dztrader::db::Connection conn{":memory:"};
    dztrader::db::SqliteDatabaseRef db_{conn.db()};

    void SetUp() override {
        dztrader::db::MigrationManager mgr;
        apply_td_migrations(mgr);
        mgr.apply(conn.db());
    }
};

TEST_F(InstrumentStoreTest, UpsertThenQueryAllFields) {
    const InstrumentRecord record = make_record("rb2601");
    upsert_instrument(db_, record);

    const auto result = query_instruments(db_, "", {});
    ASSERT_EQ(result.columns.size(), 25u);
    EXPECT_EQ(result.columns[0].name, "instrument_id");
    ASSERT_EQ(result.rows.size(), 1u);
    const auto& row = result.rows.front();
    ASSERT_EQ(row.size(), 25u);
    EXPECT_EQ(std::get<std::string>(row[0]), "rb2601");
    EXPECT_EQ(std::get<std::string>(row[1]), "SHFE");
    EXPECT_EQ(std::get<std::string>(row[2]), "rb2601");
    EXPECT_EQ(std::get<std::string>(row[3]), "螺纹钢");
    EXPECT_EQ(std::get<int64_t>(row[4]), DZ_PRODUCT_FUTURES);
    EXPECT_EQ(std::get<std::string>(row[5]), "rb");
    EXPECT_EQ(std::get<int64_t>(row[6]), -1);
    EXPECT_EQ(std::get<std::string>(row[7]), "CNY");
    EXPECT_EQ(std::get<std::string>(row[8]), "");
    EXPECT_EQ(std::get<int64_t>(row[9]), 0);
    EXPECT_DOUBLE_EQ(std::get<double>(row[10]), 10.0);
    EXPECT_DOUBLE_EQ(std::get<double>(row[11]), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(row[12]), 0.5);
    EXPECT_EQ(std::get<int64_t>(row[13]), 1);
    EXPECT_EQ(std::get<int64_t>(row[14]), 500);
    EXPECT_EQ(std::get<int64_t>(row[15]), 1);
    EXPECT_EQ(std::get<int64_t>(row[16]), 500);
    EXPECT_EQ(std::get<int64_t>(row[17]), 20000);
    EXPECT_EQ(std::get<int64_t>(row[18]), 21000);
    EXPECT_EQ(std::get<int64_t>(row[19]), 0);
    EXPECT_DOUBLE_EQ(std::get<double>(row[20]), 0.0);
    EXPECT_EQ(std::get<std::string>(row[21]), "");
    EXPECT_DOUBLE_EQ(std::get<double>(row[22]), 0.0);
    EXPECT_EQ(std::get<std::string>(row[23]), "20260915");
    EXPECT_EQ(std::get<int64_t>(row[24]), 1757923200000);
}

TEST_F(InstrumentStoreTest, UpsertIsReplaceIdempotent) {
    upsert_instrument(db_, make_record("rb2601"));
    InstrumentRecord updated = make_record("rb2601");
    updated.price_tick = 1.0;
    updated.updated_at = 1757923200001;
    upsert_instrument(db_, updated);

    const std::vector<std::string> fields = {"instrument_id", "price_tick", "updated_at"};
    const auto result = query_instruments(db_, "", fields);
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(std::get<std::string>(result.rows[0][0]), "rb2601");
    EXPECT_DOUBLE_EQ(std::get<double>(result.rows[0][1]), 1.0);
    EXPECT_EQ(std::get<int64_t>(result.rows[0][2]), 1757923200001);
}

TEST_F(InstrumentStoreTest, UpserterReusesStatementAndUpserts) {
    // 同一预编译器连写 2 条 + 覆盖同 id: 行数不增, 覆盖后取值正确
    InstrumentUpserter upserter(db_);
    upserter.upsert(make_record("rb2601"));
    upserter.upsert(make_record("rb2605"));
    InstrumentRecord updated = make_record("rb2601");
    updated.price_tick = 1.0;
    updated.updated_at = 1757923200001;
    upserter.upsert(updated);

    const std::vector<std::string> fields = {"instrument_id", "price_tick", "updated_at"};
    const auto result = query_instruments(db_, "", fields);
    ASSERT_EQ(result.rows.size(), 2u);
    EXPECT_EQ(std::get<std::string>(result.rows[0][0]), "rb2601");
    EXPECT_DOUBLE_EQ(std::get<double>(result.rows[0][1]), 1.0);
    EXPECT_EQ(std::get<int64_t>(result.rows[0][2]), 1757923200001);
    EXPECT_EQ(std::get<std::string>(result.rows[1][0]), "rb2605");
    EXPECT_DOUBLE_EQ(std::get<double>(result.rows[1][1]), 0.5);
}

TEST_F(InstrumentStoreTest, ProjectionOrderFollowsRequest) {
    upsert_instrument(db_, make_record("rb2601"));

    const std::vector<std::string> fields = {"price_tick", "instrument_id"};
    const auto result = query_instruments(db_, "", fields);
    ASSERT_EQ(result.columns.size(), 2u);
    EXPECT_EQ(result.columns[0].name, "price_tick");
    EXPECT_EQ(result.columns[1].name, "instrument_id");
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(result.rows[0][0]), 0.5);
    EXPECT_EQ(std::get<std::string>(result.rows[0][1]), "rb2601");
}

TEST_F(InstrumentStoreTest, UnknownFieldThrows) {
    const std::vector<std::string> fields = {"no_such_field"};
    try {
        (void)query_instruments(db_, "", fields);
        FAIL() << "unknown field must throw";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_INVALID_PARAM);
    }
}

TEST_F(InstrumentStoreTest, DuplicateFieldThrows) {
    const std::vector<std::string> fields = {"price_tick", "price_tick"};
    try {
        (void)query_instruments(db_, "", fields);
        FAIL() << "duplicate field must throw";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_INVALID_PARAM);
    }
}

TEST_F(InstrumentStoreTest, EmptyInstrumentIdReturnsAllSorted) {
    upsert_instrument(db_, make_record("rb2605"));
    upsert_instrument(db_, make_record("rb2601"));

    const std::vector<std::string> fields = {"instrument_id"};
    const auto result = query_instruments(db_, "", fields);
    ASSERT_EQ(result.rows.size(), 2u);
    EXPECT_EQ(std::get<std::string>(result.rows[0][0]), "rb2601");
    EXPECT_EQ(std::get<std::string>(result.rows[1][0]), "rb2605");
}

TEST_F(InstrumentStoreTest, QueryInstrumentsEmptyKeepsColumnMeta) {
    const std::vector<std::string> fields = {"price_tick", "instrument_id"};
    const auto result = query_instruments(db_, "", fields);
    ASSERT_EQ(result.columns.size(), 2u);
    EXPECT_EQ(result.columns[0].name, "price_tick");
    EXPECT_EQ(result.columns[0].type, dztrader::db::ColumnType::Float64);
    EXPECT_EQ(result.columns[1].name, "instrument_id");
    EXPECT_EQ(result.columns[1].type, dztrader::db::ColumnType::String);
    EXPECT_TRUE(result.rows.empty());
}

}  // namespace
}  // namespace dztrader::tdstore
