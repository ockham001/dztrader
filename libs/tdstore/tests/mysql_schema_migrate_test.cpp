// tdstore 真实 schema × MySQL 迁移集成测试。
// 缺陷回归：instruments（28 字段 + 主键 + 零索引）曾因 CREATE TABLE 生成器留尾随逗号而 migrate 失败，
// 导致 tdstore::schemas() 在 MySQL 上五张表全部建不出来。本文件在真机 MySQL 上跑真实 schema 的
// migrate + instruments upsert/find，服务器未配置（DZ_TEST_MYSQL_HOST 为空）时整体 GTEST_SKIP。
#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_address.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/error_code.hpp>
#include <boost/mysql/field_kind.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <boost/mysql/string_view.hpp>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace db = dztrader::db;
namespace tdstore = dztrader::tdstore;

namespace {

/// 本文件自带的最小环境 helper（不依赖 libs/db 测试辅助头，避免跨目录 include）
std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value == nullptr || *value == '\0') ? std::string(fallback) : std::string(value);
}

bool mysql_configured() { return !env_or("DZ_TEST_MYSQL_HOST", "").empty(); }

std::string unique_database_name() {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<std::uint32_t> distribution;
    return "dz_schema_" +
           std::to_string(static_cast<std::uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(distribution(generator));
}

/// 独立连接执行语句并返回第一行第一列标量（database 为空表示不选库）
std::optional<std::string> run_statement(const std::string& database, const std::string& sql) {
    boost::asio::io_context context;
    boost::mysql::any_connection connection(context);
    boost::mysql::connect_params params;
    params.server_address = boost::mysql::host_and_port{env_or("DZ_TEST_MYSQL_HOST", "127.0.0.1"),
                                                        static_cast<std::uint16_t>(std::stoi(
                                                            env_or("DZ_TEST_MYSQL_PORT", "3306")))};
    params.username = env_or("DZ_TEST_MYSQL_USER", "");
    params.password = env_or("DZ_TEST_MYSQL_PASSWORD", "");
    params.database = database;
    params.ssl = boost::mysql::ssl_mode::enable;
    boost::mysql::diagnostics diagnostics;
    boost::mysql::error_code ec;
    connection.connect(params, ec, diagnostics);
    if (ec) {
        throw std::runtime_error("tdstore mysql test connect failed: " +
                                 std::string(diagnostics.server_message()));
    }
    boost::mysql::results result;
    connection.execute(sql, result, ec, diagnostics);
    if (ec) {
        throw std::runtime_error("tdstore mysql test statement failed | sql=" + sql + " error=" +
                                 std::string(diagnostics.server_message()));
    }
    std::optional<std::string> value;
    if (!result.rows().empty() && result.rows()[0].size() > 0) {
        const boost::mysql::field_view field = result.rows()[0][0];
        if (!field.is_null()) {
            switch (field.kind()) {
                case boost::mysql::field_kind::int64:
                    value = std::to_string(field.as_int64());
                    break;
                case boost::mysql::field_kind::uint64:
                    value = std::to_string(field.as_uint64());
                    break;
                case boost::mysql::field_kind::string: {
                    const boost::mysql::string_view text = field.as_string();
                    value = std::string(text.data(), text.size());
                    break;
                }
                default: value = "<non-scalar>";
            }
        }
    }
    boost::mysql::error_code close_ec;
    connection.close(close_ec, diagnostics);
    return value;
}

void create_database(const std::string& name) {
    (void)run_statement("", "CREATE DATABASE `" + name +
                                "` CHARACTER SET utf8mb4 COLLATE utf8mb4_bin");
}

void drop_database(const std::string& name) {
    try {
        (void)run_statement("", "DROP DATABASE IF EXISTS `" + name + "`");
    } catch (const std::exception&) {
        // 清理失败不掩盖用例结论
    }
}

db::Config config_for(const std::string& database) {
    std::map<std::string, std::string, std::less<>> options;
    options["host"] = env_or("DZ_TEST_MYSQL_HOST", "127.0.0.1");
    options["port"] = env_or("DZ_TEST_MYSQL_PORT", "3306");
    options["user"] = env_or("DZ_TEST_MYSQL_USER", "");
    options["password"] = env_or("DZ_TEST_MYSQL_PASSWORD", "");
    options["database"] = database;
    options["schema_version"] = std::to_string(tdstore::kTdSchemaVersion);
    return db::Config{.backend = "mysql", .options = std::move(options)};
}

/// instruments 真实列序（28 列）：instrument_id/exchange_id/symbol/name/...
db::Row instrument_row(std::string instrument_id, double price_tick, std::int64_t updated_at) {
    return db::Row{std::vector<db::Value>{
        std::move(instrument_id),   // instrument_id (PK)
        std::string("SHFE"),        // exchange_id
        std::string("cu2612"),      // symbol
        std::string("沪铜2612"),    // name (utf8mb4 往返)
        int64_t{1},                 // product_class
        int64_t{1},                 // settle_cycle
        int64_t{0},                 // settlement_method
        int64_t{0},                 // is_inverse
        std::string("CNY"),         // currency
        std::string(""),            // base_asset
        int64_t{1},                 // min_limit_order_volume
        int64_t{500},               // max_limit_order_volume
        double{5.0},                // volume_multiple
        price_tick,                 // price_tick
        double{10.0},               // volume_step
        int64_t{20261001},          // listed_date
        int64_t{0},                 // delisted_date
        int64_t{0},                 // option_type
        int64_t{0},                 // option_exercise_style
        std::string(""),            // underlying_id
        double{0.0},                // option_strike
        std::string(""),            // option_series
        std::string("20261001"),    // update_day
        std::string("cu"),          // product_code
        int64_t{1},                 // min_market_order_volume
        int64_t{500},               // max_market_order_volume
        double{5.0},                // underlying_multiple
        updated_at,                 // updated_at
    }};
}

}  // namespace

class TdSchemaMysqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysql_configured()) {
            GTEST_SKIP() << "mysql test server not configured | env=DZ_TEST_MYSQL_HOST";
        }
        database_name_ = unique_database_name();
        create_database(database_name_);
        database_ = db::Database::open(config_for(database_name_), tdstore::schemas());
        database_->migrate();
        session_ = database_->session();
    }

    void TearDown() override {
        session_.reset();
        database_.reset();
        if (!database_name_.empty()) {
            drop_database(database_name_);
        }
    }

    std::string database_name_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<db::Session> session_;
};

TEST_F(TdSchemaMysqlTest, MigrateCreatesAllFiveTables) {
    for (const char* table : {"orders", "trades", "positions", "trading_accounts", "instruments"}) {
        const auto exists = run_statement(
            database_name_,
            std::string("SELECT COUNT(*) FROM information_schema.TABLES WHERE "
                        "TABLE_SCHEMA=DATABASE() AND TABLE_NAME='") +
                table + "'");
        ASSERT_TRUE(exists.has_value()) << "table=" << table;
        EXPECT_EQ(*exists, "1") << "table=" << table;
    }
    EXPECT_EQ(run_statement(database_name_, "SELECT MAX(version) FROM schema_version"),
              std::optional<std::string>(std::to_string(tdstore::kTdSchemaVersion)));
    EXPECT_EQ(run_statement(database_name_,
                            "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE "
                            "TABLE_SCHEMA=DATABASE() AND TABLE_NAME='instruments'"),
              std::optional<std::string>("28"));
    // instruments 无索引：SHOW INDEX 只应有 PRIMARY（零业务索引）——尾随逗号缺陷的现场
    EXPECT_EQ(run_statement(database_name_,
                            "SELECT COUNT(DISTINCT INDEX_NAME) FROM information_schema.STATISTICS "
                            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='instruments'"),
              std::optional<std::string>("1"));
}

TEST_F(TdSchemaMysqlTest, InstrumentsUpsertAndFind) {
    const db::Row first = instrument_row("cu2612", 10.0, 100);
    session_->upsert("instruments", std::span<const db::Row>(&first, 1));
    auto found = session_->find("instruments");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found.rows()[0].get<std::string>(0), "cu2612");
    EXPECT_EQ(found.rows()[0].get<std::string>(3), "沪铜2612") << "utf8mb4 往返";
    EXPECT_DOUBLE_EQ(found.rows()[0].get<double>(13), 10.0) << "price_tick 不得被读成 0";
    EXPECT_EQ(found.rows()[0].get<int64_t>(27), 100);

    // 主键幂等：同 instrument_id 再写只更新
    const db::Row updated = instrument_row("cu2612", 20.0, 200);
    session_->upsert("instruments", std::span<const db::Row>(&updated, 1));
    found = session_->find("instruments",
                           db::filters::eq("instrument_id", std::string("cu2612")));
    ASSERT_EQ(found.size(), 1u);
    EXPECT_DOUBLE_EQ(found.rows()[0].get<double>(13), 20.0);
    EXPECT_EQ(found.rows()[0].get<int64_t>(27), 200);
    EXPECT_EQ(run_statement(database_name_, "SELECT COUNT(*) FROM instruments"),
              std::optional<std::string>("1"));
}

TEST_F(TdSchemaMysqlTest, MigrateIsIdempotentOnRealSchema) {
    database_->migrate();  // 再跑一次：CREATE TABLE IF NOT EXISTS + 版本校验幂等
    EXPECT_EQ(run_statement(database_name_, "SELECT COUNT(*) FROM schema_version"),
              std::optional<std::string>("1"));
    const db::Row row = instrument_row("al2612", 5.0, 300);
    session_->upsert("instruments", std::span<const db::Row>(&row, 1));
    EXPECT_EQ(run_statement(database_name_, "SELECT COUNT(*) FROM instruments"),
              std::optional<std::string>("1"));
}

#endif  // DZ_WITH_MYSQL
