#include <gtest/gtest.h>

#include <cstdio>
#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/records.h>
#include <dztrader/tdstore/records_store.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <variant>
#include <vector>

namespace dztrader::tdstore {
namespace {

std::filesystem::path unique_db_path() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           ("dz_td_records_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)) + ".db");
}

class RecordsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = unique_db_path();
        database_ = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite", .options = {{"path", path_.string()}}},
            dztrader::tdstore::schemas());
        database_->migrate();
        session_ = database_->session();
    }
    void TearDown() override {
        session_.reset();
        database_.reset();
        std::filesystem::remove(path_);
    }
    std::filesystem::path path_;
    std::unique_ptr<dztrader::db::Database> database_;
    std::unique_ptr<dztrader::db::Session> session_;
};

TEST_F(RecordsStoreTest, UpsertInstrumentIsIdempotentAndQueryable) {
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

TEST_F(RecordsStoreTest, QueryInstrumentsProjectionFollowsRequest) {
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

TEST_F(RecordsStoreTest, QueryInstrumentsEmptyResultKeepsColumnMeta) {
    const std::vector<std::string> fields = {"price_tick", "instrument_id"};
    const auto result = tdstore::query_instruments(*session_, "", fields);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "price_tick");
    EXPECT_EQ(result.columns()[0].type, dztrader::db::ValueType::Float64);
    EXPECT_EQ(result.columns()[1].name, "instrument_id");
    EXPECT_EQ(result.columns()[1].type, dztrader::db::ValueType::String);
    EXPECT_TRUE(result.rows().empty());
}

TEST_F(RecordsStoreTest, QueryInstrumentsUnknownFieldThrows) {
    const std::vector<std::string> fields = {"nope"};
    EXPECT_THROW(tdstore::query_instruments(*session_, "", fields), dztrader::Exception);
}

TEST_F(RecordsStoreTest, QueryInstrumentsDuplicateFieldThrows) {
    const std::vector<std::string> fields = {"price_tick", "price_tick"};
    EXPECT_THROW(tdstore::query_instruments(*session_, "", fields), dztrader::Exception);
}

TEST_F(RecordsStoreTest, ToValueCharUsesCharacterCode) {
    EXPECT_EQ(std::get<int64_t>(tdstore::to_value('B')), 66);
    EXPECT_EQ(std::get<int64_t>(tdstore::to_value(static_cast<char>(0xFF))), 255);
}

TEST_F(RecordsStoreTest, MakeInstrumentRowMatchesSchemaOrder) {
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

TEST_F(RecordsStoreTest, MakePositionRowMatchesSchemaOrder) {
    DzPositionInfo p{};
    std::snprintf(p.account_id, sizeof(p.account_id), "%s", "ctp_001");
    std::snprintf(p.instrument_id, sizeof(p.instrument_id), "%s", "cu2601");
    std::snprintf(p.exchange_id, sizeof(p.exchange_id), "%s", "SHFE");
    p.direction = '0';
    p.volume = 3;
    p.frozen_volume = 4;
    p.today_volume = 1;
    p.yd_volume = 2;
    p.price = 70000.0;
    p.seq = 42;
    const auto row = tdstore::make_position_row(p, "20260917");
    ASSERT_EQ(row.size(), 11u);
    EXPECT_EQ(row.get<std::string>(0), "ctp_001");
    EXPECT_EQ(row.get<std::string>(1), "20260917");
    EXPECT_EQ(row.get<std::string>(2), "cu2601");
    EXPECT_EQ(row.get<std::string>(3), "SHFE");
    EXPECT_EQ(row.get<int64_t>(4), static_cast<int64_t>('0'));
    EXPECT_EQ(row.get<int64_t>(5), 3);
    EXPECT_EQ(row.get<int64_t>(6), 4);
    EXPECT_EQ(row.get<int64_t>(7), 1);
    EXPECT_EQ(row.get<int64_t>(8), 2);
    EXPECT_DOUBLE_EQ(row.get<double>(9), 70000.0);
    EXPECT_EQ(row.get<int64_t>(10), 42);
}

TEST_F(RecordsStoreTest, MakeTradingAccountRowMatchesSchemaOrder) {
    DzTradingAccount a{};
    std::snprintf(a.account_id, sizeof(a.account_id), "%s", "ctp_001");
    a.balance = 100000.0;
    a.available = 90000.0;
    a.frozen = 1000.0;
    a.commission = 10.0;
    a.margin = 9000.0;
    a.withdraw_quota = 80000.0;
    a.deposit = 5000.0;
    a.withdraw = 100.0;
    a.seq = 7;
    const auto row = tdstore::make_trading_account_row(a, "20260917");
    ASSERT_EQ(row.size(), 11u);
    EXPECT_EQ(row.get<std::string>(0), "ctp_001");
    EXPECT_EQ(row.get<std::string>(1), "20260917");
    EXPECT_DOUBLE_EQ(row.get<double>(2), 100000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(3), 90000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(4), 1000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(5), 10.0);
    EXPECT_DOUBLE_EQ(row.get<double>(6), 9000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(7), 80000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(8), 5000.0);
    EXPECT_DOUBLE_EQ(row.get<double>(9), 100.0);
    EXPECT_EQ(row.get<int64_t>(10), 7);
}

}  // namespace
}  // namespace dztrader::tdstore
