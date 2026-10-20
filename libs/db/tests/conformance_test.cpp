#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>

#include "conformance_main.h"

#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL
#include "mysql_test_env.h"
#endif

#include <filesystem>
#include <map>
#include <random>

namespace db_test {

namespace db = dztrader::db;

namespace {

std::filesystem::path unique_db_path(std::string_view name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (std::string(name) + "_" +
            std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
            std::to_string(dist(gen)) + ".db");
}

// 集合名避开 td 旧迁移表 orders（其 v1..v5 建表含 order_ref/exchange_id 等额外 NOT NULL 列）
constexpr std::string_view kCollection = "conformance_orders";

db::Row make_order(std::string account, int64_t order_id, int64_t seq) {
    return db::Row{std::vector<db::Value>{std::monostate{}, std::move(account), order_id, seq,
                                          std::monostate{}, true}};
}

/// 捕获异常码；未抛异常返回 DZ_EC_OK（负例断言具体码而非"抛了某种异常"）
template <typename Call>
DzErrorCode code_of(Call&& call) {
    try {
        call();
    } catch (const dztrader::Exception& e) {
        return e.code();
    }
    return DZ_EC_OK;
}

std::unique_ptr<db::Database> make_sqlite(const std::filesystem::path& path,
                                          std::span<const db::ResourceSchema> schemas) {
    return db::Database::open(db::Config{.backend = "sqlite", .options = {{"path", path.string()}}},
                              schemas);
}

#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL
/// path.stem() 即库名（dz_conformance_<pid>_<rand>）：自建库，用例结束由 cleanup 删库
std::unique_ptr<db::Database> make_mysql(const std::filesystem::path& path,
                                         std::span<const db::ResourceSchema> schemas) {
    const std::string database = path.stem().string();
    mysql_env::create_database(database);
    return db::Database::open(mysql_env::config_for(database, {{"schema_version", "0"}}), schemas);
}

void cleanup_mysql(const std::filesystem::path& path) {
    mysql_env::drop_database(path.stem().string());
}
#endif

}  // namespace

const std::vector<db::ResourceSchema>& conformance_schemas() {
    static const std::vector<db::ResourceSchema> kSchemas = {
        db::ResourceSchema{.name = kCollection,
                           .fields = {{"id", db::ValueType::Int64, false, true, true},
                                      {"account_id", db::ValueType::String, false, false, false},
                                      {"order_id", db::ValueType::Int64, false, false, false},
                                      {"seq", db::ValueType::Int64, false, false, false},
                                      {"price", db::ValueType::Float64, true, false, false},
                                      {"active", db::ValueType::Bool, false, false, false}},
                           .indexes = {{"uq_orders", {"account_id", "order_id"}, true}}}};
    return kSchemas;
}

const std::vector<Driver>& drivers() {
    static const std::vector<Driver> kDrivers = {
        Driver{"sqlite", make_sqlite, {}, false},
#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL
        Driver{"mysql", make_mysql, cleanup_mysql, true},
#endif
    };
    return kDrivers;
}

class ConformanceTest : public ::testing::TestWithParam<size_t> {
protected:
    void SetUp() override {
        const Driver& driver = drivers()[GetParam()];
#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL
        if (driver.requires_server && !mysql_env::configured()) {
            GTEST_SKIP() << "mysql test server not configured | env=DZ_TEST_MYSQL_HOST";
        }
#endif
        path_ = unique_db_path("dz_conformance");
        database_ = driver.make(path_, conformance_schemas());
        ASSERT_NE(database_, nullptr);
        database_->migrate();
        session_ = database_->session();
    }
    void TearDown() override {
        session_.reset();
        database_.reset();
        if (path_.empty()) {
            return;  // SetUp 跳过：无资源可清理
        }
        const Cleanup& cleanup = drivers()[GetParam()].cleanup;
        if (cleanup) {
            cleanup(path_);
        } else {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<db::Session> session_;
};

TEST_P(ConformanceTest, UpsertIdempotentByKey) {
    const db::Row first = make_order("a", 1, 10);
    session_->upsert(kCollection, std::span<const db::Row>(&first, 1));
    const db::Row second = make_order("a", 1, 11);
    session_->upsert(kCollection, std::span<const db::Row>(&second, 1));
    const auto result =
        session_->find(kCollection, db::filters::eq("account_id", std::string("a")));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(3), 11);
}

TEST_P(ConformanceTest, FindSortPage) {
    const db::Row rows[] = {make_order("a", 1, 1), make_order("a", 2, 2), make_order("a", 3, 3)};
    session_->upsert(kCollection, rows);
    const auto result = session_->find(
        kCollection, {},
        db::FindOptions{.sort = {{"seq", db::SortOrder::Descending}}, .offset = 1, .limit = 1});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(3), 2);
    ASSERT_EQ(result.columns().size(), 6u);
    EXPECT_EQ(result.columns()[0].name, "id");
}

TEST_P(ConformanceTest, FindEmptyColumns) {
    const auto result =
        session_->find(kCollection, db::filters::eq("account_id", std::string("none")));
    EXPECT_TRUE(result.empty());
    ASSERT_EQ(result.columns().size(), 6u);
    EXPECT_EQ(result.columns()[3].name, "seq");
    EXPECT_EQ(result.columns()[4].name, "price");
    EXPECT_EQ(result.columns()[5].name, "active");
}

// FIX-2：无 Filter 全量 find 必须按 schema 声明类型解码（String 仍是 string，
// Int64 仍是 int64_t），Row::values() 的 variant 类型断言即证据
TEST_P(ConformanceTest, FindAllDecodesDeclaredTypes) {
    const db::Row rows[] = {make_order("a", 7, 42)};
    session_->upsert(kCollection, rows);
    const auto result = session_->find(kCollection);
    ASSERT_EQ(result.size(), 1u);
    const db::Row& row = result.rows()[0];
    ASSERT_EQ(row.size(), 6u);
    EXPECT_EQ(std::get<int64_t>(row.values()[0]) > 0, true) << "自增 id 应为整数";
    EXPECT_EQ(std::get<std::string>(row.values()[1]), "a");
    EXPECT_EQ(row.get<int64_t>(2), 7);
    EXPECT_EQ(row.get<int64_t>(3), 42);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(row.values()[4]))
        << "nullable price 为 NULL";
    EXPECT_EQ(std::get<bool>(row.values()[5]), true);
}

