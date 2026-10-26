#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/exception.h>
#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/error.h>

#include <filesystem>
#include <string>

#include "test_temp_dir.h"

using namespace dztrader::db;

namespace {

template <typename Call>
DzErrorCode code_of(Call&& call) {
    try {
        call();
    } catch (const dztrader::Exception& e) {
        return e.code();
    }
    return DZ_EC_OK;
}

std::string raw_string(const std::filesystem::path& path, const std::string& sql) {
    SQLite::Database db(path.string(), SQLite::OPEN_READONLY);
    SQLite::Statement q(db, sql);
    return q.executeStep() ? q.getColumn(0).getString() : "";
}

// 集合名避开 td 旧迁移表 orders（其 v1..v5 建表含 order_ref/exchange_id 等额外 NOT NULL 列）
const std::vector<ResourceSchema>& orders_schema() {
    static const std::vector<ResourceSchema> kSchemas = {
        ResourceSchema{.name = "session_orders",
                       .fields = {{"id", ValueType::Int64, false, true, true},
                                  {"account_id", ValueType::String, false, false, false},
                                  {"trading_day", ValueType::String, false, false, false},
                                  {"order_id", ValueType::Int64, false, false, false},
                                  {"instrument_id", ValueType::String, false, false, false},
                                  {"price", ValueType::Float64, true, false, false},
                                  {"seq", ValueType::Int64, false, false, false}},
                       .indexes = {{"uq_orders", {"account_id", "order_id"}, true}}}};
    return kSchemas;
}

Row make_order(std::string account, int64_t order_id, std::string instrument, int64_t seq) {
    return Row{std::vector<Value>{std::monostate{}, std::move(account), std::string("20260917"),
                                  order_id, std::move(instrument), std::monostate{}, seq}};
}

}  // namespace

class SqliteSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = dztrader::test::unique_temp_path("dz_sqlite_session", "db");
        database_ = Database::open(
            Config{.backend = "sqlite", .options = {{"path", path_.string()}}}, orders_schema());
        database_->migrate();
        session_ = database_->session();
    }
    void TearDown() override {
        session_.reset();
        database_.reset();
        std::filesystem::remove(path_);
    }

    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
    std::unique_ptr<Session> session_;
};

TEST_F(SqliteSessionTest, UpsertIsIdempotentByKey) {
    const Row first = make_order("a", 1, "cu2601", 10);
    session_->upsert("session_orders", std::span<const Row>(&first, 1));
    const Row second = make_order("a", 1, "cu2601", 11);
    session_->upsert("session_orders", std::span<const Row>(&second, 1));
    const auto result =
        session_->find("session_orders", filters::eq("account_id", std::string("a")));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(6), 11);
}

TEST_F(SqliteSessionTest, FindSortsAndPagesWithSchemaColumns) {
    const Row rows[] = {make_order("a", 1, "cu", 1), make_order("a", 2, "cu", 2),
                        make_order("a", 3, "cu", 3)};
    session_->upsert("session_orders", rows);
    // 断言排序列本身 (seq=6): offset=0/limit=1 下升序必须得 1、降序必须得 3,
    // 原用例取 offset=1 且断言 order_id, 升降序/忽略 ORDER BY 都得同一个 2
    const auto descending = session_->find(
        "session_orders", {},
        FindOptions{.sort = {{"seq", SortOrder::Descending}}, .offset = 0, .limit = 1});
    ASSERT_EQ(descending.size(), 1u);
    EXPECT_EQ(descending.rows()[0].get<int64_t>(6), 3);
    const auto ascending = session_->find(
        "session_orders", {},
        FindOptions{.sort = {{"seq", SortOrder::Ascending}}, .offset = 0, .limit = 1});
    ASSERT_EQ(ascending.size(), 1u);
    EXPECT_EQ(ascending.rows()[0].get<int64_t>(6), 1);
    ASSERT_EQ(descending.columns().size(), 7u);
    EXPECT_EQ(descending.columns()[0].name, "id");
}

TEST_F(SqliteSessionTest, FindEmptyStillReturnsSchemaColumns) {
    const auto result =
        session_->find("session_orders", filters::eq("account_id", std::string("none")));
    EXPECT_TRUE(result.empty());
    ASSERT_EQ(result.columns().size(), 7u);
    EXPECT_EQ(result.columns()[6].name, "seq");
}

TEST_F(SqliteSessionTest, RemoveReturnsAffectedRows) {
    const Row rows[] = {make_order("a", 1, "cu", 1), make_order("b", 2, "cu", 2)};
    session_->upsert("session_orders", rows);
    EXPECT_EQ(session_->remove("session_orders", filters::eq("account_id", std::string("a"))), 1u);
    EXPECT_EQ(session_->find("session_orders").size(), 1u);
}

