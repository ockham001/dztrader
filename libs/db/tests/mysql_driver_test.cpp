// MySQL 驱动集成测试：连接/DDL 形状/版本表/upsert 幂等/事务/快照/聚合/只读/类型归一化/错误码
// 服务器未配置（DZ_TEST_MYSQL_HOST 为空）时用例 GTEST_SKIP；配置后必须全部通过。
// 已知跨后端差异（Lead Q2 裁定）：非有限 double 只在 MySQL 侧拒绝绑定（DZ_EC_INVALID_PARAM），
// sqlite 侧仍静默接受（NaN -> NULL），故一致性套件不做 NaN 的共同断言。
#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL

#include <gtest/gtest.h>

#include <dztrader/core/exception.h>
#include <dztrader/db/database.h>
#include <dztrader/error.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/mysql_driver.h"
#include "mysql_test_env.h"

namespace db = dztrader::db;
namespace internal_ns = dztrader::db::internal;
namespace mysql_env = db_test::mysql_env;

namespace {

using db::AggregateOp;
using db::ValueType;

constexpr std::string_view kCollection = "mysql_probe";

const std::vector<db::ResourceSchema>& probe_schemas() {
    static const std::vector<db::ResourceSchema> kSchemas = {
        db::ResourceSchema{.name = kCollection,
                           .fields = {{"id", ValueType::Int64, false, true, true},
                                      {"account_id", ValueType::String, false, false, false},
                                      {"seq", ValueType::Int64, false, false, false},
                                      {"price", ValueType::Float64, true, false, false},
                                      {"active", ValueType::Bool, false, false, false},
                                      {"note", ValueType::String, true, false, false}},
                           .indexes = {{"uq_probe", {"account_id", "seq"}, true}}}};
    return kSchemas;
}

/// 6 列行：id(NULL 自增) / account_id / seq / price / active / note
db::Row make_row(std::string account, int64_t seq, db::Value price, bool active, db::Value note) {
    return db::Row{std::vector<db::Value>{std::monostate{}, std::move(account), seq,
                                          std::move(price), active, std::move(note)}};
}

void upsert_one(db::Session& session, db::Row row) {
    session.upsert(kCollection, std::span<const db::Row>(&row, 1));
}

template <typename Call>
DzErrorCode code_of(Call&& call) {
    try {
        call();
    } catch (const dztrader::Exception& e) {
        return e.code();
    }
    return DZ_EC_OK;
}

}  // namespace

class MysqlDriverTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysql_env::configured()) {
            GTEST_SKIP() << "mysql test server not configured | env=DZ_TEST_MYSQL_HOST";
        }
        database_name_ = mysql_env::unique_database_name("dz_test");
        mysql_env::create_database(database_name_);
    }

    void TearDown() override {
        session_.reset();
        database_.reset();
        if (!database_name_.empty()) {
            mysql_env::drop_database(database_name_);
        }
    }

    /// 打开库（不迁移；调用方决定 migrate 时机）
    void open(const std::string& version = "1",
              const std::map<std::string, std::string, std::less<>>& extra = {}) {
        std::map<std::string, std::string, std::less<>> options = extra;
        options["schema_version"] = version;
        database_ =
            db::Database::open(mysql_env::config_for(database_name_, options), probe_schemas());
    }

    /// 打开 + 迁移 + 建会话
    void open_migrated(const std::string& version = "1") {
        open(version);
        database_->migrate();
        session_ = database_->session();
    }

    std::string database_name_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<db::Session> session_;
};

// ---------------------------------------------------------------- 选项解析（无需服务器）

TEST(MysqlOptionsTest, Defaults) {
    const internal_ns::MysqlOptions options = internal_ns::parse_mysql_options({});
    EXPECT_EQ(options.host, "127.0.0.1");
    EXPECT_EQ(options.port, 3306);
    EXPECT_EQ(options.database, "dztrader");
    EXPECT_EQ(options.user, "");
    EXPECT_EQ(options.connect_timeout.count(), 5000);
    EXPECT_EQ(options.read_timeout.count(), 0);
    EXPECT_EQ(options.write_timeout.count(), 0);
    EXPECT_EQ(options.ssl_mode, internal_ns::MysqlSslMode::Preferred);
    EXPECT_FALSE(options.schema_version.has_value()) << "migrate 才要求显式版本";
}