// Bool / 可空 Float64 的跨驱动往返: NULL 必须保持 monostate, false 不得被读成 true
TEST_P(ConformanceTest, NullAndBoolRoundTrip) {
    const db::Row rows[] = {
        make_order("a", 1, 10),
        db::Row{std::vector<db::Value>{std::monostate{}, std::string("a"), int64_t{2}, int64_t{20},
                                       12.5, false}},
    };
    session_->upsert(kCollection, rows);
    const auto result = session_->find(
        kCollection, {}, db::FindOptions{.sort = {{"order_id", db::SortOrder::Ascending}}});
    ASSERT_EQ(result.size(), 2u);
    const db::Row& first = result.rows()[0];
    EXPECT_TRUE(std::holds_alternative<std::monostate>(first.values()[4]));
    EXPECT_EQ(std::get<bool>(first.values()[5]), true);
    const db::Row& second = result.rows()[1];
    EXPECT_DOUBLE_EQ(std::get<double>(second.values()[4]), 12.5);
    EXPECT_EQ(std::get<bool>(second.values()[5]), false);
    ASSERT_EQ(result.columns().size(), 6u);
    EXPECT_EQ(result.columns()[4].type, db::ValueType::Float64);
    EXPECT_EQ(result.columns()[5].type, db::ValueType::Bool);
}

TEST_P(ConformanceTest, RemoveCount) {
    const db::Row rows[] = {make_order("a", 1, 1), make_order("b", 2, 2)};
    session_->upsert(kCollection, rows);
    EXPECT_EQ(session_->remove(kCollection, db::filters::eq("account_id", std::string("a"))), 1u);
    EXPECT_EQ(session_->find(kCollection).size(), 1u);
}

TEST_P(ConformanceTest, AggregateMaxGroupBy) {
    const db::Row rows[] = {make_order("a", 1, 10), make_order("a", 2, 20), make_order("b", 3, 33)};
    session_->upsert(kCollection, rows);
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Max;
    aggregation.field = "seq";
    aggregation.group_by = {"account_id"};
    const auto result = session_->aggregate(kCollection, aggregation);
    ASSERT_EQ(result.size(), 2u);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "account_id");
    EXPECT_EQ(result.columns()[0].type, db::ValueType::String);
    EXPECT_EQ(result.columns()[1].name, "max");
    EXPECT_EQ(result.columns()[1].type, db::ValueType::Int64);
    std::map<std::string, int64_t> maxima;
    for (const db::Row& row : result.rows()) {
        maxima[row.get<std::string>(0)] = row.get<int64_t>(1);
    }
    ASSERT_EQ(maxima.size(), 2u);
    EXPECT_EQ(maxima["a"], 20);
    EXPECT_EQ(maxima["b"], 33);
}