TEST_F(SqliteSessionTest, AggregateMaxGroupBy) {
    const Row rows[] = {make_order("a", 1, "cu", 10), make_order("a", 2, "cu", 20),
                        make_order("b", 3, "cu", 33)};
    session_->upsert("session_orders", rows);
    Aggregation aggregation;
    aggregation.op = AggregateOp::Max;
    aggregation.field = "seq";
    aggregation.group_by = {"account_id"};
    const auto result = session_->aggregate("session_orders", aggregation);
    ASSERT_EQ(result.size(), 2u);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "account_id");
    EXPECT_EQ(result.columns()[1].name, "max");
}

TEST_F(SqliteSessionTest, InAndRangeFilters) {
    // seq=25 在范围 [15,30) 内但 instrument 不在 in 集合: 删掉 in 条件结果会变 2 行
    const Row rows[] = {make_order("a", 1, "cu", 10), make_order("a", 2, "al", 20),
                        make_order("a", 3, "zn", 30), make_order("a", 4, "zn", 25)};
    session_->upsert("session_orders", rows);
    const Filter f = {filters::gte("seq", 15), filters::lt("seq", 30),
                      filters::in("instrument_id", {"cu", "al"})};
    const auto result = session_->find("session_orders", f);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<std::string>(4), "al");
    const Filter range_only = {filters::gte("seq", 15), filters::lt("seq", 30)};
    EXPECT_EQ(session_->find("session_orders", range_only).size(), 2u);
}

TEST_F(SqliteSessionTest, UnknownCollectionMapsToQueryFailed) {
    EXPECT_EQ(code_of([&] { (void)session_->find("no_such_collection"); }), DZ_EC_DB_QUERY_FAILED);
}

TEST_F(SqliteSessionTest, UnknownAggregateFieldMapsToQueryFailed) {
    Aggregation aggregation;
    aggregation.op = AggregateOp::Max;
    aggregation.field = "missing";
    EXPECT_EQ(code_of([&] { (void)session_->aggregate("session_orders", aggregation); }),
              DZ_EC_DB_QUERY_FAILED);
}

TEST_F(SqliteSessionTest, UnknownGroupByFieldMapsToQueryFailed) {
    Aggregation aggregation;
    aggregation.op = AggregateOp::Count;
    aggregation.group_by = {"missing"};
    EXPECT_EQ(code_of([&] { (void)session_->aggregate("session_orders", aggregation); }),
              DZ_EC_DB_QUERY_FAILED);
}

TEST_F(SqliteSessionTest, RealDeclaredColumnStoresIntegerAsReal) {
    // 事实校正: REAL affinity 会把整数 bind 转成 REAL 存储 (typeof='real'),
    // 原注释"3800.0 可能以 INTEGER 存储"不成立
    Row row = make_order("a", 9, "cu", 99);
    std::vector<Value> values(row.values().begin(), row.values().end());
    values[5] = int64_t{3800};
    const Row stored{std::move(values)};
    session_->upsert("session_orders", std::span<const Row>(&stored, 1));
    EXPECT_EQ(raw_string(path_, "SELECT typeof(price) FROM session_orders WHERE order_id=9"),
              "real");
    const auto result = session_->find("session_orders", filters::eq("order_id", 9));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(5), 3800.0);
}

// 声明类型为 Float64、实际列却按 INTEGER affinity 建表 (schema 与既有表漂移) 时,
// read_column 的 SQLITE_INTEGER 分支必须按 double 解码. 真实库中该形态无法由
// 正常 upsert 产生 (REAL affinity 必然转 REAL), 故用 raw SQLite 预建表制造存储类型.
TEST(SqliteSessionStorageTypeTest, ReadsIntegerStorageInRealDeclaredColumn) {
    const auto path = dztrader::test::unique_temp_path("dz_sqlite_session_int_storage", "db");
    {
        SQLite::Database db(path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(
            "CREATE TABLE session_orders ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    account_id TEXT NOT NULL,"
            "    trading_day TEXT NOT NULL,"
            "    order_id INTEGER NOT NULL,"
            "    instrument_id TEXT NOT NULL,"
            "    price INTEGER,"  // 与 schema 声明的 Float64 不同 -> INTEGER 存储
            "    seq INTEGER NOT NULL)");
    }
    auto database = Database::open(
        Config{.backend = "sqlite", .options = {{"path", path.string()}}}, orders_schema());
    database->migrate();  // 表已存在: create_missing_collections 跳过建表
    auto session = database->session();
    Row row = make_order("a", 9, "cu", 99);
    std::vector<Value> values(row.values().begin(), row.values().end());
    values[5] = int64_t{3800};
    const Row stored{std::move(values)};
    session->upsert("session_orders", std::span<const Row>(&stored, 1));
    ASSERT_EQ(raw_string(path, "SELECT typeof(price) FROM session_orders WHERE order_id=9"),
              "integer");
    const auto result = session->find("session_orders", filters::eq("order_id", 9));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(5), 3800.0);
    session.reset();
    database.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
}