TEST(MysqlOptionsTest, ParsesAllKeys) {
    const std::map<std::string, std::string, std::less<>> raw = {
        {"host", "10.0.0.1"},
        {"port", "3307"},
        {"user", "dz"},
        {"password", "secret"},
        {"database", "td"},
        {"connect_timeout_ms", "1500"},
        {"read_timeout_ms", "2500"},
        {"write_timeout_ms", "3500"},
        {"schema_version", "5"},
        {"ssl_mode", "verify_identity"},
    };
    const internal_ns::MysqlOptions options = internal_ns::parse_mysql_options(raw);
    EXPECT_EQ(options.host, "10.0.0.1");
    EXPECT_EQ(options.port, 3307);
    EXPECT_EQ(options.user, "dz");
    EXPECT_EQ(options.password, "secret");
    EXPECT_EQ(options.database, "td");
    EXPECT_EQ(options.connect_timeout.count(), 1500);
    EXPECT_EQ(options.read_timeout.count(), 2500);
    EXPECT_EQ(options.write_timeout.count(), 3500);
    ASSERT_TRUE(options.schema_version.has_value());
    EXPECT_EQ(*options.schema_version, 5);
    EXPECT_EQ(options.ssl_mode, internal_ns::MysqlSslMode::VerifyIdentity);

    // 未知键忽略（向前兼容新驱动选项）
    EXPECT_EQ(internal_ns::parse_mysql_options({{"future_key", "x"}}).port, 3306);
}

TEST(MysqlOptionsTest, RejectsInvalidValues) {
    for (const std::string port : {"0", "65536", "abc", "-1"}) {
        EXPECT_EQ(code_of([&] { (void)internal_ns::parse_mysql_options({{"port", port}}); }),
                  DZ_EC_INVALID_PARAM)
            << "port=" << port;
    }
    EXPECT_EQ(
        code_of([&] { (void)internal_ns::parse_mysql_options({{"connect_timeout_ms", "-5"}}); }),
        DZ_EC_INVALID_PARAM);
    EXPECT_EQ(code_of([&] { (void)internal_ns::parse_mysql_options({{"schema_version", "abc"}}); }),
              DZ_EC_INVALID_PARAM);
    EXPECT_EQ(code_of([&] { (void)internal_ns::parse_mysql_options({{"ssl_mode", "bogus"}}); }),
              DZ_EC_INVALID_PARAM);
}

// ---------------------------------------------------------------- 连接与 DDL

TEST_F(MysqlDriverTest, MigrateCreatesDdlShape) {
    open_migrated();
    // utf8mb4_bin 是对齐 SQLite BINARY 比较的关键（唯一键/过滤语义）
    const auto collation = mysql_env::scalar(
        database_name_,
        "SELECT COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND "
        "TABLE_NAME='mysql_probe' AND COLUMN_NAME='account_id'");
    ASSERT_TRUE(collation.has_value());
    EXPECT_EQ(*collation, "utf8mb4_bin");
    const auto engine = mysql_env::scalar(
        database_name_,
        "SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND "
        "TABLE_NAME='mysql_probe'");
    ASSERT_TRUE(engine.has_value());
    EXPECT_EQ(*engine, "InnoDB");
    const auto row_format = mysql_env::scalar(
        database_name_,
        "SELECT ROW_FORMAT FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND "
        "TABLE_NAME='mysql_probe'");
    ASSERT_TRUE(row_format.has_value());
    EXPECT_EQ(*row_format, "Dynamic");
    const auto bool_type = mysql_env::scalar(
        database_name_,
        "SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND "
        "TABLE_NAME='mysql_probe' AND COLUMN_NAME='active'");
    ASSERT_TRUE(bool_type.has_value());
    EXPECT_EQ(*bool_type, "tinyint(1)");
    const auto price_type = mysql_env::scalar(
        database_name_,
        "SELECT COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND "
        "TABLE_NAME='mysql_probe' AND COLUMN_NAME='price'");
    ASSERT_TRUE(price_type.has_value());
    EXPECT_EQ(*price_type, "double");
}

