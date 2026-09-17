#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/db/database.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/records.h>
#include <dztrader/tdstore/records_store.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/// instruments 查询 SDK 接口 (dz_db_query_instruments) 测试。
/// 建库走 tdstore::schemas() 声明 + 驱动迁移, schema 不与测试副本漂移。
class DbInstrumentsTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = std::filesystem::temp_directory_path() / "dz_test_strategy_db_instruments";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "td.db").string();

        {
            auto database = dztrader::db::Database::open(
                dztrader::db::Config{.backend = "sqlite", .options = {{"path", db_path_}}},
                dztrader::tdstore::schemas());
            database->migrate();
            auto session = database->session();
            const std::vector<dztrader::tdstore::InstrumentRecord> records{
                make_record("rb2601"), make_record("rb2605")};
            dztrader::tdstore::upsert_instruments(*session, records);
        }

        db_ = dz_db_open(db_path_.c_str());
        ASSERT_NE(nullptr, db_) << "dz_db_open failed: " << dz_errmsg();
    }

    void TearDown() override {
        if (db_ != nullptr) {
            dz_db_close(db_);
            db_ = nullptr;
        }
        std::filesystem::remove_all(std::filesystem::path(db_path_).parent_path());
    }

    /// 全字段记录 (值回读断言基准)
    static dztrader::tdstore::InstrumentRecord make_record(const std::string& instrument_id) {
        dztrader::tdstore::InstrumentRecord r{};
        r.instrument_id = instrument_id;
        r.exchange_id = "SHFE";
        r.symbol = instrument_id;
        r.name = "螺纹钢";
        r.product_class = DZ_PRODUCT_FUTURES;
        r.product_code = "rb";
        r.currency = "CNY";
        r.volume_multiple = 10;
        r.volume_step = 1;
        r.price_tick = 0.5;
        r.update_day = "20260915";
        r.updated_at = 1757923200000;
        return r;
    }

    /// 列名 -> 首列索引; 未找到返回列数 (越界哨兵)
    static uint32_t column_index(DzResultSet* rs, const std::string& name) {
        for (uint32_t i = 0; i < dz_resultset_column_count(rs); ++i) {
            if (name == dz_resultset_column_name(rs, i)) {
                return i;
            }
        }
        return dz_resultset_column_count(rs);
    }
};

TEST_F(DbInstrumentsTest, QueryInstrumentsAllFields) {
    DzResultSet* rs = dz_db_query_instruments(db_, nullptr, nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(25u, dz_resultset_column_count(rs));
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 0));

    const uint32_t price_tick = column_index(rs, "price_tick");
    ASSERT_LT(price_tick, dz_resultset_column_count(rs));
    EXPECT_EQ(DZ_COL_TYPE_FLOAT64, dz_resultset_column_type(rs, price_tick));

    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, 0));
    EXPECT_DOUBLE_EQ(0.5, dz_resultset_get_float64(rs, price_tick));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2605", dz_resultset_get_string(rs, 0));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

TEST_F(DbInstrumentsTest, QueryInstrumentsProjection) {
    DzResultSet* rs = dz_db_query_instruments(db_, nullptr, "price_tick,instrument_id");
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(2u, dz_resultset_column_count(rs));
    EXPECT_STREQ("price_tick", dz_resultset_column_name(rs, 0));
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 1));

    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_DOUBLE_EQ(0.5, dz_resultset_get_float64(rs, 0));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, 1));
    dz_resultset_close(rs);
}

TEST_F(DbInstrumentsTest, QueryInstrumentsUnknownField) {
    DzResultSet* rs = dz_db_query_instruments(db_, nullptr, "no_such_field");
    EXPECT_EQ(nullptr, rs);
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_NE(std::string::npos, std::string(dz_errmsg()).find("no_such_field"));
}

TEST_F(DbInstrumentsTest, QueryInstrumentsDuplicateField) {
    // 逗号后空格一并覆盖 parse_fields 的 trim 行为
    DzResultSet* rs = dz_db_query_instruments(db_, nullptr, "price_tick, price_tick");
    EXPECT_EQ(nullptr, rs);
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_NE(std::string::npos, std::string(dz_errmsg()).find("price_tick"));
}

// v3 保留列 (settlement_method/option_exercise_style/option_series) 物理存在但不在 25 个
// 承诺列白名单内: 契约 instrument §8 "不可查询", 必须报 unknown field (与 legacy 白名单一致)。
TEST_F(DbInstrumentsTest, QueryInstrumentsReservedV3FieldsRejected) {
    for (const char* field : {"settlement_method", "option_exercise_style", "option_series"}) {
        DzResultSet* rs = dz_db_query_instruments(db_, nullptr, field);
        EXPECT_EQ(nullptr, rs) << field;
        EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode()) << field;
        EXPECT_NE(std::string::npos, std::string(dz_errmsg()).find(field));
    }
}

TEST_F(DbInstrumentsTest, QueryInstrumentsFilterById) {
    DzResultSet* rs = dz_db_query_instruments(db_, "rb2601", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, 0));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

TEST_F(DbInstrumentsTest, QueryInstrumentsEmptyFilterAllSorted) {
    // 空串 instrument_id/fields 等价 NULL: 全量、全部承诺列
    DzResultSet* rs = dz_db_query_instruments(db_, "", "");
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(25u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, 0));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2605", dz_resultset_get_string(rs, 0));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

TEST_F(DbInstrumentsTest, QueryInstrumentsCommaOnlyFieldsReturnsAll) {
    // 只有分隔符: 空段逐项跳过, 解析结果为空 -> 与 NULL/"" 等价, 返回全部 25 列
    DzResultSet* rs = dz_db_query_instruments(db_, nullptr, ",");
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(25u, dz_resultset_column_count(rs));
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 0));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, 0));
    dz_resultset_close(rs);
}

}  // namespace
