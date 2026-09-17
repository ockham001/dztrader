#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>

#include "conformance_main.h"

#include <filesystem>
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
    return db::Row{std::vector<db::Value>{std::monostate{}, std::move(account), order_id, seq}};
}

}  // namespace

const std::vector<db::ResourceSchema>& conformance_schemas() {
    static const std::vector<db::ResourceSchema> kSchemas = {
        db::ResourceSchema{.name = kCollection,
                           .fields = {{"id", db::ValueType::Int64, false, true, true},
                                      {"account_id", db::ValueType::String, false, false, false},
                                      {"order_id", db::ValueType::Int64, false, false, false},
                                      {"seq", db::ValueType::Int64, false, false, false}},
                           .indexes = {{"uq_orders", {"account_id", "order_id"}, true}}}};
    return kSchemas;
}

const std::vector<Driver>& drivers() {
    static const std::vector<Driver> kDrivers = {
        Driver{"sqlite",
               [](const std::filesystem::path& path,
                  std::span<const db::ResourceSchema> schemas) {
                   return db::Database::open(
                       db::Config{.backend = "sqlite", .options = {{"path", path.string()}}},
                       schemas);
               }},
    };
    return kDrivers;
}

class ConformanceTest : public ::testing::TestWithParam<size_t> {
protected:
    void SetUp() override {
        path_ = unique_db_path("dz_conformance");
        database_ = drivers()[GetParam()].make(path_, conformance_schemas());
        ASSERT_NE(database_, nullptr);
        database_->migrate();
        session_ = database_->session();
    }
    void TearDown() override {
        session_.reset();
        database_.reset();
        std::filesystem::remove(path_);
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
    const auto result = session_->find(kCollection, db::filters::eq("account_id", std::string("a")));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(3), 11);
}

TEST_P(ConformanceTest, FindSortPage) {
    const db::Row rows[] = {make_order("a", 1, 1), make_order("a", 2, 2), make_order("a", 3, 3)};
    session_->upsert(kCollection, rows);
    const auto result = session_->find(
        kCollection, {}, db::FindOptions{.sort = {{"seq", db::SortOrder::Descending}},
                                         .offset = 1, .limit = 1});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(3), 2);
    ASSERT_EQ(result.columns().size(), 4u);
    EXPECT_EQ(result.columns()[0].name, "id");
}

TEST_P(ConformanceTest, FindEmptyColumns) {
    const auto result =
        session_->find(kCollection, db::filters::eq("account_id", std::string("none")));
    EXPECT_TRUE(result.empty());
    ASSERT_EQ(result.columns().size(), 4u);
    EXPECT_EQ(result.columns()[3].name, "seq");
}

TEST_P(ConformanceTest, RemoveCount) {
    const db::Row rows[] = {make_order("a", 1, 1), make_order("b", 2, 2)};
    session_->upsert(kCollection, rows);
    EXPECT_EQ(session_->remove(kCollection, db::filters::eq("account_id", std::string("a"))), 1u);
    EXPECT_EQ(session_->find(kCollection).size(), 1u);
}

TEST_P(ConformanceTest, AggregateMaxGroupBy) {
    const db::Row rows[] = {make_order("a", 1, 10), make_order("a", 2, 20),
                            make_order("b", 3, 33)};
    session_->upsert(kCollection, rows);
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Max;
    aggregation.field = "seq";
    aggregation.group_by = {"account_id"};
    const auto result = session_->aggregate(kCollection, aggregation);
    ASSERT_EQ(result.size(), 2u);
    ASSERT_EQ(result.columns().size(), 2u);
    EXPECT_EQ(result.columns()[0].name, "account_id");
    EXPECT_EQ(result.columns()[1].name, "max");
}

TEST_P(ConformanceTest, AggregateCount) {
    const db::Row rows[] = {make_order("a", 1, 10), make_order("a", 2, 20),
                            make_order("b", 3, 33)};
    session_->upsert(kCollection, rows);
    db::Aggregation aggregation;
    aggregation.op = db::AggregateOp::Count;
    const auto result = session_->aggregate(kCollection, aggregation);
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result.columns().size(), 1u);
    EXPECT_EQ(result.columns()[0].name, "count");
    EXPECT_EQ(result.rows()[0].get<int64_t>(0), 3);
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
    EXPECT_THROW(session_->upsert(kCollection, std::span<const db::Row>(&late, 1)),
                 dztrader::Exception);
    auto writer = database_->session();
    writer->upsert(kCollection, std::span<const db::Row>(&late, 1));
    const auto result = session_->find(kCollection);
    EXPECT_EQ(result.size(), 1u) << "快照内不得看到快照开始后的提交";
    snapshot.reset();
    EXPECT_EQ(session_->find(kCollection).size(), 2u);
}

TEST_P(ConformanceTest, ReadOnlyRejectsWrites) {
    auto ro = database_->session(/*read_only=*/true);
    const db::Row row = make_order("a", 1, 1);
    EXPECT_THROW(ro->upsert(kCollection, std::span<const db::Row>(&row, 1)), dztrader::Exception);
    EXPECT_THROW((void)ro->begin_transaction(), dztrader::Exception);
    EXPECT_NO_THROW((void)ro->find(kCollection));
}

TEST_P(ConformanceTest, CapabilitiesCoverAllFour) {
    const db::Capability caps = database_->capabilities();
    EXPECT_TRUE(db::has(caps, db::Capability::Transactions));
    EXPECT_TRUE(db::has(caps, db::Capability::SnapshotRead));
    EXPECT_TRUE(db::has(caps, db::Capability::Upsert));
    EXPECT_TRUE(db::has(caps, db::Capability::OrderedRangeScan));
}

INSTANTIATE_TEST_SUITE_P(Drivers, ConformanceTest, ::testing::Range<size_t>(0, drivers().size()),
                         [](const ::testing::TestParamInfo<size_t>& info) {
                             return drivers()[info.param].name;
                         });

}  // namespace db_test