TEST_F(MysqlDriverTest, MigrateWritesVersionOnceAndIsIdempotent) {
    open("3");
    database_->migrate();
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT MAX(version) FROM schema_version"),
              std::optional<std::string>("3"));
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM schema_version"),
              std::optional<std::string>("1"));
    const auto applied_at =
        mysql_env::scalar(database_name_, "SELECT applied_at FROM schema_version WHERE version=3");
    ASSERT_TRUE(applied_at.has_value());
    EXPECT_EQ(applied_at->size(), 20u) << "applied_at 形状对齐 sqlite 的 %FT%TZ";
    EXPECT_EQ(applied_at->back(), 'Z');

    database_->migrate();  // 幂等：不新增版本行
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM schema_version"),
              std::optional<std::string>("1"));
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT MAX(version) FROM schema_version"),
              std::optional<std::string>("3"));
}

TEST_F(MysqlDriverTest, MigrateRejectsVersionSkew) {
    open("3");
    database_->migrate();
    // 库落后于代码：不静默按新 schema 跑
    auto older = db::Database::open(
        mysql_env::config_for(database_name_, {{"schema_version", "2"}}), probe_schemas());
    EXPECT_EQ(code_of([&] { older->migrate(); }), DZ_EC_DB_MIGRATE_FAILED);
    // 库比代码新
    auto newer = db::Database::open(
        mysql_env::config_for(database_name_, {{"schema_version", "4"}}), probe_schemas());
    EXPECT_EQ(code_of([&] { newer->migrate(); }), DZ_EC_DB_MIGRATE_FAILED);
    // 同版本幂等通过
    auto same = db::Database::open(mysql_env::config_for(database_name_, {{"schema_version", "3"}}),
                                   probe_schemas());
    EXPECT_NO_THROW(same->migrate());
}

// ---------------------------------------------------------------- 无服务器用例
// migrate 的版本校验与选项解析都在建连之前完成，必须在 CI 实跑（不依赖 DZ_TEST_MYSQL_*）

TEST(MysqlNoServerTest, MigrateRequiresExplicitSchemaVersion) {
    const db::Config no_version{.backend = "mysql",
                                .options = {{"host", "127.0.0.1"}, {"database", "dz_no_server"}}};
    auto database = db::Database::open(no_version, probe_schemas());
    EXPECT_EQ(code_of([&] { database->migrate(); }), DZ_EC_INVALID_PARAM);

    const db::Config bad_version{
        .backend = "mysql",
        .options = {
            {"host", "127.0.0.1"}, {"database", "dz_no_server"}, {"schema_version", "abc"}}};
    EXPECT_EQ(code_of([&] { (void)db::Database::open(bad_version, probe_schemas()); }),
              DZ_EC_INVALID_PARAM);
}

TEST(MysqlNoServerTest, ConnectFailuresMapToOpenFailed) {
    // 127.0.0.1:1 无监听（拒绝连接）：session() 建连即失败，映射为 DZ_EC_DB_OPEN_FAILED
    const db::Config refused{.backend = "mysql",
                             .options = {{"host", "127.0.0.1"},
                                         {"port", "1"},
                                         {"connect_timeout_ms", "1000"},
                                         {"schema_version", "1"}}};
    auto database = db::Database::open(refused, probe_schemas());
    EXPECT_EQ(code_of([&] { (void)database->session(); }), DZ_EC_DB_OPEN_FAILED);
}

// ---------------------------------------------------------------- 连接与 DDL

TEST_F(MysqlDriverTest, ConnectFailuresWithServerMapToOpenFailed) {
    auto wrong_password = db::Database::open(
        mysql_env::config_for(database_name_,
                              {{"password", "definitely-wrong"}, {"schema_version", "1"}}),
        probe_schemas());
    EXPECT_EQ(code_of([&] { (void)wrong_password->session(); }), DZ_EC_DB_OPEN_FAILED);

    auto missing_database =
        db::Database::open(mysql_env::config_for(mysql_env::unique_database_name("dz_missing"),
                                                 {{"schema_version", "1"}}),
                           probe_schemas());
    EXPECT_EQ(code_of([&] { (void)missing_database->session(); }), DZ_EC_DB_OPEN_FAILED);
}

