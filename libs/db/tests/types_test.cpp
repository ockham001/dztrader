#include <gtest/gtest.h>

#include <dztrader/db/types.h>

using namespace dztrader::db;

TEST(DbTypesTest, FilterHelpersMapScalars) {
    const Filter f = {filters::eq("account_id", std::string("ctp_001")),
                      filters::gte("seq", 100), filters::lt("seq", 200),
                      filters::in("instrument_id", {"cu2601", "al2601"})};
    ASSERT_EQ(f.conditions().size(), 4u);
    EXPECT_EQ(f.conditions()[0].op, CompareOp::Eq);
    EXPECT_EQ(std::get<std::string>(f.conditions()[0].values[0]), "ctp_001");
    EXPECT_EQ(std::get<int64_t>(f.conditions()[1].values[0]), 100);
    EXPECT_EQ(f.conditions()[3].op, CompareOp::In);
    ASSERT_EQ(f.conditions()[3].values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(f.conditions()[3].values[1]), "al2601");
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

TEST(DbTypesTest, RowGetAndTypeMismatch) {
    const Row row(std::vector<Value>{int64_t{3}, std::string{"x"}});
    EXPECT_EQ(row.get<int64_t>(0), 3);
    EXPECT_EQ(row.get<std::string>(1), "x");
    EXPECT_THROW((void)row.get<double>(0), dztrader::Exception);
}

TEST(DbTypesTest, ResultSetColumnIndex) {
    ResultSet rs(std::vector<Column>{{"seq", ValueType::Int64}, {"account_id", ValueType::String}},
                 std::vector<Row>{});
    ASSERT_TRUE(rs.column_index("account_id").has_value());
    EXPECT_EQ(rs.column_index("account_id").value(), 1u);
    EXPECT_FALSE(rs.column_index("nope").has_value());
    EXPECT_TRUE(rs.empty());
}
