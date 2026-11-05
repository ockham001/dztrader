#include <gtest/gtest.h>

#include <dztrader/core/exception.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/records.h>
#include "td_instrument_query.h"
#include "td_schema.h"
#include <dztrader/tdstore/trade_db.h>

#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "test_temp_dir.h"

namespace dztrader::tdstore {
namespace {

class TdInstrumentQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_td_store", "db");
        (void)dztrader::tdstore::ensure_trade_db(path_);
        database_ = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite", .options = {{"path", path_.string()}}},
            dztrader::db::td_schemas());
        session_ = database_->session();
    }
    void TearDown() override {
        session_.reset();
        database_.reset();
        std::filesystem::remove(path_);
        std::filesystem::remove(path_.string() + "-wal");
        std::filesystem::remove(path_.string() + "-shm");
    }
    std::filesystem::path path_;
    std::unique_ptr<dztrader::db::Database> database_;
    std::unique_ptr<dztrader::db::Session> session_;
};

TEST_F(TdInstrumentQueryTest, UpsertInstrumentIsIdempotentAndQueryable) {
    tdstore::InstrumentRecord r{};
    r.instrument_id = "cu2601";
    r.exchange_id = "SHFE";
    r.symbol = "cu2601";
    r.name = "沪铜2601";
    r.product_class = 1;
    r.product_code = "cu";
    r.volume_multiple = 5.0;
    r.price_tick = 10.0;
    r.update_day = "20260917";
    r.updated_at = 111;
    const tdstore::InstrumentRecord first = r;
    tdstore::upsert_instruments(*session_, std::span<const tdstore::InstrumentRecord>(&first, 1));
    r.updated_at = 222;
    const tdstore::InstrumentRecord second = r;
    tdstore::upsert_instruments(*session_, std::span<const tdstore::InstrumentRecord>(&second, 1));

    const auto result = tdstore::query_instruments(*session_, "cu2601", {});
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result.columns().size(), 28u);
    EXPECT_EQ(result.columns()[0].name, "instrument_id");
    EXPECT_EQ(result.rows()[0].get<int64_t>(27), 222);  // updated_at
}

TEST_F(TdInstrumentQueryTest, QueryInstrumentsProjectionFollowsRequest) {
    tdstore::InstrumentRecord r{};
    r.instrument_id = "cu2601";
    r.exchange_id = "SHFE";
    r.symbol = "cu2601";
    r.price_tick = 10.0;
    const tdstore::InstrumentRecord record = r;
    tdstore::upsert_instruments(*session_, std::span<const tdstore::InstrumentRecord>(&record, 1));

    const std::vector<std::string> fields = {"price_tick", "instrument_id"};
    const auto result = tdstore::query_instruments(*session_, "", fields);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "price_tick");
    EXPECT_EQ(result.columns()[1].name, "instrument_id");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(0), 10.0);
    EXPECT_EQ(result.rows()[0].get<std::string>(1), "cu2601");
}

TEST_F(TdInstrumentQueryTest, QueryInstrumentsEmptyResultKeepsColumnMeta) {
    const std::vector<std::string> fields = {"price_tick", "instrument_id"};
    const auto result = tdstore::query_instruments(*session_, "", fields);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "price_tick");
    EXPECT_EQ(result.columns()[0].type, dztrader::db::ValueType::Float64);
    EXPECT_EQ(result.columns()[1].name, "instrument_id");
    EXPECT_EQ(result.columns()[1].type, dztrader::db::ValueType::String);
    EXPECT_TRUE(result.rows().empty());
}

TEST_F(TdInstrumentQueryTest, QueryInstrumentsUnknownFieldThrows) {
    const std::vector<std::string> fields = {"nope"};
    try {
        (void)tdstore::query_instruments(*session_, "", fields);
        FAIL() << "query_instruments should reject unknown field";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_INVALID_PARAM);
        EXPECT_NE(std::string(e.what()).find("nope"), std::string::npos) << "error=" << e.what();
    }
}