TEST_F(MysqlDriverTest, UnregisteredCollectionMapsToQueryFailed) {
    open_migrated();
    EXPECT_EQ(code_of([&] { (void)session_->find("no_such_collection"); }), DZ_EC_DB_QUERY_FAILED);
    EXPECT_EQ(code_of([&] { (void)session_->find(kCollection, db::filters::eq("missing", 1)); }),
              DZ_EC_DB_QUERY_FAILED);
}

// ---------------------------------------------------------------- 读写语义

TEST_F(MysqlDriverTest, UpsertIdempotentByUniqueKey) {
    open_migrated();
    upsert_one(*session_, make_row("a", 5, 1.25, true, std::monostate{}));
    upsert_one(*session_, make_row("a", 5, 2.5, false, std::string("updated")));
    const auto result = session_->find(kCollection);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(3), 2.5);
    EXPECT_EQ(result.rows()[0].get<bool>(4), false);
    EXPECT_EQ(result.rows()[0].get<std::string>(5), "updated");
}

TEST_F(MysqlDriverTest, FindAllDecodesDeclaredTypes) {
    open_migrated();
    upsert_one(*session_, make_row("a", 5, 1.5, true, std::string("note")));
    const auto result = session_->find(kCollection);
    ASSERT_EQ(result.size(), 1u);
    const db::Row& row = result.rows()[0];
    ASSERT_EQ(row.size(), 6u);
    EXPECT_GT(std::get<int64_t>(row.values()[0]), 0) << "自增主键回读为 int64_t";
    EXPECT_EQ(std::get<std::string>(row.values()[1]), "a");
    EXPECT_EQ(std::get<int64_t>(row.values()[2]), 5);
    EXPECT_DOUBLE_EQ(std::get<double>(row.values()[3]), 1.5);
    EXPECT_TRUE(std::get<bool>(row.values()[4]));
    EXPECT_EQ(std::get<std::string>(row.values()[5]), "note");
}

TEST_F(MysqlDriverTest, NullRoundTripAndDoublePrecision) {
    open_migrated();
    const double precise = 1.2345678901234567;
    upsert_one(*session_, make_row("a", 5, precise, true, std::monostate{}));
    const auto result = session_->find(kCollection);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_DOUBLE_EQ(result.rows()[0].get<double>(3), precise);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(result.rows()[0].values()[5]));
}

TEST_F(MysqlDriverTest, FindOffsetWithoutLimit) {
    open_migrated();
    for (int64_t seq = 1; seq <= 3; ++seq) {
        upsert_one(*session_, make_row("a", seq, 1.0, true, std::monostate{}));
    }
    const auto result = session_->find(
        kCollection, {},
        db::FindOptions{.sort = {{"seq", db::SortOrder::Ascending}}, .offset = 1, .limit = -1});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result.rows()[0].get<int64_t>(2), 2);
    EXPECT_EQ(result.rows()[1].get<int64_t>(2), 3);
}

TEST_F(MysqlDriverTest, RemoveCountsAffectedRows) {
    open_migrated();
    upsert_one(*session_, make_row("a", 1, 1.0, true, std::monostate{}));
    upsert_one(*session_, make_row("b", 2, 1.0, true, std::monostate{}));
    EXPECT_EQ(session_->remove(kCollection, db::filters::eq("account_id", std::string("a"))), 1u);
    EXPECT_EQ(session_->find(kCollection).size(), 1u);
    EXPECT_EQ(session_->remove(kCollection, db::Filter{}), 1u) << "无 WHERE 即清表";
    EXPECT_EQ(session_->find(kCollection).size(), 0u);
}

