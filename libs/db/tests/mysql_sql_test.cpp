// MySQL 方言 SQL 文本与类型映射单测（无需服务器）
#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL

#include <gtest/gtest.h>

#include <dztrader/core/exception.h>
#include <dztrader/db/types.h>
#include <dztrader/error.h>

#include <string>
#include <vector>

#include "../src/mysql_sql.h"
#include "../src/sql_compiler.h"

namespace db = dztrader::db;
namespace internal_ns = dztrader::db::internal;

namespace {

using db::ValueType;

template <typename Call>
DzErrorCode code_of(Call&& call) {
    try {
        call();
    } catch (const dztrader::Exception& e) {
        return e.code();
    }
    return DZ_EC_OK;
}

db::ResourceSchema probe_schema() {
    return db::ResourceSchema{
        .name = "orders",
        .fields = {{"id", ValueType::Int64, false, true, true},
                   {"account_id", ValueType::String, false, false, false},
                   {"seq", ValueType::Int64, false, false, false},
                   {"price", ValueType::Float64, true, false, false},
                   {"active", ValueType::Bool, false, false, false}},
        .indexes = {{"uq_orders", {"account_id", "seq"}, true}, {"idx_price", {"price"}, false}}};
}

}  // namespace

TEST(MysqlSqlTest, ColumnTypeMapping) {
    EXPECT_EQ(internal_ns::mysql_column_type(ValueType::Int64, "c", "t"), "BIGINT");
    EXPECT_EQ(internal_ns::mysql_column_type(ValueType::Float64, "c", "t"), "DOUBLE");
    EXPECT_EQ(internal_ns::mysql_column_type(ValueType::Bool, "c", "t"), "TINYINT(1)");
    EXPECT_EQ(internal_ns::mysql_column_type(ValueType::String, "c", "t"),
              "VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin");
    EXPECT_EQ(code_of([&] { (void)internal_ns::mysql_column_type(ValueType::Null, "c", "t"); }),
              DZ_EC_INVALID_PARAM);
}

TEST(MysqlSqlTest, CreateTableText) {
    const db::ResourceSchema schema = probe_schema();
    const std::string expected =
        "CREATE TABLE IF NOT EXISTS `orders` (\n"
        "  `id` BIGINT NOT NULL AUTO_INCREMENT,\n"
        "  `account_id` VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,\n"
        "  `seq` BIGINT NOT NULL,\n"
        "  `price` DOUBLE,\n"
        "  `active` TINYINT(1) NOT NULL,\n"
        "  PRIMARY KEY (`id`),\n"
        "  UNIQUE KEY `uq_orders` (`account_id`, `seq`),\n"
        "  KEY `idx_price` (`price`)\n"
        ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin";
    EXPECT_EQ(internal_ns::build_mysql_create_table_sql(schema), expected);
}

