#include <gtest/gtest.h>

#include <dztrader/db/connection.h>
#include <dztrader/db/migration.h>

#include <stdexcept>

namespace {

/// 辅助: 创建 schema_version 表是否存在的检查
bool table_exists(dztrader::db::Connection& conn, const std::string& name) {
    return conn.scalar<int>(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='" + name + "'") > 0;
}

TEST(MigrationManagerTest, ApplyCreatesSchemaVersionTable) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) {
        db.exec("CREATE TABLE foo (id INTEGER)");
    });
    auto applied = mgr.apply(conn.db());
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0], 1);
    EXPECT_TRUE(table_exists(conn, "schema_version"));
    EXPECT_TRUE(table_exists(conn, "foo"));
}

TEST(MigrationManagerTest, ApplyRecordsVersionAndTimestamp) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database&) {});
    mgr.apply(conn.db());

    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 1);
    EXPECT_EQ(conn.scalar<int>("SELECT version FROM schema_version WHERE version=1"), 1);
    EXPECT_FALSE(conn.scalar<std::string>("SELECT applied_at FROM schema_version WHERE version=1").empty());
}

TEST(MigrationManagerTest, ApplyMultipleMigrationsInOrder) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE TABLE t2 (id INTEGER)"); });
    mgr.add(3, [](SQLite::Database& db) { db.exec("ALTER TABLE t1 ADD COLUMN name TEXT"); });

    auto applied = mgr.apply(conn.db());
    ASSERT_EQ(applied.size(), 3u);
    EXPECT_EQ(applied[0], 1);
    EXPECT_EQ(applied[1], 2);
    EXPECT_EQ(applied[2], 3);
    EXPECT_TRUE(table_exists(conn, "t1"));
    EXPECT_TRUE(table_exists(conn, "t2"));
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 3);
}

TEST(MigrationManagerTest, DuplicateApplyIsNoOp) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });

    mgr.apply(conn.db());
    auto second = mgr.apply(conn.db());
    EXPECT_TRUE(second.empty());
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 1);
}

TEST(MigrationManagerTest, PartialApplyResumesFromLastVersion) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.apply(conn.db());

    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE TABLE t2 (id INTEGER)"); });
    auto applied = mgr.apply(conn.db());
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0], 2);
    EXPECT_TRUE(table_exists(conn, "t2"));
}

TEST(MigrationManagerTest, MigrationFailureRollsBackAndThrows) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE INVALID TABLE"); });

    EXPECT_THROW(mgr.apply(conn.db()), std::runtime_error);
    EXPECT_TRUE(table_exists(conn, "t1"));
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 1);
}

TEST(MigrationManagerTest, EmptyMigrationsAppliesNothing) {
    dztrader::db::Connection conn(":memory:");
    dztrader::db::MigrationManager mgr;
    auto applied = mgr.apply(conn.db());
    EXPECT_TRUE(applied.empty());
    EXPECT_TRUE(table_exists(conn, "schema_version"));
}

}  // namespace
