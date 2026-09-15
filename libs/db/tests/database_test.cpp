#include <gtest/gtest.h>

#include <dztrader/db/database_sqlite.h>

#include <string_view>
#include <variant>

using dztrader::db::ColumnType;
using dztrader::db::Database;
using dztrader::db::SqliteDatabase;
using dztrader::db::SqliteDatabaseRef;

namespace {

/// 测试辅助: 读单值整数 (COUNT(*))
int64_t scalar_int(Database& db, std::string_view sql) {
    auto result = db.query(sql);
    return std::get<int64_t>(result.rows.at(0).at(0));
}

}  // namespace

TEST(DbDatabaseTest, ExecAndQueryTypedValues) {
    SqliteDatabase db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE t(a INTEGER, b REAL, c TEXT, d INTEGER)");

    auto stmt = db.prepare("INSERT INTO t(a,b,c,d) VALUES(?,?,?,?)");
    stmt->bind(1, int64_t{7});
    stmt->bind(2, 2.5);
    stmt->bind(3, std::string("x"));
    stmt->bind(4, std::monostate{});
    stmt->execute();

    auto result = db.query("SELECT a,b,c,d FROM t");
    ASSERT_EQ(result.columns.size(), 4u);
    EXPECT_EQ(result.columns[0].type, ColumnType::Int64);
    EXPECT_EQ(result.columns[1].type, ColumnType::Float64);
    EXPECT_EQ(result.columns[2].type, ColumnType::String);
    EXPECT_EQ(result.columns[3].type, ColumnType::Null);

    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(result.rows[0][0]), 7);
    EXPECT_DOUBLE_EQ(std::get<double>(result.rows[0][1]), 2.5);
    EXPECT_EQ(std::get<std::string>(result.rows[0][2]), "x");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(result.rows[0][3]));
}

TEST(DbDatabaseTest, TransactionRollbackOnDestruct) {
    SqliteDatabase db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE t(v INTEGER)");
    {
        auto txn = db.begin();
        db.exec("INSERT INTO t(v) VALUES(1)");
        // 不 commit, 析构时回滚
    }
    EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM t"), 0);
}

TEST(DbDatabaseTest, TransactionCommitPersists) {
    SqliteDatabase db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE t(v INTEGER)");
    {
        auto txn = db.begin();
        db.exec("INSERT INTO t(v) VALUES(1)");
        txn->commit();
    }
    EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM t"), 1);
}

TEST(DbDatabaseTest, ImmediateTransactionWorks) {
    SqliteDatabase db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE t(v INTEGER)");
    auto txn = db.begin(true);
    db.exec("INSERT INTO t(v) VALUES(1)");
    txn->commit();
    EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM t"), 1);
}

TEST(DbDatabaseTest, RefWrapperSharesConnection) {
    SQLite::Database raw(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    raw.exec("CREATE TABLE t(v INTEGER)");
    raw.exec("INSERT INTO t(v) VALUES(9)");

    SqliteDatabaseRef db(raw);
    EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM t"), 1);
}

TEST(DbDatabaseTest, PrepareReuseWithReset) {
    SqliteDatabase db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE t(v INTEGER)");

    auto stmt = db.prepare("INSERT INTO t(v) VALUES(?)");
    stmt->bind(1, int64_t{1});
    stmt->execute();
    stmt->reset();
    stmt->bind(1, int64_t{2});
    stmt->execute();

    EXPECT_EQ(scalar_int(db, "SELECT COUNT(*) FROM t"), 2);
}