// 缺陷回归（verifier B2 独立复现）：无索引形态不得留尾随逗号。
// 真实漏网表 = tdstore instruments（28 字段 + 主键 + 零索引）；这里覆盖四种形态
TEST(MysqlSqlTest, CreateTableHasNoTrailingCommaInAllShapes) {
    const std::string suffix =
        ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin";

    // 形态 1：有主键 + 零索引（instruments 形状，主键非自增）
    const db::ResourceSchema no_index_with_pk{
        .name = "instruments_probe",
        .fields = {{"instrument_id", ValueType::String, false, true, false},
                   {"price_tick", ValueType::Float64, false, false, false}},
        .indexes = {}};
    const std::string no_index_with_pk_sql =
        "CREATE TABLE IF NOT EXISTS `instruments_probe` (\n"
        "  `instrument_id` VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,\n"
        "  `price_tick` DOUBLE NOT NULL,\n"
        "  PRIMARY KEY (`instrument_id`)\n" +
        suffix;
    EXPECT_EQ(internal_ns::build_mysql_create_table_sql(no_index_with_pk), no_index_with_pk_sql);

    // 形态 2：单字段 + 无主键 + 零索引
    const db::ResourceSchema minimal{.name = "minimal",
                                     .fields = {{"value", ValueType::Int64, false, false, false}},
                                     .indexes = {}};
    const std::string minimal_sql =
        "CREATE TABLE IF NOT EXISTS `minimal` (\n"
        "  `value` BIGINT NOT NULL\n" +
        suffix;
    EXPECT_EQ(internal_ns::build_mysql_create_table_sql(minimal), minimal_sql);

    // 形态 3：无主键 + 单索引
    const db::ResourceSchema only_index{
        .name = "only_index",
        .fields = {{"account_id", ValueType::String, false, false, false},
                   {"seq", ValueType::Int64, false, false, false}},
        .indexes = {{"idx_seq", {"seq"}, false}}};
    const std::string only_index_sql =
        "CREATE TABLE IF NOT EXISTS `only_index` (\n"
        "  `account_id` VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,\n"
        "  `seq` BIGINT NOT NULL,\n"
        "  KEY `idx_seq` (`seq`)\n" +
        suffix;
    EXPECT_EQ(internal_ns::build_mysql_create_table_sql(only_index), only_index_sql);

    // 形态 4：无主键 + 多索引（唯一键在后）
    const db::ResourceSchema multi_index{
        .name = "multi_index",
        .fields = {{"a", ValueType::Int64, false, false, false},
                   {"b", ValueType::Int64, true, false, false}},
        .indexes = {{"idx_a", {"a"}, false}, {"uq_b", {"b"}, true}}};
    const std::string multi_index_sql =
        "CREATE TABLE IF NOT EXISTS `multi_index` (\n"
        "  `a` BIGINT NOT NULL,\n"
        "  `b` BIGINT,\n"
        "  KEY `idx_a` (`a`),\n"
        "  UNIQUE KEY `uq_b` (`b`)\n" +
        suffix;
    EXPECT_EQ(internal_ns::build_mysql_create_table_sql(multi_index), multi_index_sql);

    // 显式反断言：任何形态都不得出现 ",\n)" 尾随逗号模式
    const db::ResourceSchema with_pk_and_indexes = probe_schema();
    for (const db::ResourceSchema* schema :
         {&no_index_with_pk, &minimal, &only_index, &multi_index, &with_pk_and_indexes}) {
        const std::string sql = internal_ns::build_mysql_create_table_sql(*schema);
        EXPECT_EQ(sql.find(",\n)"), std::string::npos)
            << "collection=" << schema->name << " sql=" << sql;
    }
}

TEST(MysqlSqlTest, CreateTableRejectsBadSchema) {
    db::ResourceSchema no_fields{.name = "t", .fields = {}, .indexes = {}};
    EXPECT_EQ(code_of([&] { (void)internal_ns::build_mysql_create_table_sql(no_fields); }),
              DZ_EC_INVALID_PARAM);

    db::ResourceSchema unknown_index_field = probe_schema();
    unknown_index_field.indexes = {{"idx_bad", {"missing"}, false}};
    EXPECT_EQ(
        code_of([&] { (void)internal_ns::build_mysql_create_table_sql(unknown_index_field); }),
        DZ_EC_INVALID_PARAM);

    db::ResourceSchema empty_index = probe_schema();
    empty_index.indexes = {{"idx_empty", {}, false}};
    EXPECT_EQ(code_of([&] { (void)internal_ns::build_mysql_create_table_sql(empty_index); }),
              DZ_EC_INVALID_PARAM);
}

TEST(MysqlSqlTest, UpsertSkipsAutoIncrement) {
    const db::ResourceSchema schema = probe_schema();
    EXPECT_EQ(
        internal_ns::build_mysql_upsert_sql(schema),
        "INSERT INTO `orders` (`id`, `account_id`, `seq`, `price`, `active`) VALUES (?, ?, ?, "
        "?, ?) ON DUPLICATE KEY UPDATE `account_id`=VALUES(`account_id`), "
        "`seq`=VALUES(`seq`), `price`=VALUES(`price`), `active`=VALUES(`active`)");
}

TEST(MysqlSqlTest, UpsertSelfAssignWhenOnlyAutoIncrement) {
    const db::ResourceSchema schema{
        .name = "counter", .fields = {{"id", ValueType::Int64, false, true, true}}, .indexes = {}};
    EXPECT_EQ(internal_ns::build_mysql_upsert_sql(schema),
              "INSERT INTO `counter` (`id`) VALUES (?) ON DUPLICATE KEY UPDATE `id`=`id`");
}

TEST(MysqlSqlTest, UpsertRejectsEmptyFields) {
    const db::ResourceSchema no_fields{.name = "t", .fields = {}, .indexes = {}};
    EXPECT_EQ(code_of([&] { (void)internal_ns::build_mysql_upsert_sql(no_fields); }),
              DZ_EC_INVALID_PARAM);
}

