#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/db/legacy/connection.h>
#include <dztrader/db/legacy/migration.h>

#include <filesystem>
#include <random>
#include <stdexcept>

namespace {

/// 辅助: 创建 schema_version 表是否存在的检查
bool table_exists(dztrader::db::legacy::Connection& conn, const std::string& name) {
    return conn.scalar<int>(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='" + name + "'") > 0;
}

/// 进程唯一临时目录名 (PID + 随机数), 避免并发测试共用同一文件库
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

TEST(MigrationManagerTest, ApplyCreatesSchemaVersionTable) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
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
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database&) {});
    mgr.apply(conn.db());

    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 1);
    EXPECT_EQ(conn.scalar<int>("SELECT version FROM schema_version WHERE version=1"), 1);
    EXPECT_FALSE(conn.scalar<std::string>("SELECT applied_at FROM schema_version WHERE version=1").empty());
}

TEST(MigrationManagerTest, ApplyMultipleMigrationsInOrder) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
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
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });

    mgr.apply(conn.db());
    auto second = mgr.apply(conn.db());
    EXPECT_TRUE(second.empty());
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 1);
}

TEST(MigrationManagerTest, PartialApplyResumesFromLastVersion) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.apply(conn.db());

    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE TABLE t2 (id INTEGER)"); });
    auto applied = mgr.apply(conn.db());
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0], 2);
    EXPECT_TRUE(table_exists(conn, "t2"));
}

TEST(MigrationManagerTest, MigrationFailureRollsBackAndThrows) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE INVALID TABLE"); });

    EXPECT_THROW(mgr.apply(conn.db()), std::runtime_error);
    // 单事务语义: 失败整体回滚, 本次已执行的 v1 建表与版本记录一并撤销
    EXPECT_FALSE(table_exists(conn, "t1"));
    EXPECT_FALSE(table_exists(conn, "schema_version"));
}

TEST(MigrationManagerTest, EmptyMigrationsAppliesNothing) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    auto applied = mgr.apply(conn.db());
    EXPECT_TRUE(applied.empty());
    EXPECT_TRUE(table_exists(conn, "schema_version"));
}

TEST(MigrationManagerTest, ReapplyIsNoop) {
    dztrader::db::legacy::Connection conn(":memory:");
    dztrader::db::legacy::MigrationManager mgr;
    mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
    mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE TABLE t2 (id INTEGER)"); });

    auto first = mgr.apply(conn.db());
    ASSERT_EQ(first.size(), 2u);

    auto second = mgr.apply(conn.db());
    EXPECT_TRUE(second.empty());
    // 每个版本恰好一行 (重复 apply 不追加记录)
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(*) FROM schema_version"), 2);
    EXPECT_EQ(conn.scalar<int>("SELECT COUNT(DISTINCT version) FROM schema_version"), 2);
}

TEST(MigrationManagerTest, FailedMigrationLeavesNoPartialState) {
    const auto tmp_dir = unique_temp_dir("dz_migration_fail_test");
    std::filesystem::create_directories(tmp_dir);
    const auto db_path = (tmp_dir / "test.db").string();

    {
        dztrader::db::legacy::Connection conn(db_path);
        dztrader::db::legacy::MigrationManager mgr;
        mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
        mgr.add(2, [](SQLite::Database&) { throw std::runtime_error("migration 2 failed"); });
        EXPECT_THROW(mgr.apply(conn.db()), std::runtime_error);
    }
    {
        // 重开连接: 单事务语义下失败的 apply 整体回滚, 不残留任何部分状态
        dztrader::db::legacy::Connection conn(db_path);
        EXPECT_FALSE(table_exists(conn, "schema_version"));
        EXPECT_FALSE(table_exists(conn, "t1"));
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST(MigrationManagerTest, ConcurrentApplySafe) {
    const auto tmp_dir = unique_temp_dir("dz_migration_concurrent_test");
    std::filesystem::create_directories(tmp_dir);
    const auto db_path = (tmp_dir / "test.db").string();

    auto register_all = [](dztrader::db::legacy::MigrationManager& mgr) {
        mgr.add(1, [](SQLite::Database& db) { db.exec("CREATE TABLE t1 (id INTEGER)"); });
        mgr.add(2, [](SQLite::Database& db) { db.exec("CREATE TABLE t2 (id INTEGER)"); });
    };

    {
        dztrader::db::legacy::Connection conn_a(db_path);
        dztrader::db::legacy::Connection conn_b(db_path);
        dztrader::db::legacy::MigrationManager mgr_a;
        dztrader::db::legacy::MigrationManager mgr_b;
        register_all(mgr_a);
        register_all(mgr_b);

        auto applied_a = mgr_a.apply(conn_a.db());
        ASSERT_EQ(applied_a.size(), 2u);

        // 连接 B 在事务内复查版本: 已由 A 提交的迁移不再重放
        auto applied_b = mgr_b.apply(conn_b.db());
        EXPECT_TRUE(applied_b.empty());

        // 两连接都能读到最终 schema
        EXPECT_EQ(conn_a.scalar<int>("SELECT COUNT(*) FROM schema_version"), 2);
        EXPECT_EQ(conn_b.scalar<int>("SELECT COUNT(*) FROM schema_version"), 2);
        EXPECT_TRUE(table_exists(conn_a, "t1"));
        EXPECT_TRUE(table_exists(conn_b, "t2"));
    }

    // 关闭连接后再删临时目录 (Windows 下占用文件无法删除)
    std::filesystem::remove_all(tmp_dir);
}

}  // namespace
