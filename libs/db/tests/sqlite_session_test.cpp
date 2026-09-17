#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include <filesystem>
#include <random>

using namespace dztrader::db;

namespace {

std::filesystem::path unique_db_path(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
            std::to_string(dist(gen)) + ".db");
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
        path_ = unique_db_path("dz_sqlite_session");
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
    const auto result = session_->find("session_orders", filters::eq("account_id", std::string("a")));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(6), 11);
}

TEST_F(SqliteSessionTest, FindSortsAndPagesWithSchemaColumns) {
    const Row rows[] = {make_order("a", 1, "cu", 1), make_order("a", 2, "cu", 2),
                        make_order("a", 3, "cu", 3)};
    session_->upsert("session_orders", rows);
    const auto result = session_->find(
        "session_orders", {}, FindOptions{.sort = {{"seq", SortOrder::Descending}}, .offset = 1, .limit = 1});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(3), 2);
    ASSERT_EQ(result.columns().size(), 7u);
    EXPECT_EQ(result.columns()[0].name, "id");
}

TEST_F(SqliteSessionTest, FindEmptyStillReturnsSchemaColumns) {
    const auto result = session_->find("session_orders", filters::eq("account_id", std::string("none")));
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
    const Row rows[] = {make_order("a", 1, "cu", 10), make_order("a", 2, "al", 20),
                        make_order("a", 3, "zn", 30)};
    session_->upsert("session_orders", rows);
    const Filter f = {filters::gte("seq", 15), filters::lt("seq", 30),
                      filters::in("instrument_id", {"cu", "al"})};
    const auto result = session_->find("session_orders", f);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<std::string>(4), "al");
}

TEST_F(SqliteSessionTest, RealDeclaredColumnReadsIntegerStorageAsDouble) {
    // SQLite REAL affinity 下 3800.0 可能以 INTEGER 存储；按 schema 类型强制读为 double
    Row row = make_order("a", 9, "cu", 99);
    std::vector<Value> values(row.values().begin(), row.values().end());
    values[5] = int64_t{3800};
    const Row stored{std::move(values)};
    session_->upsert("session_orders", std::span<const Row>(&stored, 1));
    const auto result = session_->find("session_orders", filters::eq("order_id", 9));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(5), 3800.0);
}