TEST_F(MysqlDriverTest, UpsertBatchIsAtomicAndEmptySpanIsNoOp) {
    open_migrated();
    const db::Row valid = make_row("a", 1, 1.0, true, std::monostate{});
    // 第二行字段数不足：首行已执行，必须由驱动自开事务整体回滚
    const db::Row short_row{std::vector<db::Value>{std::monostate{}, std::string("b")}};
    const db::Row batch[] = {valid, short_row};
    EXPECT_EQ(code_of([&] { session_->upsert(kCollection, batch); }), DZ_EC_DB_WRITE_FAILED);
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM mysql_probe"),
              std::optional<std::string>("0"))
        << "second row failure must roll back the first row";

    EXPECT_NO_THROW(session_->upsert(kCollection, std::span<const db::Row>{}));
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM mysql_probe"),
              std::optional<std::string>("0"))
        << "empty span is a no-op";
}

// ---------------------------------------------------------------- 事务与快照

TEST_F(MysqlDriverTest, TransactionCommitRollbackAndNestedRejection) {
    open_migrated();
    {
        auto txn = session_->begin_transaction();
        upsert_one(*session_, make_row("a", 1, 1.0, true, std::monostate{}));
        txn->commit();
    }
    EXPECT_EQ(session_->find(kCollection).size(), 1u);

    {
        auto txn = session_->begin_transaction();
        upsert_one(*session_, make_row("b", 2, 1.0, true, std::monostate{}));
    }  // 析构未 commit -> 回滚
    EXPECT_EQ(session_->find(kCollection).size(), 1u);

    {
        auto txn = session_->begin_transaction();
        upsert_one(*session_, make_row("c", 3, 1.0, true, std::monostate{}));
        txn->rollback();
    }
    EXPECT_EQ(session_->find(kCollection).size(), 1u);

    {
        auto txn = session_->begin_transaction();
        EXPECT_EQ(code_of([&] { (void)session_->begin_transaction(); }),
                  DZ_EC_DB_TRANSACTION_FAILED)
            << "事务作用域内再开事务";
        txn->rollback();
    }
    {
        auto snapshot = session_->begin_snapshot();
        EXPECT_EQ(code_of([&] { (void)session_->begin_transaction(); }), DZ_EC_PERMISSION_DENIED)
            << "快照作用域内再开事务（对齐 sqlite 的不对称现状）";
    }
}

TEST_F(MysqlDriverTest, SnapshotIsolationAcrossSessions) {
    open_migrated();
    upsert_one(*session_, make_row("a", 1, 1.0, true, std::monostate{}));
    auto snapshot = session_->begin_snapshot();
    EXPECT_EQ(
        code_of([&] { upsert_one(*session_, make_row("b", 2, 1.0, true, std::monostate{})); }),
        DZ_EC_PERMISSION_DENIED)
        << "快照作用域内禁止写";
    auto writer = database_->session();
    upsert_one(*writer, make_row("b", 2, 1.0, true, std::monostate{}));
    EXPECT_EQ(session_->find(kCollection).size(), 1u) << "快照内不得看到快照开始后的提交";
    snapshot.reset();
    EXPECT_EQ(session_->find(kCollection).size(), 2u);
}

TEST_F(MysqlDriverTest, ReadOnlySessionRejectsWrites) {
    open_migrated();
    upsert_one(*session_, make_row("a", 1, 1.0, true, std::monostate{}));
    session_.reset();
    auto read_only = database_->session(/*read_only=*/true);
    EXPECT_EQ(
        code_of([&] { upsert_one(*read_only, make_row("b", 2, 1.0, true, std::monostate{})); }),
        DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)read_only->remove(kCollection, db::Filter{}); }),
              DZ_EC_PERMISSION_DENIED);
    EXPECT_EQ(code_of([&] { (void)read_only->begin_transaction(); }), DZ_EC_PERMISSION_DENIED);
    EXPECT_NO_THROW((void)read_only->find(kCollection));
    EXPECT_NO_THROW((void)read_only->begin_snapshot());
}

// ---------------------------------------------------------------- 类型归一化

TEST_F(MysqlDriverTest, StringColumnBindsNumbersAsText) {
    open_migrated();
    upsert_one(*session_, make_row("1", 1, 1.0, true, std::monostate{}));
    upsert_one(*session_, make_row("01", 2, 1.0, true, std::monostate{}));
    // 数值参数绑到 String 列 -> 按字符串比较；若按数字比较 '1' 会同时命中 '01'
    const auto result = session_->find(kCollection, db::filters::eq("account_id", int64_t{1}));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.rows()[0].get<std::string>(1), "1");
    // 反向：String 参数绑到 Int64 列 -> 归一化为整数
    const auto by_text = session_->find(kCollection, db::filters::eq("seq", std::string("2")));
    ASSERT_EQ(by_text.size(), 1u);
    EXPECT_EQ(by_text.rows()[0].get<std::string>(1), "01");
}

