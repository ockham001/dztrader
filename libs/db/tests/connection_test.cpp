#include <gtest/gtest.h>

#include <dztrader/db/connection.h>

#include <filesystem>

TEST(DbConnectionTest, OpenInMemoryCreatesSchema) {
    dztrader::db::Connection conn(":memory:");
    conn.exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)");
    conn.exec("INSERT INTO test (name) VALUES ('hello')");
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM test"), 1);
}

TEST(DbConnectionTest, PragmasAppliedOnOpen) {
    dztrader::db::Connection conn(":memory:");
    EXPECT_EQ(conn.pragma<std::string>("journal_mode"), "memory");
    // PRAGMA synchronous 返回整数码 (0=OFF,1=NORMAL,2=FULL,3=EXTRA), 文本化为 "2"
    EXPECT_EQ(conn.pragma<std::string>("synchronous"), "2");
}

TEST(DbConnectionTest, TransactionCommits) {
    dztrader::db::Connection conn(":memory:");
    conn.exec("CREATE TABLE t (v INTEGER)");
    {
        auto txn = conn.begin_transaction();
        conn.exec("INSERT INTO t (v) VALUES (1)");
        txn.commit();
    }
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM t"), 1);
}

TEST(DbConnectionTest, TransactionRollsBackOnDestroy) {
    dztrader::db::Connection conn(":memory:");
    conn.exec("CREATE TABLE t (v INTEGER)");
    {
        auto txn = conn.begin_transaction();
        conn.exec("INSERT INTO t (v) VALUES (1)");
        // 不调 commit, 析构时回滚
    }
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM t"), 0);
}
