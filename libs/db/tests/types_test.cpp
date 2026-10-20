#include <gtest/gtest.h>

#include <dztrader/db/types.h>

using namespace dztrader::db;

TEST(DbTypesTest, FilterHelpersMapScalars) {
    const Filter f = {filters::eq("account_id", std::string("ctp_001")),
                      filters::ne("account_id", std::string("ctp_002")), filters::gte("seq", 100),
                      filters::lt("seq", 200), filters::in("instrument_id", {"cu2601", "al2601"})};
    const std::span<const Condition> conditions = f.conditions();
    ASSERT_EQ(conditions.size(), 5u);

    EXPECT_EQ(conditions[0].field, "account_id");
    EXPECT_EQ(conditions[0].op, CompareOp::Eq);
    ASSERT_EQ(conditions[0].values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(conditions[0].values[0]), "ctp_001");

    EXPECT_EQ(conditions[1].field, "account_id");
    EXPECT_EQ(conditions[1].op, CompareOp::Ne);
    ASSERT_EQ(conditions[1].values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(conditions[1].values[0]), "ctp_002");

    EXPECT_EQ(conditions[2].field, "seq");
    EXPECT_EQ(conditions[2].op, CompareOp::Gte);
    ASSERT_EQ(conditions[2].values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(conditions[2].values[0]), 100);

    EXPECT_EQ(conditions[3].field, "seq");
    EXPECT_EQ(conditions[3].op, CompareOp::Lt);
    ASSERT_EQ(conditions[3].values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(conditions[3].values[0]), 200);

    EXPECT_EQ(conditions[4].field, "instrument_id");
    EXPECT_EQ(conditions[4].op, CompareOp::In);
    ASSERT_EQ(conditions[4].values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(conditions[4].values[0]), "cu2601");
    EXPECT_EQ(std::get<std::string>(conditions[4].values[1]), "al2601");
}

TEST(DbTypesTest, SingleConditionConvertsToFilter) {
    const Filter f = filters::eq("direction", "B");  // 不是 initializer_list
    ASSERT_EQ(f.conditions().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::string>(f.conditions()[0].values[0]));
    EXPECT_EQ(std::get<std::string>(f.conditions()[0].values[0]), "B");
}

TEST(DbTypesTest, IntLiteralWinsOverDouble) {
    const Condition c = filters::gt("seq", 7);
    EXPECT_TRUE(std::holds_alternative<int64_t>(c.values[0]));
}

TEST(DbTypesTest, FloatLiteralMapsToDouble) {
    const Condition c = filters::gt("price_tick", 10.0);
    EXPECT_EQ(c.field, "price_tick");
    EXPECT_EQ(c.op, CompareOp::Gt);
    ASSERT_EQ(c.values.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<double>(c.values[0]));
    EXPECT_DOUBLE_EQ(std::get<double>(c.values[0]), 10.0);
}

TEST(DbTypesTest, RowGetAndTypeMismatch) {
    const Row row(std::vector<Value>{int64_t{3}, std::string{"x"}});
    EXPECT_EQ(row.get<int64_t>(0), 3);
    EXPECT_EQ(row.get<std::string>(1), "x");
    EXPECT_THROW((void)row.get<double>(0), dztrader::Exception);
    EXPECT_THROW((void)row.get<int64_t>(2), dztrader::Exception);  // 2 = 行宽，越界
}

TEST(DbTypesTest, ResultSetColumnIndex) {
    ResultSet rs(std::vector<Column>{{"seq", ValueType::Int64}, {"account_id", ValueType::String}},
                 std::vector<Row>{});
    ASSERT_TRUE(rs.column_index("account_id").has_value());
    EXPECT_EQ(rs.column_index("account_id").value(), 1u);
    EXPECT_FALSE(rs.column_index("nope").has_value());
    EXPECT_TRUE(rs.empty());
}
