#include <gtest/gtest.h>

#include <dztrader/db/collection.h>

using namespace dztrader::db;

namespace {

class FakeSession final : public Session {
public:
    mutable std::vector<std::string> calls;
    ResultSet next_result;
    Filter last_filter;
    FindOptions last_options;
    Aggregation last_aggregation;
    size_t upserted = 0;

    void upsert(std::string_view collection, std::span<const Row> rows) override {
        calls.emplace_back("upsert:" + std::string(collection));
        upserted = rows.size();
    }
    uint64_t remove(std::string_view collection, const Filter& filter) override {
        calls.emplace_back("remove:" + std::string(collection));
        last_filter = filter;
        return 2;
    }
    ResultSet find(std::string_view collection, const Filter& filter,
                   const FindOptions& options) override {
        calls.emplace_back("find:" + std::string(collection));
        last_filter = filter;
        last_options = options;
        return next_result;
    }
    ResultSet aggregate(std::string_view collection, const Aggregation& aggregation) override {
        calls.emplace_back("aggregate:" + std::string(collection));
        last_aggregation = aggregation;
        return next_result;
    }
    std::unique_ptr<Transaction> begin_transaction() override { return nullptr; }
    std::unique_ptr<Snapshot> begin_snapshot() override { return nullptr; }
};

}  // namespace

TEST(DbCollectionTest, ForwardsOperations) {
    FakeSession session;
    Collection orders(session, "orders");

    const Row row{std::vector<Value>{int64_t{1}}};
    orders.upsert(std::span<const Row>(&row, 1));
    EXPECT_EQ(session.upserted, 1u);

    EXPECT_EQ(orders.remove(filters::eq("account_id", std::string("a"))), 2u);
    EXPECT_EQ(session.last_filter.conditions().size(), 1u);
}

TEST(DbCollectionTest, FindOneKeepsSortIgnoresPaging) {
    FakeSession session;
    session.next_result = ResultSet(
        std::vector<Column>{{"seq", ValueType::Int64}},
        std::vector<Row>{Row{std::vector<Value>{int64_t{5}}}, Row{std::vector<Value>{int64_t{9}}}});
    Collection orders(session, "orders");

    const auto row = orders.find_one({}, FindOptions{.sort = {{"seq", SortOrder::Descending}},
                                                     .offset = 3, .limit = 10});
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->get<int64_t>(0), 5);
    EXPECT_EQ(session.last_options.limit, 1);
    EXPECT_EQ(session.last_options.offset, 0);
}

TEST(DbCollectionTest, CountUsesAggregate) {
    FakeSession session;
    session.next_result = ResultSet(std::vector<Column>{{"count", ValueType::Int64}},
                                    std::vector<Row>{Row{std::vector<Value>{int64_t{7}}}});
    Collection orders(session, "orders");

    EXPECT_EQ(orders.count(filters::eq("account_id", std::string("a"))), 7u);
    EXPECT_EQ(session.last_aggregation.op, AggregateOp::Count);
}

TEST(DbCapabilityTest, HasRequiresAllBits) {
    const Capability both = Capability::Transactions | Capability::SnapshotRead;
    EXPECT_TRUE(has(both, Capability::Transactions));
    EXPECT_TRUE(has(both, both));
    EXPECT_FALSE(has(both, both | Capability::Upsert));
    EXPECT_FALSE(has(both, Capability::None));  // None 无位，全位语义下不成立
}