TEST_F(TdInstrumentQueryTest, QueryInstrumentsDuplicateFieldThrows) {
    const std::vector<std::string> fields = {"price_tick", "price_tick"};
    try {
        (void)tdstore::query_instruments(*session_, "", fields);
        FAIL() << "query_instruments should reject duplicate field";
    } catch (const dztrader::Exception& e) {
        EXPECT_EQ(e.code(), DZ_EC_INVALID_PARAM);
        EXPECT_NE(std::string(e.what()).find("price_tick"), std::string::npos)
            << "error=" << e.what();
    }
}

TEST_F(TdInstrumentQueryTest, ToValueCharUsesCharacterCode) {
    EXPECT_EQ(std::get<int64_t>(tdstore::to_value('B')), 66);
    EXPECT_EQ(std::get<int64_t>(tdstore::to_value(static_cast<char>(0xFF))), 255);
}

TEST_F(TdInstrumentQueryTest, MakeInstrumentRowMatchesSchemaOrder) {
    tdstore::InstrumentRecord r{};
    r.instrument_id = "cu2601";
    r.exchange_id = "SHFE";
    r.symbol = "cu2601";
    r.name = "沪铜2601";
    r.product_class = 1;
    r.settle_cycle = 1;
    r.is_inverse = 1;
    r.currency = "CNY";
    r.base_asset = "cu";
    r.min_limit_order_volume = 1;
    r.max_limit_order_volume = 500;
    r.volume_multiple = 5.0;
    r.price_tick = 10.0;
    r.volume_step = 1.0;
    r.listed_date = 20250101;
    r.delisted_date = 20261231;
    r.option_type = 0;
    r.underlying_id = "";
    r.option_strike = 0.0;
    r.update_day = "20260917";
    r.product_code = "cu";
    r.min_market_order_volume = 1;
    r.max_market_order_volume = 500;
    r.underlying_multiple = 0.0;
    r.updated_at = 111;

    const auto row = tdstore::make_instrument_row(r);
    ASSERT_EQ(row.size(), 28u);
    EXPECT_EQ(row.get<std::string>(0), "cu2601");
    EXPECT_EQ(row.get<std::string>(1), "SHFE");
    EXPECT_EQ(row.get<std::string>(2), "cu2601");
    EXPECT_EQ(row.get<std::string>(3), "沪铜2601");
    EXPECT_EQ(row.get<int64_t>(4), 1);
    EXPECT_EQ(row.get<int64_t>(5), 1);
    EXPECT_EQ(row.get<int64_t>(6), 0);  // settlement_method 历史列默认值
    EXPECT_EQ(row.get<int64_t>(7), 1);
    EXPECT_EQ(row.get<std::string>(8), "CNY");
    EXPECT_EQ(row.get<std::string>(9), "cu");
    EXPECT_EQ(row.get<int64_t>(10), 1);
    EXPECT_EQ(row.get<int64_t>(11), 500);
    EXPECT_DOUBLE_EQ(row.get<double>(12), 5.0);
    EXPECT_DOUBLE_EQ(row.get<double>(13), 10.0);
    EXPECT_DOUBLE_EQ(row.get<double>(14), 1.0);
    EXPECT_EQ(row.get<int64_t>(15), 20250101);
    EXPECT_EQ(row.get<int64_t>(16), 20261231);
    EXPECT_EQ(row.get<int64_t>(17), 0);
    EXPECT_EQ(row.get<int64_t>(18), 0);       // option_exercise_style 历史列默认值
    EXPECT_EQ(row.get<std::string>(19), "");  // underlying_id
    EXPECT_DOUBLE_EQ(row.get<double>(20), 0.0);
    EXPECT_EQ(row.get<std::string>(21), "");  // option_series 历史列默认值
    EXPECT_EQ(row.get<std::string>(22), "20260917");
    EXPECT_EQ(row.get<std::string>(23), "cu");
    EXPECT_EQ(row.get<int64_t>(24), 1);
    EXPECT_EQ(row.get<int64_t>(25), 500);
    EXPECT_DOUBLE_EQ(row.get<double>(26), 0.0);
    EXPECT_EQ(row.get<int64_t>(27), 111);
}

}  // namespace
}  // namespace dztrader::tdstore
