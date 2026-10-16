/**
 * @file sql_compiler_test.cpp
 * @brief 共享 SQL 编译器单元测试：方言引用、白名单拒绝、文本逐字节、占位符数量
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../src/sql_compiler.h"
#include "../src/sqlite_sql.h"

namespace {

namespace db = dztrader::db;
namespace internal = dztrader::db::internal;

const db::ResourceSchema& orders_schema() {
    static const db::ResourceSchema kSchema{
        .name = "orders",
        .fields = {{"id", db::ValueType::Int64, false, true, true},
                   {"account_id", db::ValueType::String, false, false, false},
                   {"order_id", db::ValueType::Int64, false, false, false},
                   {"seq", db::ValueType::Int64, false, false, false}},
        .indexes = {{"uq_orders", {"account_id", "order_id"}, true}}};
    return kSchema;
}

struct CaughtError {
    bool caught = false;
    DzErrorCode code = DZ_EC_OK;
    std::string message;
};

template <typename Fn>
CaughtError catch_error(Fn&& fn) {
    CaughtError error;
    try {
        fn();
    } catch (const dztrader::Exception& e) {
        error.caught = true;
        error.code = e.code();
        error.message = e.what();
    }
    return error;
}

constexpr std::string_view kSqliteColumns = R"("id", "account_id", "order_id", "seq")";
constexpr std::string_view kMysqlColumns = R"(`id`, `account_id`, `order_id`, `seq`)";
constexpr std::string_view kSqliteSelect =
    R"(SELECT "id", "account_id", "order_id", "seq" FROM "orders")";
constexpr std::string_view kMysqlSelect =
    R"(SELECT `id`, `account_id`, `order_id`, `seq` FROM `orders`)";

}  // namespace

TEST(SqlCompilerTest, DialectQuoteCharacters) {
    EXPECT_EQ(internal::kSqliteDialect.ident_quote, '"');
    EXPECT_EQ(internal::kMysqlDialect.ident_quote, '`');
    EXPECT_EQ(internal::quote_ident(internal::kSqliteDialect, "seq"), "\"seq\"");
    EXPECT_EQ(internal::quote_ident(internal::kMysqlDialect, "seq"), "`seq`");
}

TEST(SqlCompilerTest, FindFieldWhitelist) {
    const db::ResourceSchema& schema = orders_schema();
    ASSERT_NE(internal::find_field(schema, "seq"), nullptr);
    EXPECT_EQ(internal::find_field(schema, "seq")->type, db::ValueType::Int64);
    EXPECT_EQ(internal::find_field(schema, "nope"), nullptr);
}

TEST(SqlCompilerTest, CompileFilterComparisonOperators) {
    struct OpCase {
        db::CompareOp op;
        const char* where;
    };
    const OpCase cases[] = {
        {db::CompareOp::Eq, "\"seq\" = ?"}, {db::CompareOp::Ne, "\"seq\" != ?"},
        {db::CompareOp::Gt, "\"seq\" > ?"}, {db::CompareOp::Gte, "\"seq\" >= ?"},
        {db::CompareOp::Lt, "\"seq\" < ?"}, {db::CompareOp::Lte, "\"seq\" <= ?"},
    };
    for (const OpCase& op_case : cases) {
        const std::vector<db::Condition> conditions{db::Condition{"seq", op_case.op, {int64_t{7}}}};
        const internal::CompiledFilter compiled =
            internal::compile_filter(conditions, orders_schema(), internal::kSqliteDialect);
        EXPECT_EQ(compiled.where, op_case.where);
        ASSERT_EQ(compiled.params.size(), 1u);
        EXPECT_EQ(std::get<int64_t>(compiled.params[0]), 7);
    }
}

TEST(SqlCompilerTest, CompileFilterJoinsWithAnd) {
    const std::vector<db::Condition> conditions{
        db::filters::eq("account_id", std::string("a")),
        db::filters::gt("seq", int64_t{10}),
        db::filters::lte("order_id", int64_t{99}),
    };
    const internal::CompiledFilter compiled =
        internal::compile_filter(conditions, orders_schema(), internal::kSqliteDialect);
    EXPECT_EQ(compiled.where, "\"account_id\" = ? AND \"seq\" > ? AND \"order_id\" <= ?");
    ASSERT_EQ(compiled.params.size(), 3u);
    EXPECT_EQ(std::get<std::string>(compiled.params[0]), "a");
    EXPECT_EQ(std::get<int64_t>(compiled.params[1]), 10);
    EXPECT_EQ(std::get<int64_t>(compiled.params[2]), 99);
}

TEST(SqlCompilerTest, CompileFilterInValues) {
    const std::vector<std::string> accounts{"a", "b", "c"};
    const std::vector<db::Condition> conditions{db::filters::in("account_id", accounts)};
    const internal::CompiledFilter compiled =
        internal::compile_filter(conditions, orders_schema(), internal::kSqliteDialect);
    EXPECT_EQ(compiled.where, "\"account_id\" IN (?, ?, ?)");
    EXPECT_EQ(compiled.params.size(), accounts.size());
}

TEST(SqlCompilerTest, CompileFilterEmptyInIsConstantFalse) {
    const std::vector<db::Condition> only_empty_in{db::Condition{"seq", db::CompareOp::In, {}}};
    const internal::CompiledFilter compiled =
        internal::compile_filter(only_empty_in, orders_schema(), internal::kSqliteDialect);
    EXPECT_EQ(compiled.where, "0");
    EXPECT_TRUE(compiled.params.empty());

    const std::vector<db::Condition> mixed{db::Condition{"seq", db::CompareOp::In, {}},
                                           db::filters::eq("account_id", std::string("a"))};
    const internal::CompiledFilter joined =
        internal::compile_filter(mixed, orders_schema(), internal::kSqliteDialect);
    EXPECT_EQ(joined.where, "0 AND \"account_id\" = ?");
    ASSERT_EQ(joined.params.size(), 1u);
    EXPECT_EQ(std::get<std::string>(joined.params[0]), "a");
}

TEST(SqlCompilerTest, CompileFilterMysqlDialectOnlyQuotesIdentifiers) {
    const std::vector<db::Condition> conditions{
        db::filters::eq("account_id", std::string("a")),
        db::Condition{"seq", db::CompareOp::In, {}},
    };
    const internal::CompiledFilter compiled =
        internal::compile_filter(conditions, orders_schema(), internal::kMysqlDialect);
    EXPECT_EQ(compiled.where, "`account_id` = ? AND 0");
    ASSERT_EQ(compiled.params.size(), 1u);
}

TEST(SqlCompilerTest, CompileFilterUnknownFieldRejected) {
    const std::vector<db::Condition> conditions{db::filters::eq("nope", int64_t{1})};
    const CaughtError error = catch_error([&] {
        static_cast<void>(
            internal::compile_filter(conditions, orders_schema(), internal::kSqliteDialect));
    });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(error.message, "unknown field | field=nope collection=orders");
}

TEST(SqlCompilerTest, CompileFilterMissingValueRejected) {
    const std::vector<db::Condition> conditions{db::Condition{"seq", db::CompareOp::Eq, {}}};
    const CaughtError error = catch_error([&] {
        static_cast<void>(
            internal::compile_filter(conditions, orders_schema(), internal::kSqliteDialect));
    });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_INVALID_PARAM);
    EXPECT_EQ(error.message, "filter condition missing value | field=seq");
}

TEST(SqlCompilerTest, BuildColumnListPerDialect) {
    EXPECT_EQ(internal::build_column_list(orders_schema(), internal::kSqliteDialect),
              kSqliteColumns);
    EXPECT_EQ(internal::build_column_list(orders_schema(), internal::kMysqlDialect), kMysqlColumns);
}

TEST(SqlCompilerTest, BuildPlaceholders) {
    EXPECT_EQ(internal::build_placeholders(0), "");
    EXPECT_EQ(internal::build_placeholders(1), "?");
    EXPECT_EQ(internal::build_placeholders(3), "?, ?, ?");
}

TEST(SqlCompilerTest, BuildInsertSqlPerDialect) {
    EXPECT_EQ(internal::build_insert_sql(orders_schema(), internal::kSqliteDialect),
              "INSERT INTO \"orders\" (" + std::string(kSqliteColumns) + ") VALUES (?, ?, ?, ?)");
    EXPECT_EQ(internal::build_insert_sql(orders_schema(), internal::kMysqlDialect),
              "INSERT INTO `orders` (" + std::string(kMysqlColumns) + ") VALUES (?, ?, ?, ?)");
}

TEST(SqlCompilerTest, BuildDeleteSql) {
    EXPECT_EQ(internal::build_delete_sql(orders_schema(), {}, internal::kSqliteDialect),
              "DELETE FROM \"orders\"");
    const std::vector<db::Condition> conditions{db::filters::eq("account_id", std::string("a"))};
    const internal::CompiledFilter compiled =
        internal::compile_filter(conditions, orders_schema(), internal::kMysqlDialect);
    EXPECT_EQ(internal::build_delete_sql(orders_schema(), compiled, internal::kMysqlDialect),
              "DELETE FROM `orders` WHERE `account_id` = ?");
}

TEST(SqlCompilerTest, BuildSelectSql) {
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, -1, internal::kSqliteDialect),
              kSqliteSelect);
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, -1, internal::kMysqlDialect),
              kMysqlSelect);
}

TEST(SqlCompilerTest, BuildSelectSqlSortLimitOffset) {
    const std::vector<db::SortSpec> sort{{"seq", db::SortOrder::Descending},
                                         {"account_id", db::SortOrder::Ascending}};
    EXPECT_EQ(
        internal::build_select_sql(orders_schema(), {}, sort, 0, -1, internal::kSqliteDialect),
        std::string(kSqliteSelect) + " ORDER BY \"seq\" DESC, \"account_id\" ASC");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 5, 10, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT 10 OFFSET 5");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 5, -1, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT -1 OFFSET 5");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, 3, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT 3");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, 0, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT 0");
}

TEST(SqlCompilerTest, BuildSelectSqlUnlimitedLimitPerDialect) {
    // limit < 0 且 offset == 0：两方言都不输出 LIMIT
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, -1, internal::kSqliteDialect),
              kSqliteSelect);
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, -1, internal::kMysqlDialect),
              kMysqlSelect);
    // limit < 0 且 offset > 0：sqlite 保持 "LIMIT -1"，mysql 用无符号上界（不接受负数）
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 5, -1, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT -1 OFFSET 5");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 5, -1, internal::kMysqlDialect),
              std::string(kMysqlSelect) + " LIMIT 18446744073709551615 OFFSET 5");
    // limit >= 0：两方言一致
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 0, 0, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT 0");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 3, 10, internal::kSqliteDialect),
              std::string(kSqliteSelect) + " LIMIT 10 OFFSET 3");
    EXPECT_EQ(internal::build_select_sql(orders_schema(), {}, {}, 3, 10, internal::kMysqlDialect),
              std::string(kMysqlSelect) + " LIMIT 10 OFFSET 3");
    EXPECT_EQ(internal::kSqliteDialect.unlimited_limit, "-1");
    EXPECT_EQ(internal::kMysqlDialect.unlimited_limit, "18446744073709551615");
}

TEST(SqlCompilerTest, BuildSelectSqlUnknownSortFieldRejected) {
    const std::vector<db::SortSpec> sort{{"nope", db::SortOrder::Ascending}};
    const CaughtError error = catch_error([&] {
        static_cast<void>(
            internal::build_select_sql(orders_schema(), {}, sort, 0, -1, internal::kSqliteDialect));
    });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(error.message, "unknown field | field=nope collection=orders");
}

TEST(SqlCompilerTest, BuildAggregateSqlCount) {
    EXPECT_EQ(internal::build_aggregate_sql(orders_schema(), db::AggregateOp::Count, "nope", {}, {},
                                            {}, internal::kSqliteDialect),
              "SELECT COUNT(*) FROM \"orders\"");
    EXPECT_EQ(internal::build_aggregate_sql(orders_schema(), db::AggregateOp::Count, "", {}, {}, {},
                                            internal::kMysqlDialect),
              "SELECT COUNT(*) FROM `orders`");
}

TEST(SqlCompilerTest, BuildAggregateSqlGroupAndFunctions) {
    struct OpCase {
        db::AggregateOp op;
        const char* function;
    };
    const OpCase cases[] = {{db::AggregateOp::Max, "MAX"},
                            {db::AggregateOp::Min, "MIN"},
                            {db::AggregateOp::Sum, "SUM"},
                            {db::AggregateOp::Avg, "AVG"}};
    const std::vector<std::string> group_by{"account_id"};
    for (const OpCase& op_case : cases) {
        EXPECT_EQ(internal::build_aggregate_sql(orders_schema(), op_case.op, "seq", group_by, {},
                                                {}, internal::kSqliteDialect),
                  std::string("SELECT \"account_id\", ") + op_case.function +
                      "(\"seq\") FROM \"orders\" GROUP BY \"account_id\"");
    }
}

TEST(SqlCompilerTest, BuildAggregateSqlFilterAndSort) {
    const std::vector<db::Condition> conditions{db::filters::eq("account_id", std::string("a"))};
    const internal::CompiledFilter compiled =
        internal::compile_filter(conditions, orders_schema(), internal::kMysqlDialect);
    const std::vector<std::string> group_by{"account_id"};
    const std::vector<db::SortSpec> sort{{"account_id", db::SortOrder::Ascending}};
    EXPECT_EQ(internal::build_aggregate_sql(orders_schema(), db::AggregateOp::Max, "seq", group_by,
                                            compiled, sort, internal::kMysqlDialect),
              "SELECT `account_id`, MAX(`seq`) FROM `orders` WHERE `account_id` = ? "
              "GROUP BY `account_id` ORDER BY `account_id` ASC");
    ASSERT_EQ(compiled.params.size(), 1u);
}

TEST(SqlCompilerTest, BuildAggregateSqlUnknownFieldRejected) {
    const std::vector<std::string> bad_group{"nope"};
    const CaughtError group_error = catch_error([&] {
        static_cast<void>(internal::build_aggregate_sql(orders_schema(), db::AggregateOp::Count, "",
                                                        bad_group, {}, {},
                                                        internal::kSqliteDialect));
    });
    EXPECT_TRUE(group_error.caught);
    EXPECT_EQ(group_error.code, DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(group_error.message, "unknown field | field=nope collection=orders");

    const CaughtError field_error = catch_error([&] {
        static_cast<void>(internal::build_aggregate_sql(
            orders_schema(), db::AggregateOp::Max, "nope", {}, {}, {}, internal::kSqliteDialect));
    });
    EXPECT_TRUE(field_error.caught);
    EXPECT_EQ(field_error.code, DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(field_error.message, "unknown field | field=nope collection=orders");
}

TEST(SqlCompilerTest, SqliteUpsertKeepsInsertOrReplace) {
    EXPECT_EQ(internal::build_upsert_sql(orders_schema()), "INSERT OR REPLACE INTO \"orders\" (" +
                                                               std::string(kSqliteColumns) +
                                                               ") VALUES (?, ?, ?, ?)");
}
