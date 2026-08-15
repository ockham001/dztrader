#include <gtest/gtest.h>

#include <climits>
#include <cfloat>

#include <dztrader/error.h>
#include "result_set.h"
#include "vector_result_set.h"

using namespace dztrader::strategy_api_internal;

namespace {

DzResultSet* make_test_rs() {
    std::vector<ColumnMeta> cols = {
        {DZ_COL_TYPE_INT64, "order_id"},
        {DZ_COL_TYPE_FLOAT64, "price"},
        {DZ_COL_TYPE_STRING, "instrument_id"},
        {DZ_COL_TYPE_BOOL, "is_active"},
    };
    std::vector<Row> rows = {
        {int64_t(1001), 3500.5, std::string("au2506"), true},
        {int64_t(1002), 3510.0, std::string("au2507"), false},
        {ColumnValue(std::monostate{}), DBL_MAX, std::string("rb2506"), true},
    };
    auto impl = std::make_unique<VectorResultSet>(std::move(cols), std::move(rows));
    return new DzResultSet{std::move(impl)};
}

DzResultSet* make_empty_rs() {
    std::vector<ColumnMeta> cols = {
        {DZ_COL_TYPE_INT64, "id"},
    };
    auto impl = std::make_unique<VectorResultSet>(std::move(cols), std::vector<Row>{});
    return new DzResultSet{std::move(impl)};
}

DzResultSet* make_error_rs() {
    std::vector<ColumnMeta> cols;
    auto impl =
        std::make_unique<VectorResultSet>(std::move(cols), std::vector<Row>{}, DZ_EC_NOT_FOUND);
    return new DzResultSet{std::move(impl)};
}

}  // namespace

TEST(ResultSetTest, BasicIteration) {
    auto* rs = make_test_rs();
    ASSERT_NE(rs, nullptr);

    EXPECT_TRUE(rs->impl->next());
    EXPECT_EQ(rs->impl->get_int64(0), 1001);
    EXPECT_DOUBLE_EQ(rs->impl->get_float64(1), 3500.5);
    EXPECT_STREQ(rs->impl->get_string(2), "au2506");
    EXPECT_TRUE(rs->impl->get_bool(3));

    EXPECT_TRUE(rs->impl->next());
    EXPECT_EQ(rs->impl->get_int64(0), 1002);
    EXPECT_DOUBLE_EQ(rs->impl->get_float64(1), 3510.0);
    EXPECT_STREQ(rs->impl->get_string(2), "au2507");
    EXPECT_FALSE(rs->impl->get_bool(3));

    EXPECT_TRUE(rs->impl->next());
    EXPECT_TRUE(rs->impl->is_null(0));
    EXPECT_EQ(rs->impl->get_int64(0), INT64_MAX);
    EXPECT_DOUBLE_EQ(rs->impl->get_float64(1), DBL_MAX);
    EXPECT_STREQ(rs->impl->get_string(2), "rb2506");

    EXPECT_FALSE(rs->impl->next());
    EXPECT_FALSE(rs->impl->next());

    delete rs;
}

TEST(ResultSetTest, ColumnMeta) {
    auto* rs = make_test_rs();
    EXPECT_EQ(rs->impl->column_count(), 4u);
    EXPECT_EQ(rs->impl->column_type(0), DZ_COL_TYPE_INT64);
    EXPECT_EQ(rs->impl->column_type(1), DZ_COL_TYPE_FLOAT64);
    EXPECT_EQ(rs->impl->column_type(2), DZ_COL_TYPE_STRING);
    EXPECT_EQ(rs->impl->column_type(3), DZ_COL_TYPE_BOOL);
    EXPECT_STREQ(rs->impl->column_name(0), "order_id");
    EXPECT_STREQ(rs->impl->column_name(1), "price");
    EXPECT_STREQ(rs->impl->column_name(2), "instrument_id");
    EXPECT_STREQ(rs->impl->column_name(3), "is_active");
    delete rs;
}

TEST(ResultSetTest, Status) {
    auto* ok_rs = make_test_rs();
    EXPECT_EQ(ok_rs->impl->status(), 0);
    delete ok_rs;

    auto* err_rs = make_error_rs();
    EXPECT_EQ(err_rs->impl->status(), DZ_EC_NOT_FOUND);
    delete err_rs;
}

TEST(ResultSetTest, NullHandling) {
    auto* rs = make_test_rs();
    rs->impl->next();
    rs->impl->next();
    rs->impl->next();

    EXPECT_TRUE(rs->impl->is_null(0));
    EXPECT_EQ(rs->impl->get_int64(0), INT64_MAX);

    EXPECT_FALSE(rs->impl->is_null(1));
    EXPECT_DOUBLE_EQ(rs->impl->get_float64(1), DBL_MAX);

    delete rs;
}

TEST(ResultSetTest, EmptyResultSet) {
    auto* rs = make_empty_rs();
    EXPECT_EQ(rs->impl->column_count(), 1u);
    EXPECT_FALSE(rs->impl->next());
    EXPECT_EQ(rs->impl->status(), 0);
    delete rs;
}

TEST(ResultSetTest, IndexOutOfBounds) {
    auto* rs = make_test_rs();
    rs->impl->next();

    EXPECT_EQ(rs->impl->column_type(99), DZ_COL_TYPE_NULL);
    EXPECT_STREQ(rs->impl->column_name(99), "");
    EXPECT_TRUE(rs->impl->is_null(99));
    EXPECT_EQ(rs->impl->get_int64(99), INT64_MAX);
    EXPECT_DOUBLE_EQ(rs->impl->get_float64(99), DBL_MAX);
    EXPECT_STREQ(rs->impl->get_string(99), "");
    EXPECT_FALSE(rs->impl->get_bool(99));

    delete rs;
}

TEST(ResultSetTest, TypeMismatch) {
    auto* rs = make_test_rs();
    rs->impl->next();

    EXPECT_DOUBLE_EQ(rs->impl->get_float64(0), DBL_MAX);
    EXPECT_EQ(rs->impl->get_int64(1), INT64_MAX);
    EXPECT_STREQ(rs->impl->get_string(0), "");
    EXPECT_FALSE(rs->impl->get_bool(0));

    delete rs;
}

TEST(ResultSetTest, CloseIsSafe) {
    auto* rs = make_test_rs();
    delete rs;
}
