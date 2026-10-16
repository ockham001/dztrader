/**
 * @file mysql_test_env.h
 * @brief MySQL 用例的环境开关与自建/自删库辅助（仅测试用）
 *
 * 服务器通过环境变量启用：DZ_TEST_MYSQL_HOST / PORT / USER / PASSWORD（spec §9.2）。
 * 未配置时用例必须 GTEST_SKIP（不允许失败）；GTEST_SKIP 只能在用例/SetUp 内直接调用，
 * 故这里只提供 `mysql_env_configured()` 判定，由用例自行 skip。
 * 每个用例自建 `dz_test_<pid>_<rand>` 库并在结束时 DROP，不污染服务器。
 */
#ifndef DZTRADER_DB_TESTS_MYSQL_TEST_ENV_H_
#define DZTRADER_DB_TESTS_MYSQL_TEST_ENV_H_

#if defined(DZ_WITH_MYSQL) && DZ_WITH_MYSQL

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

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace db_test::mysql_env {

/// 环境变量文本（未设置或空串时取 fallback）
inline std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value == nullptr || *value == '\0') ? std::string(fallback) : std::string(value);
}

/// DZ_TEST_MYSQL_HOST 非空即视为已配置服务器
inline bool configured() { return !env_or("DZ_TEST_MYSQL_HOST", "").empty(); }

struct Server {
    std::string host;
    std::uint16_t port = 3306;
    std::string user;
    std::string password;
};

inline Server server() {
    Server value;
    value.host = env_or("DZ_TEST_MYSQL_HOST", "127.0.0.1");
    value.port = static_cast<std::uint16_t>(std::stoi(env_or("DZ_TEST_MYSQL_PORT", "3306")));
    value.user = env_or("DZ_TEST_MYSQL_USER", "");
    value.password = env_or("DZ_TEST_MYSQL_PASSWORD", "");
    return value;
}

/// 临时库名：dz_test_<pid>_<rand>（spec §9.2）
inline std::string unique_database_name(std::string_view prefix) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<std::uint32_t> distribution;
    return std::string(prefix) + "_" +
           std::to_string(static_cast<std::uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(distribution(generator));
}

/// 用一条独立连接执行服务器级语句（CREATE/DROP DATABASE、information_schema 查询），
/// 返回第一行第一列的标量文本（NULL/无行返回 nullopt）；database 为空表示不选库
inline std::optional<std::string> run_statement(const std::string& database,
                                                const std::string& sql) {
    const Server target = server();
    boost::asio::io_context context;
    boost::mysql::any_connection connection(context);
    boost::mysql::connect_params params;
    params.server_address = boost::mysql::host_and_port{target.host, target.port};
    params.username = target.user;
    params.password = target.password;
    params.database = database;
    params.ssl = boost::mysql::ssl_mode::enable;
    boost::mysql::diagnostics diagnostics;
    boost::mysql::error_code ec;
    connection.connect(params, ec, diagnostics);
    if (ec) {
        throw std::runtime_error("mysql test env connect failed: " +
                                 std::string(diagnostics.server_message()));
    }
    boost::mysql::results result;
    connection.execute(sql, result, ec, diagnostics);
    if (ec) {
        throw std::runtime_error("mysql test env statement failed | sql=" + sql +
                                 " error=" + std::string(diagnostics.server_message()));
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
                default:
                    value = "<non-scalar>";
            }
        }
    }
    boost::mysql::error_code close_ec;
    connection.close(close_ec, diagnostics);
    return value;
}

inline void create_database(const std::string& name) {
    (void)run_statement("",
                        "CREATE DATABASE `" + name + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_bin");
}

inline void drop_database(const std::string& name) {
    try {
        (void)run_statement("", "DROP DATABASE IF EXISTS `" + name + "`");
    } catch (const std::exception&) {
        // 清理失败不掩盖用例结论
    }
}

/// 目标库上执行标量查询（第一行第一列文本；空/NULL 返回 nullopt）
inline std::optional<std::string> scalar(const std::string& database, const std::string& sql) {
    return run_statement(database, sql);
}

/// Config 构造：指向临时库 + 给定 schema version（migrate 必填）
inline dztrader::db::Config config_for(
    const std::string& database,
    const std::map<std::string, std::string, std::less<>>& extra = {}) {
    const Server target = server();
    std::map<std::string, std::string, std::less<>> options;
    options["host"] = target.host;
    options["port"] = std::to_string(target.port);
    options["user"] = target.user;
    options["password"] = target.password;
    options["database"] = database;
    for (const auto& [key, value] : extra) {
        options[key] = value;
    }
    return dztrader::db::Config{.backend = "mysql", .options = std::move(options)};
}

}  // namespace db_test::mysql_env

#endif  // DZ_WITH_MYSQL

#endif  // DZTRADER_DB_TESTS_MYSQL_TEST_ENV_H_