TEST(MysqlSqlTest, VersionTableShape) {
    EXPECT_EQ(internal_ns::kMysqlSchemaVersionTable, "schema_version");
    const std::string_view ddl = internal_ns::mysql_schema_version_ddl();
    EXPECT_NE(ddl.find("CREATE TABLE IF NOT EXISTS `schema_version`"), std::string_view::npos);
    EXPECT_NE(ddl.find("`version` INT NOT NULL"), std::string_view::npos);
    EXPECT_NE(ddl.find("`applied_at` VARCHAR(32) NOT NULL"), std::string_view::npos);
    EXPECT_NE(ddl.find("PRIMARY KEY (`version`)"), std::string_view::npos);
    EXPECT_NE(ddl.find("ENGINE=InnoDB ROW_FORMAT=DYNAMIC"), std::string_view::npos);
    EXPECT_EQ(internal_ns::mysql_schema_version_select(),
              "SELECT MAX(`version`) FROM `schema_version`");
    EXPECT_EQ(internal_ns::mysql_schema_version_insert(),
              "INSERT INTO `schema_version` (`version`, `applied_at`) VALUES (?, ?) ON DUPLICATE "
              "KEY UPDATE `applied_at`=VALUES(`applied_at`)");
}

// 与 WP1 共享编译器的方言契约（mysql 驱动侧依赖这些文本）
TEST(MysqlSqlTest, CompilerMysqlDialectText) {
    const db::ResourceSchema schema = probe_schema();
    const std::vector<db::Condition> conditions = {
        db::filters::eq("account_id", std::string("a")),
        db::filters::in("seq", {int64_t{1}, int64_t{2}})};
    const internal_ns::CompiledFilter compiled =
        internal_ns::compile_filter(conditions, schema, internal_ns::kMysqlDialect);
    EXPECT_EQ(compiled.where, "`account_id` = ? AND `seq` IN (?, ?)");
    ASSERT_EQ(compiled.params.size(), 3u);

    const std::vector<db::SortSpec> sort = {{"seq", db::SortOrder::Descending}};
    EXPECT_EQ(internal_ns::build_select_sql(schema, {}, sort, 1, 1, internal_ns::kMysqlDialect),
              "SELECT `id`, `account_id`, `seq`, `price`, `active` FROM `orders` ORDER BY `seq` "
              "DESC LIMIT 1 OFFSET 1");
    // 不限量 + offset：MySQL 不接受 LIMIT -1，方言常量给出无符号上界
    EXPECT_EQ(internal_ns::build_select_sql(schema, {}, {}, 5, -1, internal_ns::kMysqlDialect),
              "SELECT `id`, `account_id`, `seq`, `price`, `active` FROM `orders` LIMIT "
              "18446744073709551615 OFFSET 5");
    EXPECT_EQ(internal_ns::build_select_sql(schema, {}, {}, 0, -1, internal_ns::kMysqlDialect),
              "SELECT `id`, `account_id`, `seq`, `price`, `active` FROM `orders`");

    const internal_ns::CompiledFilter empty;
    const std::vector<std::string> group_by = {"account_id"};
    EXPECT_EQ(internal_ns::build_aggregate_sql(schema, db::AggregateOp::Count, "", group_by, empty,
                                               {}, internal_ns::kMysqlDialect),
              "SELECT `account_id`, COUNT(*) FROM `orders` GROUP BY `account_id`");
    EXPECT_EQ(internal_ns::build_aggregate_sql(schema, db::AggregateOp::Max, "seq", {}, empty, {},
                                               internal_ns::kMysqlDialect),
              "SELECT MAX(`seq`) FROM `orders`");
    EXPECT_EQ(internal_ns::build_delete_sql(schema, empty, internal_ns::kMysqlDialect),
              "DELETE FROM `orders`");
}

TEST(MysqlSqlTest, CompilerRejectsUnknownFilterField) {
    const db::ResourceSchema schema = probe_schema();
    const std::vector<db::Condition> conditions = {db::filters::eq("missing", std::string("a"))};
    EXPECT_EQ(code_of([&] {
                  (void)internal_ns::compile_filter(conditions, schema, internal_ns::kMysqlDialect);
              }),
              DZ_EC_DB_QUERY_FAILED);
}

#endif  // DZ_WITH_MYSQL
