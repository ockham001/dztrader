#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include <filesystem>
#include <random>

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

std::filesystem::path unique_db_path(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)) + ".db");
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

class SqliteTxnTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = unique_db_path("dz_sqlite_txn");
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

TEST_F(SqliteTxnTest, TransactionRollbackOnDestruct) {
    {
        auto txn = session_->begin_transaction();
        const Row row = make_order("a", 1, "cu", 1);
        session_->upsert("session_orders", std::span<const Row>(&row, 1));
    }
    EXPECT_EQ(session_->find("session_orders").size(), 0u);
}

TEST_F(SqliteTxnTest, TransactionCommitPersists) {
    auto txn = session_->begin_transaction();
    const Row row = make_order("a", 1, "cu", 1);
    session_->upsert("session_orders", std::span<const Row>(&row, 1));
    txn->commit();
    txn.reset();  // 先释放事务句柄, 再用另一连接验证: commit 空操作时此处必须为 0
    auto verifier = database_->session();
    EXPECT_EQ(verifier->find("session_orders").size(), 1u);
}

// 批量 upsert 第二行列数错误: 整批回滚, 第一行不得落库 (owned scope 的失败分支)
TEST_F(SqliteTxnTest, BatchUpsertRollsBackOnSecondRowWidthMismatch) {
    const Row first = make_order("a", 1, "cu", 1);
    const Row second{std::vector<Value>{std::monostate{}, std::string("a"), std::string("20260917"),
                                        int64_t{2}, std::string("cu"),
                                        std::monostate{}}};  // 6 != 7
    const Row rows[] = {first, second};
    EXPECT_EQ(code_of([&] { session_->upsert("session_orders", rows); }), DZ_EC_DB_WRITE_FAILED);
    EXPECT_EQ(session_->find("session_orders").size(), 0u);
}

TEST_F(SqliteTxnTest, CommitTwiceRejected) {
    auto txn = session_->begin_transaction();
    txn->commit();
    EXPECT_THROW(txn->commit(), dztrader::Exception);
}

TEST_F(SqliteTxnTest, NestedScopeRejected) {
    auto txn = session_->begin_transaction();
    EXPECT_THROW((void)session_->begin_snapshot(), dztrader::Exception);
    EXPECT_THROW((void)session_->begin_transaction(), dztrader::Exception);
    txn->rollback();
    auto retry = session_->begin_transaction();  // 拒绝嵌套不得卡死作用域
    retry->commit();
    EXPECT_EQ(session_->find("session_orders").size(), 0u);
}

TEST_F(SqliteTxnTest, BeginTransactionBusyIsWrappedAndKeepsScopeFree) {
    auto locker = database_->session();
    auto lock_txn = locker->begin_transaction();  // 持 WAL 写锁
    auto no_wait_db =
        Database::open(Config{.backend = "sqlite",
                              .options = {{"path", path_.string()}, {"busy_timeout_ms", "0"}}},
                       orders_schema());
    auto victim = no_wait_db->session();

    DzErrorCode code = DZ_EC_OK;
    try {
        (void)victim->begin_transaction();  // BEGIN IMMEDIATE 立即 BUSY
    } catch (const dztrader::Exception& e) {
        code = e.code();
    }
    EXPECT_EQ(code, DZ_EC_DB_TRANSACTION_FAILED);

    lock_txn->rollback();
    auto retry = victim->begin_transaction();  // BEGIN 失败未卡死 scope_
    retry->commit();
}

TEST_F(SqliteTxnTest, SnapshotSeesStableViewAndRejectsWrites) {
    const Row seed[] = {make_order("a", 1, "cu", 10)};
    session_->upsert("session_orders", seed);
    auto snapshot = session_->begin_snapshot();
    const Row late = make_order("a", 2, "cu", 20);
    EXPECT_EQ(code_of([&] { session_->upsert("session_orders", std::span<const Row>(&late, 1)); }),
              DZ_EC_PERMISSION_DENIED);
    // 快照内的 remove 与 begin_transaction 同样必须拒绝
    EXPECT_EQ(code_of([&] { (void)session_->remove("session_orders", {}); }),
              DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)session_->begin_transaction(); }), DZ_EC_PERMISSION_DENIED);
    auto writer = database_->session();
    writer->upsert("session_orders", std::span<const Row>(&late, 1));
    const auto result = session_->find("session_orders");
    EXPECT_EQ(result.size(), 1u) << "快照内不得看到快照开始后的提交";
    snapshot.reset();
    EXPECT_EQ(session_->find("session_orders").size(), 2u);
}

TEST_F(SqliteTxnTest, ReadOnlySessionRejectsWrites) {
    auto ro = database_->session(/*read_only=*/true);
    const Row row = make_order("a", 1, "cu", 1);
    EXPECT_EQ(code_of([&] { ro->upsert("session_orders", std::span<const Row>(&row, 1)); }),
              DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)ro->remove("session_orders", {}); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)ro->begin_transaction(); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_NO_THROW((void)ro->find("session_orders"));
}