TEST_F(MysqlDriverTest, NonIntegerStringForIntColumnIsRejected) {
    open_migrated();
    EXPECT_EQ(code_of([&] {
                  (void)session_->find(kCollection, db::filters::eq("seq", std::string("abc")));
              }),
              DZ_EC_INVALID_PARAM);
    EXPECT_EQ(code_of([&] {
                  (void)session_->find(kCollection, db::filters::eq("seq", std::string("1.5")));
              }),
              DZ_EC_INVALID_PARAM);
}

TEST_F(MysqlDriverTest, BoolColumnAcceptsIntegerNormalization) {
    open_migrated();
    // Bool 列（TINYINT(1)）绑 int64 -> 归一化为 0/1
    db::Row raw{std::vector<db::Value>{std::monostate{}, std::string("a"), int64_t{1},
                                       std::monostate{}, int64_t{1}, std::monostate{}}};
    session_->upsert(kCollection, std::span<const db::Row>(&raw, 1));
    const auto result = session_->find(kCollection);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result.rows()[0].get<bool>(4));
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM mysql_probe WHERE active=1"),
              std::optional<std::string>("1"));
}

TEST_F(MysqlDriverTest, NonFiniteDoubleIsRejected) {
    open_migrated();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(
        code_of([&] { upsert_one(*session_, make_row("a", 1, nan, true, std::monostate{})); }),
        DZ_EC_INVALID_PARAM);
    EXPECT_EQ(
        code_of([&] { upsert_one(*session_, make_row("b", 2, inf, true, std::monostate{})); }),
        DZ_EC_INVALID_PARAM);
    // 归一化前统一检查：非有限 double 绑到 String 列同样拒绝
    EXPECT_EQ(code_of([&] { upsert_one(*session_, make_row("c", 3, 1.0, true, nan)); }),
              DZ_EC_INVALID_PARAM);
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM mysql_probe"),
              std::optional<std::string>("0"));
}

TEST_F(MysqlDriverTest, StrictModeRejectsOverlongString) {
    open_migrated();
    const std::string overlong(300, 'x');  // VARCHAR(255) 超长
    EXPECT_EQ(code_of([&] { upsert_one(*session_, make_row("a", 1, 1.0, true, overlong)); }),
              DZ_EC_DB_WRITE_FAILED)
        << "STRICT_ALL_TABLES 下必须响亮失败，不得静默截断";
    EXPECT_EQ(mysql_env::scalar(database_name_, "SELECT COUNT(*) FROM mysql_probe"),
              std::optional<std::string>("0"));
}

// 超时口径（spec §7.1）：write_timeout_ms 生效的证据 —— 行锁等待必须被截止时间打断，
// 否则会一直等到 innodb_lock_wait_timeout（默认 50s）
TEST_F(MysqlDriverTest, WriteTimeoutFiresOnLockWait) {
    open_migrated();
    auto blocker = database_->session();
    auto blocker_txn = blocker->begin_transaction();
    upsert_one(*blocker, make_row("a", 1, 1.0, true, std::monostate{}));  // 持有唯一键行锁

    auto waiting_database = db::Database::open(
        mysql_env::config_for(database_name_,
                              {{"schema_version", "1"}, {"write_timeout_ms", "500"}}),
        probe_schemas());
    auto waiting = waiting_database->session();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(code_of([&] { upsert_one(*waiting, make_row("a", 1, 2.0, true, std::monostate{})); }),
              DZ_EC_DB_WRITE_FAILED);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(5)) << "write_timeout_ms 未生效：行锁等待未被截止";
    blocker_txn->rollback();
}

// ---------------------------------------------------------------- 聚合

