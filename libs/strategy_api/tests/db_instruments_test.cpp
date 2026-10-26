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

#include "test_temp_dir.h"

namespace {

/// instruments 查询 SDK 接口 (dz_db_query_instruments) 测试。
/// 建库走 tdstore::schemas() 声明 + 驱动迁移, schema 不与测试副本漂移。
class DbInstrumentsTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = dztrader::test::unique_temp_dir("dz_test_strategy_db_instruments");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "td.db").string();

        {
            auto database = dztrader::db::Database::open(
                dztrader::db::Config{.backend = "sqlite", .options = {{"path", db_path_}}},
                dztrader::tdstore::schemas());
            database->migrate();
            auto session = database->session();
            // 插入序 (rb2605、rb2601) 与期望升序相反: 实现丢掉 ORDER BY instrument_id
            // 时用例必须变红。
            const std::vector<dztrader::tdstore::InstrumentRecord> records{
                make_sentinel_record("rb2605"), make_record("rb2601")};
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

    /// 全字段记录 (值回读断言基准; listed_date/delisted_date 为 DzDate 纪元天数)
    static dztrader::tdstore::InstrumentRecord make_record(const std::string& instrument_id) {
        dztrader::tdstore::InstrumentRecord r{};
        r.instrument_id = instrument_id;
        r.exchange_id = "SHFE";
        r.symbol = instrument_id;
        r.name = "螺纹钢";
        r.product_class = DZ_PRODUCT_FUTURES;
        r.product_code = "rb";
        r.settle_cycle = 1;
        r.currency = "CNY";
        r.volume_multiple = 10;
        r.volume_step = 1;
        r.price_tick = 0.5;
        r.min_limit_order_volume = 1;
        r.max_limit_order_volume = 500;
        r.min_market_order_volume = 1;
        r.max_market_order_volume = 100;
        r.listed_date = 20090;    // 2025-01-02
        r.delisted_date = 20818;  // 2026-12-31
        r.update_day = "20260915";
        r.updated_at = 1757923200000;
        return r;
    }

    /// 哨兵记录: 0=DZ_DATE_NA、<=0=未提供/无限制、空=跟随账户本币、-1=不适用 (契约 instrument §4)。
    static dztrader::tdstore::InstrumentRecord make_sentinel_record(
        const std::string& instrument_id) {
        dztrader::tdstore::InstrumentRecord r = make_record(instrument_id);
        r.settle_cycle = -1;
        r.currency = "";
        r.min_limit_order_volume = 0;
        r.max_limit_order_volume = 0;
        r.min_market_order_volume = 0;
        r.max_market_order_volume = 0;
        r.listed_date = 0;
        r.delisted_date = 0;
        r.option_type = 0;
        r.option_strike = 0;
        r.underlying_id = "";
        r.underlying_multiple = 0;
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
    // 空 fields = 白名单声明序: 首列 instrument_id、末列 updated_at。
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 0));
    EXPECT_STREQ("updated_at", dz_resultset_column_name(rs, 24));

    const auto col = [rs](const char* name) {
        const uint32_t index = column_index(rs, name);
        EXPECT_LT(index, dz_resultset_column_count(rs)) << name;
        return index;
    };
    const uint32_t instrument_id = col("instrument_id");
    const uint32_t exchange_id = col("exchange_id");
    const uint32_t symbol = col("symbol");
    const uint32_t name = col("name");
    const uint32_t product_class = col("product_class");
    const uint32_t product_code = col("product_code");
    const uint32_t settle_cycle = col("settle_cycle");
    const uint32_t currency = col("currency");
    const uint32_t is_inverse = col("is_inverse");
    const uint32_t volume_multiple = col("volume_multiple");
    const uint32_t volume_step = col("volume_step");
    const uint32_t price_tick = col("price_tick");
    const uint32_t min_limit = col("min_limit_order_volume");
    const uint32_t max_limit = col("max_limit_order_volume");
    const uint32_t min_market = col("min_market_order_volume");
    const uint32_t max_market = col("max_market_order_volume");
    const uint32_t listed_date = col("listed_date");
    const uint32_t delisted_date = col("delisted_date");
    const uint32_t option_type = col("option_type");
    const uint32_t option_strike = col("option_strike");
    const uint32_t underlying_id = col("underlying_id");
    const uint32_t underlying_multiple = col("underlying_multiple");
    const uint32_t update_day = col("update_day");
    const uint32_t updated_at = col("updated_at");

    // 类型映射三支: TEXT->STRING、INTEGER->INT64、REAL->FLOAT64。
    EXPECT_EQ(DZ_COL_TYPE_STRING, dz_resultset_column_type(rs, instrument_id));
    EXPECT_EQ(DZ_COL_TYPE_INT64, dz_resultset_column_type(rs, product_class));
    EXPECT_EQ(DZ_COL_TYPE_FLOAT64, dz_resultset_column_type(rs, volume_multiple));
    EXPECT_EQ(DZ_COL_TYPE_FLOAT64, dz_resultset_column_type(rs, price_tick));

    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, instrument_id));
    EXPECT_STREQ("SHFE", dz_resultset_get_string(rs, exchange_id));
    EXPECT_STREQ("rb2601", dz_resultset_get_string(rs, symbol));
    EXPECT_STREQ("螺纹钢", dz_resultset_get_string(rs, name));
    EXPECT_EQ(DZ_PRODUCT_FUTURES, dz_resultset_get_int64(rs, product_class));
    EXPECT_STREQ("rb", dz_resultset_get_string(rs, product_code));
    EXPECT_EQ(1, dz_resultset_get_int64(rs, settle_cycle));
    EXPECT_STREQ("CNY", dz_resultset_get_string(rs, currency));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, is_inverse));
    EXPECT_DOUBLE_EQ(10.0, dz_resultset_get_float64(rs, volume_multiple));
    EXPECT_DOUBLE_EQ(1.0, dz_resultset_get_float64(rs, volume_step));
    EXPECT_DOUBLE_EQ(0.5, dz_resultset_get_float64(rs, price_tick));
    EXPECT_EQ(1, dz_resultset_get_int64(rs, min_limit));
    EXPECT_EQ(500, dz_resultset_get_int64(rs, max_limit));
    EXPECT_EQ(1, dz_resultset_get_int64(rs, min_market));
    EXPECT_EQ(100, dz_resultset_get_int64(rs, max_market));
    EXPECT_EQ(20090, dz_resultset_get_int64(rs, listed_date));
    EXPECT_EQ(20818, dz_resultset_get_int64(rs, delisted_date));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, option_type));  // 期货: 0=非期权
    EXPECT_DOUBLE_EQ(0.0, dz_resultset_get_float64(rs, option_strike));
    EXPECT_STREQ("", dz_resultset_get_string(rs, underlying_id));
    EXPECT_DOUBLE_EQ(0.0, dz_resultset_get_float64(rs, underlying_multiple));
    EXPECT_STREQ("20260915", dz_resultset_get_string(rs, update_day));
    EXPECT_EQ(1757923200000, dz_resultset_get_int64(rs, updated_at));

    // 哨兵行: 0=DZ_DATE_NA、<=0=未提供/无限制、空=跟随账户本币、-1=不适用。
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("rb2605", dz_resultset_get_string(rs, instrument_id));
    EXPECT_EQ(-1, dz_resultset_get_int64(rs, settle_cycle));
    EXPECT_STREQ("", dz_resultset_get_string(rs, currency));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, min_limit));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, max_limit));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, min_market));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, max_market));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, listed_date));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, delisted_date));
    EXPECT_EQ(0, dz_resultset_get_int64(rs, option_type));
    EXPECT_DOUBLE_EQ(0.0, dz_resultset_get_float64(rs, option_strike));
    EXPECT_STREQ("", dz_resultset_get_string(rs, underlying_id));
    EXPECT_DOUBLE_EQ(0.0, dz_resultset_get_float64(rs, underlying_multiple));
    EXPECT_EQ(1757923200000, dz_resultset_get_int64(rs, updated_at));
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

// 契约 instrument §7: 无匹配行 = 空结果集 (非错误), 列元信息仍按请求顺序/类型填充。
TEST_F(DbInstrumentsTest, QueryInstrumentsFilterByIdNoMatch) {
    DzResultSet* rs = dz_db_query_instruments(db_, "rb9999", "price_tick,instrument_id");
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(2u, dz_resultset_column_count(rs));
    EXPECT_STREQ("price_tick", dz_resultset_column_name(rs, 0));
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 1));
    EXPECT_EQ(DZ_COL_TYPE_FLOAT64, dz_resultset_column_type(rs, 0));
    EXPECT_EQ(DZ_COL_TYPE_STRING, dz_resultset_column_type(rs, 1));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

TEST_F(DbInstrumentsTest, QueryInstrumentsEmptyFilterAllSorted) {
    // 空串 instrument_id/fields 等价 NULL: 全量、全部承诺列
    DzResultSet* rs = dz_db_query_instruments(db_, "", "");
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(25u, dz_resultset_column_count(rs));
    EXPECT_STREQ("instrument_id", dz_resultset_column_name(rs, 0));
    EXPECT_STREQ("updated_at", dz_resultset_column_name(rs, 24));
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