TEST_P(ConformanceTest, AggregateCount) {
    const db::Row rows[] = {make_order("a", 1, 10), make_order("a", 2, 20), make_order("b", 3, 33)};
    session_->upsert(kCollection, rows);
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Count;
    const auto result = session_->aggregate(kCollection, aggregation);
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result.columns().size(), 1u);
    EXPECT_EQ(result.columns()[0].name, "count");
    EXPECT_EQ(result.columns()[0].type, db::ValueType::Int64);
    EXPECT_EQ(result.rows()[0].get<int64_t>(0), 3);
}

// FIX-2/FIX-14：SUM/AVG 的返回类型与值（sqlite INTEGER/REAL；mysql 侧 SUM 走 DECIMAL
// 字符串也必须按 schema 声明类型解码回 int64_t / double）
TEST_P(ConformanceTest, AggregateSumAvgTypes) {
    const db::Row rows[] = {make_order("a", 1, 10), make_order("a", 2, 20), make_order("b", 3, 33)};
    session_->upsert(kCollection, rows);

    db::Aggregation sum;
    sum.op = db::AggregateOp::Sum;
    sum.field = "seq";
    const auto sum_result = session_->aggregate(kCollection, sum);
    ASSERT_EQ(sum_result.size(), 1u);
    ASSERT_EQ(sum_result.columns().size(), 1u);
    EXPECT_EQ(sum_result.columns()[0].name, "sum");
    EXPECT_EQ(sum_result.columns()[0].type, db::ValueType::Int64);
    EXPECT_EQ(sum_result.rows()[0].get<int64_t>(0), 63);

    db::Aggregation avg;
    avg.op = db::AggregateOp::Avg;
    avg.field = "seq";
    const auto avg_result = session_->aggregate(kCollection, avg);
    ASSERT_EQ(avg_result.size(), 1u);
    ASSERT_EQ(avg_result.columns().size(), 1u);
    EXPECT_EQ(avg_result.columns()[0].name, "avg");
    EXPECT_EQ(avg_result.columns()[0].type, db::ValueType::Float64);
    EXPECT_DOUBLE_EQ(avg_result.rows()[0].get<double>(0), 21.0);
}

TEST_P(ConformanceTest, TransactionRollback) {
    {
        auto txn = session_->begin_transaction();
        const db::Row row = make_order("a", 1, 1);
        session_->upsert(kCollection, std::span<const db::Row>(&row, 1));
    }
    EXPECT_EQ(session_->find(kCollection).size(), 0u);
}

TEST_P(ConformanceTest, SnapshotStableView) {
    const db::Row seed[] = {make_order("a", 1, 10)};
    session_->upsert(kCollection, seed);
    auto snapshot = session_->begin_snapshot();
    const db::Row late = make_order("a", 2, 20);
    EXPECT_EQ(code_of([&] { session_->upsert(kCollection, std::span<const db::Row>(&late, 1)); }),
              DZ_EC_PERMISSION_DENIED);
    auto writer = database_->session();
    writer->upsert(kCollection, std::span<const db::Row>(&late, 1));
    const auto result = session_->find(kCollection);
    EXPECT_EQ(result.size(), 1u) << "快照内不得看到快照开始后的提交";
    snapshot.reset();
    EXPECT_EQ(session_->find(kCollection).size(), 2u);
}

// 快照作用域内的写守卫: remove 与 begin_transaction 同样必须拒绝
TEST_P(ConformanceTest, SnapshotRejectsRemoveAndTransaction) {
    auto snapshot = session_->begin_snapshot();
    EXPECT_EQ(code_of([&] { (void)session_->remove(kCollection, {}); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)session_->begin_transaction(); }), DZ_EC_PERMISSION_DENIED);
}

// 显式事务收尾: commit 后换 Session 必须可见, rollback 后必须不可见
TEST_P(ConformanceTest, TransactionCommitAndRollback) {
    {
        auto txn = session_->begin_transaction();
        const db::Row row = make_order("a", 1, 10);
        session_->upsert(kCollection, std::span<const db::Row>(&row, 1));
        txn->commit();
    }
    {
        auto verifier = database_->session();  // 另一连接: 只看得见已提交数据
        EXPECT_EQ(verifier->find(kCollection).size(), 1u);
    }
    {
        auto txn = session_->begin_transaction();
        const db::Row row = make_order("a", 2, 20);
        session_->upsert(kCollection, std::span<const db::Row>(&row, 1));
        txn->rollback();
    }
    auto verifier = database_->session();
    EXPECT_EQ(verifier->find(kCollection).size(), 1u);
}