TEST_F(MysqlDriverTest, AggregateTypesAndValues) {
    open_migrated();
    upsert_one(*session_, make_row("a", 10, 1.5, true, std::monostate{}));
    upsert_one(*session_, make_row("a", 20, 2.5, true, std::monostate{}));
    upsert_one(*session_, make_row("b", 33, 3.5, true, std::monostate{}));

    db::Aggregation count;
    count.op = AggregateOp::Count;
    const auto count_result = session_->aggregate(kCollection, count);
    ASSERT_EQ(count_result.size(), 1u);
    EXPECT_EQ(count_result.columns()[0].type, ValueType::Int64);
    EXPECT_EQ(count_result.rows()[0].get<int64_t>(0), 3);

    db::Aggregation sum;
    sum.op = AggregateOp::Sum;
    sum.field = "seq";
    const auto sum_result = session_->aggregate(kCollection, sum);
    ASSERT_EQ(sum_result.size(), 1u);
    EXPECT_EQ(sum_result.columns()[0].type, ValueType::Int64) << "SUM(DECIMAL) 按声明类型解码";
    EXPECT_EQ(sum_result.rows()[0].get<int64_t>(0), 63);

    db::Aggregation avg;
    avg.op = AggregateOp::Avg;
    avg.field = "seq";
    const auto avg_result = session_->aggregate(kCollection, avg);
    ASSERT_EQ(avg_result.size(), 1u);
    EXPECT_EQ(avg_result.columns()[0].type, ValueType::Float64);
    EXPECT_DOUBLE_EQ(avg_result.rows()[0].get<double>(0), 21.0);

    db::Aggregation max_price;
    max_price.op = AggregateOp::Max;
    max_price.field = "price";
    const auto max_result = session_->aggregate(kCollection, max_price);
    ASSERT_EQ(max_result.size(), 1u);
    EXPECT_EQ(max_result.columns()[0].type, ValueType::Float64);
    EXPECT_DOUBLE_EQ(max_result.rows()[0].get<double>(0), 3.5);

    db::Aggregation sum_price;
    sum_price.op = AggregateOp::Sum;
    sum_price.field = "price";
    const auto sum_price_result = session_->aggregate(kCollection, sum_price);
    ASSERT_EQ(sum_price_result.size(), 1u);
    EXPECT_DOUBLE_EQ(sum_price_result.rows()[0].get<double>(0), 7.5);

    db::Aggregation min_seq;
    min_seq.op = AggregateOp::Min;
    min_seq.field = "seq";
    const auto min_result = session_->aggregate(kCollection, min_seq);
    ASSERT_EQ(min_result.size(), 1u);
    EXPECT_EQ(min_result.rows()[0].get<int64_t>(0), 10);

    db::Aggregation max_by_account;
    max_by_account.op = AggregateOp::Max;
    max_by_account.field = "seq";
    max_by_account.group_by = {"account_id"};
    max_by_account.filter = db::filters::gte("seq", 10);
    const auto grouped = session_->aggregate(kCollection, max_by_account);
    ASSERT_EQ(grouped.size(), 2u);
    EXPECT_EQ(grouped.columns()[0].name, "account_id");
    EXPECT_EQ(grouped.columns()[0].type, ValueType::String);
    EXPECT_EQ(grouped.columns()[1].type, ValueType::Int64);
    std::map<std::string, int64_t> maxima;
    for (const db::Row& row : grouped.rows()) {
        maxima[row.get<std::string>(0)] = row.get<int64_t>(1);
    }
    EXPECT_EQ(maxima["a"], 20);
    EXPECT_EQ(maxima["b"], 33);
}

TEST_F(MysqlDriverTest, UnregisteredAggregateFieldMapsToQueryFailed) {
    open_migrated();
    db::Aggregation aggregation;
    aggregation.op = AggregateOp::Max;
    aggregation.field = "missing";
    EXPECT_EQ(code_of([&] { (void)session_->aggregate(kCollection, aggregation); }),
              DZ_EC_DB_QUERY_FAILED);
    aggregation.field = "seq";
    aggregation.group_by = {"missing"};
    EXPECT_EQ(code_of([&] { (void)session_->aggregate(kCollection, aggregation); }),
              DZ_EC_DB_QUERY_FAILED);
}

#endif  // DZ_WITH_MYSQL