TEST_P(ConformanceTest, NestedTransactionRejected) {
    auto txn = session_->begin_transaction();
    EXPECT_EQ(code_of([&] { (void)session_->begin_transaction(); }), DZ_EC_DB_TRANSACTION_FAILED);
    txn->rollback();
    auto retry = session_->begin_transaction();  // 拒绝后作用域仍可重新开启
    retry->commit();
}

TEST_P(ConformanceTest, ReadOnlyRejectsWrites) {
    auto ro = database_->session(/*read_only=*/true);
    const db::Row row = make_order("a", 1, 1);
    EXPECT_EQ(code_of([&] { ro->upsert(kCollection, std::span<const db::Row>(&row, 1)); }),
              DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)ro->remove(kCollection, {}); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)ro->begin_transaction(); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_NO_THROW((void)ro->find(kCollection));
}

TEST_P(ConformanceTest, UnregisteredCollectionMapsToQueryFailed) {
    const db::Row row = make_order("a", 1, 1);
    EXPECT_EQ(code_of([&] { session_->upsert("no_such", std::span<const db::Row>(&row, 1)); }),
              DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(code_of([&] { (void)session_->find("no_such"); }), DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(code_of([&] { (void)session_->remove("no_such", {}); }), DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(code_of([&] { (void)session_->aggregate("no_such", db::Aggregation{}); }),
              DZ_EC_DB_QUERY_FAILED);
}

// 白名单外的过滤字段: 两个驱动共用 compile_filter, 必须映射同一错误码
TEST_P(ConformanceTest, UnknownFilterFieldMapsToQueryFailed) {
    EXPECT_EQ(
        code_of([&] { (void)session_->find(kCollection, db::filters::eq("missing", int64_t{1})); }),
        DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(code_of([&] {
                  (void)session_->remove(kCollection, db::filters::eq("missing", int64_t{1}));
              }),
              DZ_EC_DB_QUERY_FAILED);
    db::Aggregation aggregation;
    aggregation.filter = db::filters::eq("missing", int64_t{1});
    EXPECT_EQ(code_of([&] { (void)session_->aggregate(kCollection, aggregation); }),
              DZ_EC_DB_QUERY_FAILED);
}

// 行宽不匹配: 写失败且不得留下部分数据 (批量路径的原子性)
TEST_P(ConformanceTest, RowWidthMismatchMapsToWriteFailed) {
    const db::Row short_row{std::vector<db::Value>{std::monostate{}, std::string("a"), int64_t{1},
                                                   int64_t{10}, std::monostate{}}};
    EXPECT_EQ(
        code_of([&] { session_->upsert(kCollection, std::span<const db::Row>(&short_row, 1)); }),
        DZ_EC_DB_WRITE_FAILED);
    EXPECT_EQ(session_->find(kCollection).size(), 0u);

    const db::Row long_row{std::vector<db::Value>{std::monostate{}, std::string("a"), int64_t{1},
                                                  int64_t{10}, std::monostate{}, true, int64_t{0}}};
    EXPECT_EQ(
        code_of([&] { session_->upsert(kCollection, std::span<const db::Row>(&long_row, 1)); }),
        DZ_EC_DB_WRITE_FAILED);
    EXPECT_EQ(session_->find(kCollection).size(), 0u);
}

TEST_P(ConformanceTest, CapabilitiesCoverAllFour) {
    const db::Capability caps = database_->capabilities();
    EXPECT_TRUE(db::has(caps, db::Capability::Transactions));
    EXPECT_TRUE(db::has(caps, db::Capability::SnapshotRead));
    EXPECT_TRUE(db::has(caps, db::Capability::Upsert));
    EXPECT_TRUE(db::has(caps, db::Capability::OrderedRangeScan));
}

INSTANTIATE_TEST_SUITE_P(Drivers,
                         ConformanceTest,
                         ::testing::Range<size_t>(0, drivers().size()),
                         [](const ::testing::TestParamInfo<size_t>& info) {
                             return drivers()[info.param].name;
                         });

}  // namespace db_test
