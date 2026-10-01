#include "mysql_driver.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/mysql/any_address.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/diagnostics.hpp>
#include <boost/mysql/error_code.hpp>
#include <boost/mysql/field_kind.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/row_view.hpp>
#include <boost/mysql/ssl_mode.hpp>
#include <boost/mysql/statement.hpp>
#include <boost/mysql/string_view.hpp>
#include <boost/system/error_code.hpp>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

#include "mysql_sql.h"
#include "sql_compiler.h"

namespace dztrader::db::internal {

namespace {

/// 每会话缓存的上限：超限按 FIFO 关闭最旧语句，避免服务端 max_prepared_stmt_count 耗尽
constexpr size_t kMaxCachedStatements = 64;

std::string to_display(boost::mysql::string_view text) {
    return std::string(text.data(), text.size());
}

std::string_view as_view(boost::mysql::string_view text) {
    return std::string_view(text.data(), text.size());
}

/// 服务端诊断优先；客户端错误（连不上/超时）无服务端消息时用 error_code 文本
std::string error_text(const boost::mysql::error_code& ec,
                       const boost::mysql::diagnostics& diag) {
    const std::string_view server = as_view(diag.server_message());
    return server.empty() ? ec.message() : std::string(server);
}

[[noreturn]] void throw_param_type_mismatch(std::string_view column, std::string_view collection,
                                            std::string_view detail) {
    throw Exception(DZ_EC_INVALID_PARAM,
                    "parameter type mismatch | column={} collection={} detail={}", column,
                    collection, detail);
}

[[noreturn]] void throw_column_type_mismatch(std::string_view column, std::string_view collection,
                                             std::string_view kind) {
    throw Exception(DZ_EC_DB_QUERY_FAILED, "unexpected column type | column={} collection={} kind={}",
                    column, collection, kind);
}

std::int64_t parse_int64_text(std::string_view text, std::string_view column,
                              std::string_view collection) {
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw Exception(DZ_EC_INVALID_PARAM,
                        "string parameter is not an integer | column={} collection={} value=\"{}\"",
                        column, collection, text);
    }
    return value;
}

double parse_double_text(std::string_view text, std::string_view column,
                         std::string_view collection) {
    double value = 0.0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw Exception(DZ_EC_INVALID_PARAM,
                        "string parameter is not a number | column={} collection={} value=\"{}\"",
                        column, collection, text);
    }
    return value;
}

std::int64_t parse_int64_option(std::string_view key, std::string_view text, std::int64_t lower,
                                std::int64_t upper) {
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || value < lower || value > upper) {
        throw Exception(DZ_EC_INVALID_PARAM, "invalid mysql option | key={} value=\"{}\"", key, text);
    }
    return value;
}

const std::string* find_option(const std::map<std::string, std::string, std::less<>>& options,
                               std::string_view key) {
    const auto it = options.find(key);
    return it == options.end() ? nullptr : &it->second;
}

MysqlSslMode parse_ssl_mode(std::string_view text) {
    if (text == "disabled") {
        return MysqlSslMode::Disabled;
    }
    if (text == "preferred") {
        return MysqlSslMode::Preferred;
    }
    if (text == "required") {
        return MysqlSslMode::Required;
    }
    if (text == "verify_ca") {
        return MysqlSslMode::VerifyCa;
    }
    if (text == "verify_identity") {
        return MysqlSslMode::VerifyIdentity;
    }
    throw Exception(DZ_EC_INVALID_PARAM,
                    "invalid mysql option | key=ssl_mode value=\"{}\" allowed=disabled,preferred,"
                    "required,verify_ca,verify_identity",
                    text);
}

boost::mysql::ssl_mode to_boost_ssl_mode(MysqlSslMode mode) {
    switch (mode) {
        case MysqlSslMode::Disabled: return boost::mysql::ssl_mode::disable;
        case MysqlSslMode::Preferred: return boost::mysql::ssl_mode::enable;
        case MysqlSslMode::Required:
        case MysqlSslMode::VerifyCa:
        case MysqlSslMode::VerifyIdentity: return boost::mysql::ssl_mode::require;
    }
    return boost::mysql::ssl_mode::enable;
}

/// 证书校验模式需要自建 SSL context（Boost.MySQL 1.90 的 ssl_mode 无 verify_* 取值）
bool needs_certificate_verification(MysqlSslMode mode) {
    return mode == MysqlSslMode::VerifyCa || mode == MysqlSslMode::VerifyIdentity;
}

/// UTC 时间戳，形状与 sqlite 侧 schema_version.applied_at 的 "%FT%TZ" 一致
std::string utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", utc.tm_year + 1900,
                       utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

/// 规范化枚举文本比较（MariaDB 返回 "REPEATABLE-READ"）
std::string normalize_server_value(std::string_view text) {
    std::string normalized(text);
    for (char& ch : normalized) {
        if (ch == '_') {
            ch = '-';
        } else if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return normalized;
}

/// 从单行单列结果里取字符串（会话校验用）
std::string read_scalar_text(const boost::mysql::results& result, std::string_view column) {
    if (result.rows().empty() || result.rows()[0].size() == 0) {
        throw Exception(DZ_EC_DB_OPEN_FAILED, "mysql session check returned no rows | column={}",
                        column);
    }
    const boost::mysql::field_view field = result.rows()[0][0];
    if (field.is_null()) {
        return {};
    }
    return std::string(field.as_string().data(), field.as_string().size());
}

}  // namespace

MysqlOptions parse_mysql_options(const std::map<std::string, std::string, std::less<>>& options) {
    MysqlOptions parsed;
    if (const std::string* value = find_option(options, "host"); value != nullptr && !value->empty()) {
        parsed.host = *value;
    }
    if (const std::string* value = find_option(options, "port"); value != nullptr && !value->empty()) {
        parsed.port = static_cast<std::uint16_t>(parse_int64_option("port", *value, 1, 65535));
    }
    if (const std::string* value = find_option(options, "user"); value != nullptr) {
        parsed.user = *value;
    }
    if (const std::string* value = find_option(options, "password"); value != nullptr) {
        parsed.password = *value;
    }
    if (const std::string* value = find_option(options, "database");
        value != nullptr && !value->empty()) {
        parsed.database = *value;
    }
    if (const std::string* value = find_option(options, "connect_timeout_ms");
        value != nullptr && !value->empty()) {
        parsed.connect_timeout = std::chrono::milliseconds(
            parse_int64_option("connect_timeout_ms", *value, 0, 86400000));
    }
    if (const std::string* value = find_option(options, "read_timeout_ms");
        value != nullptr && !value->empty()) {
        parsed.read_timeout =
            std::chrono::milliseconds(parse_int64_option("read_timeout_ms", *value, 0, 86400000));
    }
    if (const std::string* value = find_option(options, "write_timeout_ms");
        value != nullptr && !value->empty()) {
        parsed.write_timeout =
            std::chrono::milliseconds(parse_int64_option("write_timeout_ms", *value, 0, 86400000));
    }
    if (const std::string* value = find_option(options, "schema_version");
        value != nullptr && !value->empty()) {
        parsed.schema_version =
            parse_int64_option("schema_version", *value, 0, std::numeric_limits<std::int32_t>::max());
    }
    if (const std::string* value = find_option(options, "ssl_mode");
        value != nullptr && !value->empty()) {
        parsed.ssl_mode = parse_ssl_mode(*value);
    }
    return parsed;
}

/// 按目标列声明类型归一化的绑定参数（§7.3，对齐 SQLite 列 affinity 语义）
class BoundParams {
public:
    void add(const Value& value, const FieldSchema& field, const ResourceSchema& schema) {
        if (const double* number = std::get_if<double>(&value);
            number != nullptr && !std::isfinite(*number)) {
            throw Exception(DZ_EC_INVALID_PARAM,
                            "non-finite double parameter rejected | column={} collection={}",
                            field.name, schema.name);
        }
        std::visit([&](const auto& typed) { add_typed(typed, field, schema); }, value);
    }

    [[nodiscard]] std::span<const boost::mysql::field_view> views() const noexcept { return views_; }

private:
    template <typename T>
    void add_typed(const T& value, const FieldSchema& field, const ResourceSchema& schema) {
        if constexpr (std::is_same_v<T, std::monostate>) {
            views_.emplace_back();  // NULL
        } else {
            switch (field.type) {
                case ValueType::String: add_string_column(value, field, schema); break;
                case ValueType::Int64: add_int_column(value, field, schema); break;
                case ValueType::Float64: add_float_column(value, field, schema); break;
                case ValueType::Bool: add_bool_column(value, field, schema); break;
                case ValueType::Null:
                    throw_param_type_mismatch(field.name, schema.name, "column-type-null");
            }
        }
    }

    template <typename T>
    void add_string_column(const T& value, const FieldSchema& field, const ResourceSchema& schema) {
        if constexpr (std::is_same_v<T, std::string>) {
            push_string(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            push_string(value ? "1" : "0");
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            push_string(std::to_string(value));
        } else if constexpr (std::is_same_v<T, double>) {
            push_string(std::format("{}", value));
        } else {
            throw_param_type_mismatch(field.name, schema.name, "unsupported-string-param");
        }
    }

    template <typename T>
    void add_int_column(const T& value, const FieldSchema& field, const ResourceSchema& schema) {
        if constexpr (std::is_same_v<T, std::int64_t>) {
            push_int(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            push_int(value ? 1 : 0);
        } else if constexpr (std::is_same_v<T, std::string>) {
            push_int(parse_int64_text(value, field.name, schema.name));
        } else {
            throw_param_type_mismatch(field.name, schema.name, "double-to-int64");
        }
    }

    template <typename T>
    void add_float_column(const T& value, const FieldSchema& field, const ResourceSchema& schema) {
        if constexpr (std::is_same_v<T, double>) {
            push_double(value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            push_double(static_cast<double>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            push_double(value ? 1.0 : 0.0);
        } else if constexpr (std::is_same_v<T, std::string>) {
            push_double(parse_double_text(value, field.name, schema.name));
        } else {
            throw_param_type_mismatch(field.name, schema.name, "unsupported-float-param");
        }
    }

    template <typename T>
    void add_bool_column(const T& value, const FieldSchema& field, const ResourceSchema& schema) {
        if constexpr (std::is_same_v<T, bool>) {
            push_int(value ? 1 : 0);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            push_int(value != 0 ? 1 : 0);
        } else if constexpr (std::is_same_v<T, std::string>) {
            push_int(parse_int64_text(value, field.name, schema.name) != 0 ? 1 : 0);
        } else {
            throw_param_type_mismatch(field.name, schema.name, "double-to-bool");
        }
    }

    void push_string(std::string text) {
        strings_.push_back(std::move(text));
        const std::string& stored = strings_.back();
        views_.emplace_back(boost::mysql::string_view(stored.data(), stored.size()));
    }
    void push_int(std::int64_t value) { views_.emplace_back(value); }
    void push_double(double value) { views_.emplace_back(value); }

    std::deque<std::string> strings_;  ///< deque: push_back 不搬移元素, field_view 引用稳定
    std::vector<boost::mysql::field_view> views_;
};

// ============================================================
// 连接：io_context + any_connection + 截止时间（对调用方是同步语义）
// ============================================================

class MysqlConnection {
public:
    MysqlConnection(MysqlOptions options, bool read_only)
        : options_(std::move(options)), read_only_(read_only) {}
    ~MysqlConnection() = default;

    MysqlConnection(const MysqlConnection&) = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;

    /// 建立连接并固定会话前置条件（隔离级别/严格模式/只读）；失败抛 DZ_EC_DB_OPEN_FAILED
    void connect() {
        boost::mysql::connect_params params;
        params.server_address = boost::mysql::host_and_port{options_.host, options_.port};
        params.username = options_.user;
        params.password = options_.password;
        params.database = options_.database;
        params.ssl = to_boost_ssl_mode(options_.ssl_mode);
        boost::mysql::any_connection_params connection_params;
        if (needs_certificate_verification(options_.ssl_mode)) {
            ssl_context_ = std::make_unique<boost::asio::ssl::context>(
                boost::asio::ssl::context::tls_client);
            ssl_context_->set_verify_mode(boost::asio::ssl::verify_peer);
            ssl_context_->set_default_verify_paths();
            if (options_.ssl_mode == MysqlSslMode::VerifyIdentity) {
                ssl_context_->set_verify_callback(
                    boost::asio::ssl::host_name_verification(options_.host));
            }
            connection_params.ssl_context = ssl_context_.get();
        }
        conn_.emplace(ctx_, connection_params);
        const boost::mysql::error_code ec = run_with_deadline(
            options_.connect_timeout, DZ_EC_DB_OPEN_FAILED, "connect",
            [&](boost::mysql::any_connection& conn, auto handler) {
                conn.async_connect(params, diag_, std::move(handler));
            });
        if (ec) {
            throw Exception(DZ_EC_DB_OPEN_FAILED,
                            "mysql connect failed | host={} port={} database={} error=\"{}\"",
                            options_.host, options_.port, options_.database, error_text(ec, diag_));
        }
        apply_session_guards();
    }

    /// 文本 SQL（DDL/事务控制/会话设置）；失败抛 code 对应异常
    boost::mysql::results run_text(std::string_view sql, std::chrono::milliseconds timeout,
                                   DzErrorCode code, std::string_view op) {
        boost::mysql::results result;
        const boost::mysql::error_code ec =
            run_with_deadline(timeout, code, op,
                              [&](boost::mysql::any_connection& conn, auto handler) {
                                  conn.async_execute(sql, result, diag_, std::move(handler));
                              });
        if (ec) {
            throw_operation_error(ec, code, op);
        }
        return result;
    }

    /// prepared statement（`?` 按序绑定）；失败抛 code 对应异常
    boost::mysql::results run_prepared(std::string_view sql,
                                       std::span<const boost::mysql::field_view> params,
                                       std::chrono::milliseconds timeout, DzErrorCode code,
                                       std::string_view op) {
        const boost::mysql::statement& statement = cached_statement(sql, timeout, code, op);
        boost::mysql::results result;
        const boost::mysql::error_code ec = run_with_deadline(
            timeout, code, op, [&](boost::mysql::any_connection& conn, auto handler) {
                conn.async_execute(statement.bind(params.begin(), params.end()), result, diag_,
                                   std::move(handler));
            });
        if (ec) {
            throw_operation_error(ec, code, op);
        }
        return result;
    }

    [[nodiscard]] std::chrono::milliseconds read_timeout() const noexcept {
        return options_.read_timeout;
    }
    [[nodiscard]] std::chrono::milliseconds write_timeout() const noexcept {
        return options_.write_timeout;
    }

private:
    /// 会话前置条件（建会话即固定，拒绝依赖服务器全局默认）：
    /// - REPEATABLE READ：READ-COMMITTED 下 WITH CONSISTENT SNAPSHOT 会被忽略，四表水位快照失效
    /// - STRICT_ALL_TABLES：非严格模式下 VARCHAR(255) 超长静默截断
    /// - 只读会话：SET SESSION TRANSACTION READ ONLY（写操作另在驱动侧直接拒绝）
    /// 每项都回读服务端实际值校验，不符即抛（不做静默降级）
    void apply_session_guards() {
        const std::chrono::milliseconds timeout = options_.connect_timeout;
        (void)run_text("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ", timeout,
                       DZ_EC_DB_OPEN_FAILED, "set_isolation_level");
        const std::string isolation = normalize_server_value(
            read_scalar_text(run_text("SELECT @@transaction_isolation", timeout,
                                      DZ_EC_DB_OPEN_FAILED, "read_isolation_level"),
                             "transaction_isolation"));
        if (isolation != "REPEATABLE-READ") {
            throw Exception(DZ_EC_DB_UNSUPPORTED_CAPABILITY,
                            "mysql isolation level mismatch | expected=REPEATABLE-READ actual={}",
                            isolation);
        }
        (void)run_text("SET SESSION sql_mode = CONCAT(@@SESSION.sql_mode, ',STRICT_ALL_TABLES')",
                       timeout, DZ_EC_DB_OPEN_FAILED, "set_strict_mode");
        const std::string sql_mode = read_scalar_text(
            run_text("SELECT @@SESSION.sql_mode", timeout, DZ_EC_DB_OPEN_FAILED, "read_sql_mode"),
            "sql_mode");
        if (sql_mode.find("STRICT_ALL_TABLES") == std::string::npos) {
            throw Exception(DZ_EC_DB_OPEN_FAILED, "mysql strict mode not applied | sql_mode=\"{}\"",
                            sql_mode);
        }
        if (read_only_) {
            (void)run_text("SET SESSION TRANSACTION READ ONLY", timeout, DZ_EC_DB_OPEN_FAILED,
                           "set_read_only");
        }
    }

    template <typename Initiate>
    boost::mysql::error_code run_with_deadline(std::chrono::milliseconds timeout, DzErrorCode code,
                                               std::string_view op, Initiate&& initiate) {
        boost::mysql::any_connection& conn = require_connection();
        boost::mysql::error_code ec;
        bool completed = false;
        bool timed_out = false;
        boost::asio::steady_timer timer(ctx_);
        if (timeout.count() > 0) {
            timer.expires_after(timeout);
            timer.async_wait([&](const boost::system::error_code& timer_ec) {
                if (timer_ec || completed) {
                    return;
                }
                timed_out = true;
                ctx_.stop();  // 放弃等待；连接随即作废（不能复用半途的连接）
            });
        }
        diag_.clear();
        initiate(conn, [&](boost::mysql::error_code operation_ec) {
            ec = operation_ec;
            completed = true;
            timer.cancel();
        });
        ctx_.restart();
        ctx_.run();
        if (timed_out) {
            broken_ = true;
            conn_.reset();
            throw Exception(code, "mysql operation timed out | op={} timeout_ms={}", op,
                            timeout.count());
        }
        return ec;
    }

    boost::mysql::any_connection& require_connection() {
        if (broken_ || !conn_.has_value()) {
            throw Exception(DZ_EC_DB_CLOSED, "mysql connection is not open | database={}",
                            options_.database);
        }
        return *conn_;
    }

    /// 语句缓存：同一 SQL 只 prepare 一次；超过上限关闭最旧一条
    const boost::mysql::statement& cached_statement(std::string_view sql,
                                                    std::chrono::milliseconds timeout,
                                                    DzErrorCode code, std::string_view op) {
        const auto found = statements_.find(sql);
        if (found != statements_.end()) {
            return found->second;
        }
        boost::mysql::statement statement;
        const boost::mysql::error_code ec = run_with_deadline(
            timeout, code, "prepare", [&](boost::mysql::any_connection& conn, auto handler) {
                conn.async_prepare_statement(
                    sql, diag_,
                    [&statement, handler](boost::mysql::error_code prepare_ec,
                                          boost::mysql::statement prepared) {
                        statement = prepared;
                        handler(prepare_ec);
                    });
            });
        if (ec) {
            throw_operation_error(ec, code, op);
        }
        if (statements_.size() >= kMaxCachedStatements) {
            const std::string oldest = statement_order_.front();
            statement_order_.pop_front();
            const auto stale = statements_.find(oldest);
            if (stale != statements_.end()) {
                boost::mysql::any_connection& conn = require_connection();
                boost::mysql::error_code close_ec;
                conn.close_statement(stale->second, close_ec, diag_);
                statements_.erase(stale);
            }
        }
        std::string key(sql);
        statement_order_.push_back(key);
        return statements_.emplace(std::move(key), statement).first->second;
    }

    [[noreturn]] void throw_operation_error(const boost::mysql::error_code& ec, DzErrorCode code,
                                            std::string_view op) const {
        throw Exception(code, "mysql operation failed | op={} error=\"{}\"", op,
                        error_text(ec, diag_));
    }

    MysqlOptions options_;
    boost::asio::io_context ctx_;  ///< 先于 conn_ 声明 → conn_ 先析构
    std::unique_ptr<boost::asio::ssl::context> ssl_context_;  ///< 证书校验模式才构造，须活过连接
    std::optional<boost::mysql::any_connection> conn_;
    boost::mysql::diagnostics diag_;
    std::map<std::string, boost::mysql::statement, std::less<>> statements_;
    std::deque<std::string> statement_order_;
    bool read_only_ = false;
    bool broken_ = false;
};

// ============================================================
// 结果读取（按 schema 声明类型）
// ============================================================

std::int64_t read_integer(const boost::mysql::field_view& field, std::string_view column,
                          std::string_view collection) {
    switch (field.kind()) {
        case boost::mysql::field_kind::int64: return field.as_int64();
        case boost::mysql::field_kind::uint64: return static_cast<std::int64_t>(field.as_uint64());
        case boost::mysql::field_kind::double_: return static_cast<std::int64_t>(field.as_double());
        case boost::mysql::field_kind::string:
            // FIX-2：SUM() 等聚合在 MySQL/MariaDB 返回 DECIMAL，二进制协议以字符串传回；
            // 按 schema 声明的 Int64 列解析（整数和不会带小数位），不用文本协议猜测类型
            return parse_int64_text(as_view(field.as_string()), column, collection);
        default: throw_column_type_mismatch(column, collection, "non-integer");
    }
}

double read_float(const boost::mysql::field_view& field, std::string_view column,
                  std::string_view collection) {
    switch (field.kind()) {
        case boost::mysql::field_kind::double_: return field.as_double();
        case boost::mysql::field_kind::float_: return static_cast<double>(field.as_float());
        case boost::mysql::field_kind::int64: return static_cast<double>(field.as_int64());
        case boost::mysql::field_kind::uint64: return static_cast<double>(field.as_uint64());
        case boost::mysql::field_kind::string:
            // FIX-2：AVG()/DECIMAL 结果在二进制协议里是字符串，显式解析为 double
            return parse_double_text(as_view(field.as_string()), column, collection);
        default: throw_column_type_mismatch(column, collection, "non-float");
    }
}

std::string read_string(const boost::mysql::field_view& field, std::string_view column,
                        std::string_view collection) {
    switch (field.kind()) {
        case boost::mysql::field_kind::string:
            return to_display(field.as_string());
        case boost::mysql::field_kind::blob: {
            const boost::mysql::blob_view blob = field.as_blob();
            return std::string(reinterpret_cast<const char*>(blob.data()), blob.size());
        }
        default: throw_column_type_mismatch(column, collection, "non-string");
    }
}

Value read_column(const boost::mysql::field_view& field, ValueType type, std::string_view column,
                  std::string_view collection) {
    if (field.is_null()) {
        return std::monostate{};
    }
    switch (type) {
        case ValueType::Bool: return read_integer(field, column, collection) != 0;
        case ValueType::Int64: return read_integer(field, column, collection);
        case ValueType::Float64: return read_float(field, column, collection);
        case ValueType::String: return read_string(field, column, collection);
        case ValueType::Null:
            throw Exception(DZ_EC_DB_QUERY_FAILED,
                            "unexpected value for null column | column={} collection={}", column,
                            collection);
    }
    return std::monostate{};
}

/// 过滤条件参数按目标列类型归一化（顺序与 compile_filter 的 `?` 一致，见 sql_compiler.h）
void bind_filter_params(BoundParams& params, std::span<const Condition> conditions,
                        const ResourceSchema& schema, const CompiledFilter& compiled) {
    size_t expected = 0;
    for (const Condition& condition : conditions) {
        const FieldSchema* field = find_field(schema, condition.field);
        if (field == nullptr) {
            continue;  // compile_filter 已对未注册字段抛错
        }
        if (condition.op == CompareOp::In) {
            if (condition.values.empty()) {
                continue;  // 空 IN 编译为常量 0，无参数
            }
            for (const Value& value : condition.values) {
                params.add(value, *field, schema);
                ++expected;
            }
            continue;
        }
        if (condition.values.empty()) {
            continue;  // compile_filter 已抛 DZ_EC_INVALID_PARAM
        }
        params.add(condition.values.front(), *field, schema);
        ++expected;
    }
    if (expected != compiled.params.size()) {
        throw Exception(DZ_EC_INTERNAL,
                        "compiled filter parameter count mismatch | expected={} actual={}",
                        expected, compiled.params.size());
    }
}

std::string_view aggregate_column_name(AggregateOp op) {
    switch (op) {
        case AggregateOp::Count: return "count";
        case AggregateOp::Max: return "max";
        case AggregateOp::Min: return "min";
        case AggregateOp::Sum: return "sum";
        case AggregateOp::Avg: return "avg";
    }
    return "count";
}

}  // namespace dztrader::db::internal

namespace dztrader::db {

using internal::BoundParams;
using internal::MysqlConnection;

MysqlDatabaseImpl::MysqlDatabaseImpl(internal::MysqlOptions options,
                                     std::span<const ResourceSchema> schemas)
    : options_(std::move(options)), schemas_(schemas.begin(), schemas.end()) {
    for (const ResourceSchema& schema : schemas_) {
        schema_by_name_.emplace(schema.name, &schema);
    }
}

const ResourceSchema* MysqlDatabaseImpl::find_schema(std::string_view name) const {
    const auto it = schema_by_name_.find(name);
    return it == schema_by_name_.end() ? nullptr : it->second;
}

Capability MysqlDatabaseImpl::capabilities() const noexcept {
    return Capability::Transactions | Capability::SnapshotRead | Capability::Upsert |
           Capability::OrderedRangeScan;
}

std::unique_ptr<Session> MysqlDatabaseImpl::session(bool read_only) {
    auto connection = std::make_unique<MysqlConnection>(options_, read_only);
    connection->connect();  // 与 sqlite 驱动一致：session() 即校验可用性
    return std::make_unique<MysqlSession>(this, std::move(connection), read_only);
}

void MysqlDatabaseImpl::migrate() {
    if (!options_.schema_version.has_value()) {
        throw Exception(DZ_EC_INVALID_PARAM,
                        "mysql schema_version option is required for migrate | database={}",
                        options_.database);
    }
    const std::int64_t code_version = *options_.schema_version;
    try {
        MysqlConnection connection(options_, /*read_only=*/false);
        connection.connect();
        const std::chrono::milliseconds timeout = connection.read_timeout();
        for (const ResourceSchema& schema : schemas_) {
            (void)connection.run_text(internal::build_mysql_create_table_sql(schema), timeout,
                                      DZ_EC_DB_MIGRATE_FAILED, "migrate_ddl");
        }
        (void)connection.run_text(internal::mysql_schema_version_ddl(), timeout,
                                  DZ_EC_DB_MIGRATE_FAILED, "migrate_version_ddl");

        const boost::mysql::results result =
            connection.run_text(internal::mysql_schema_version_select(), timeout,
                                DZ_EC_DB_MIGRATE_FAILED, "migrate_version_read");
        const boost::mysql::field_view version_field =
            result.rows().empty() ? boost::mysql::field_view() : result.rows()[0][0];
        if (version_field.is_null()) {
            // 空库：写入当前版本（version 为主键，并发重复时幂等刷新 applied_at）
            const std::string applied_at = internal::utc_timestamp();
            const boost::mysql::field_view params[] = {
                boost::mysql::field_view(code_version),
                boost::mysql::field_view(
                    boost::mysql::string_view(applied_at.data(), applied_at.size()))};
            (void)connection.run_prepared(internal::mysql_schema_version_insert(), params, timeout,
                                          DZ_EC_DB_MIGRATE_FAILED, "migrate_version_write");
            return;
        }
        const std::int64_t db_version = internal::read_integer(
            version_field, "version", internal::kMysqlSchemaVersionTable);
        if (db_version < code_version) {
            throw Exception(DZ_EC_DB_MIGRATE_FAILED,
                            "mysql schema version is older than code | db_version={} "
                            "code_version={}",
                            db_version, code_version);
        }
        if (db_version > code_version) {
            throw Exception(DZ_EC_DB_MIGRATE_FAILED,
                            "mysql schema version is newer than code | db_version={} "
                            "code_version={}",
                            db_version, code_version);
        }
    } catch (const Exception&) {
        throw;
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_MIGRATE_FAILED, "mysql migrate failed | error=\"{}\"", e.what());
    }
}

MysqlSession::MysqlSession(const MysqlDatabaseImpl* owner,
                           std::unique_ptr<internal::MysqlConnection> connection, bool read_only)
    : owner_(owner), conn_(std::move(connection)), read_only_(read_only) {}

MysqlSession::~MysqlSession() = default;

const ResourceSchema& MysqlSession::require_schema(std::string_view collection) const {
    const ResourceSchema* schema = owner_->find_schema(collection);
    if (schema == nullptr) {
        throw Exception(DZ_EC_DB_QUERY_FAILED, "collection not registered | collection={}",
                        collection);
    }
    return *schema;
}

void MysqlSession::begin_scope(Scope kind) {
    if (scope_ != Scope::None) {
        throw Exception(DZ_EC_DB_TRANSACTION_FAILED, "scope already active");
    }
    const bool is_transaction = kind == Scope::Transaction;
    (void)conn_->run_text(is_transaction ? "START TRANSACTION"
                                         : "START TRANSACTION WITH CONSISTENT SNAPSHOT",
                          conn_->read_timeout(), DZ_EC_DB_TRANSACTION_FAILED,
                          is_transaction ? "begin_transaction" : "begin_snapshot");
    scope_ = kind;
}

void MysqlSession::end_scope(bool commit) {
    if (scope_ == Scope::None) {
        return;
    }
    if (commit) {
        (void)conn_->run_text("COMMIT", conn_->read_timeout(), DZ_EC_DB_TRANSACTION_FAILED,
                              "commit");  // 失败时保持 scope_, 由析构兜底回滚
        scope_ = Scope::None;
        return;
    }
    try {
        (void)conn_->run_text("ROLLBACK", conn_->read_timeout(), DZ_EC_DB_TRANSACTION_FAILED,
                              "rollback");  // 失败路径: 不抛 (调用方已在异常处理中)
    } catch (...) {
    }
    scope_ = Scope::None;
}

void MysqlSession::upsert(std::string_view collection, std::span<const Row> rows) {
    if (read_only_) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "session is read-only");
    }
    if (scope_ == Scope::Snapshot) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "upsert inside snapshot is not allowed");
    }
    if (rows.empty()) {
        return;
    }
    const ResourceSchema& schema = require_schema(collection);
    const std::string sql = internal::build_mysql_upsert_sql(schema);
    const bool owned_scope = scope_ == Scope::None;
    try {
        if (owned_scope) {
            begin_scope(Scope::Transaction);  // 多行批量原子化
        }
        for (const Row& row : rows) {
            if (row.size() != schema.fields.size()) {
                throw Exception(DZ_EC_DB_WRITE_FAILED,
                                "row field count mismatch | collection={} expected={} actual={}",
                                collection, schema.fields.size(), row.size());
            }
            BoundParams params;
            for (size_t i = 0; i < row.size(); ++i) {
                params.add(row.values()[i], schema.fields[i], schema);
            }
            (void)conn_->run_prepared(sql, params.views(), conn_->write_timeout(),
                                      DZ_EC_DB_WRITE_FAILED, "upsert");
        }
        if (owned_scope) {
            end_scope(/*commit=*/true);
        }
    } catch (const Exception&) {
        if (owned_scope) {
            end_scope(/*commit=*/false);
        }
        throw;
    } catch (const std::exception& e) {
        if (owned_scope) {
            end_scope(/*commit=*/false);
        }
        throw Exception(DZ_EC_DB_WRITE_FAILED, "mysql upsert failed | collection={} error=\"{}\"",
                        collection, e.what());
    }
}

uint64_t MysqlSession::remove(std::string_view collection, const Filter& filter) {
    if (read_only_) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "session is read-only");
    }
    if (scope_ == Scope::Snapshot) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "remove inside snapshot is not allowed");
    }
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled =
        internal::compile_filter(filter.conditions(), schema, internal::kMysqlDialect);
    BoundParams params;
    internal::bind_filter_params(params, filter.conditions(), schema, compiled);
    const std::string sql = internal::build_delete_sql(schema, compiled, internal::kMysqlDialect);
    const boost::mysql::results result =
        conn_->run_prepared(sql, params.views(), conn_->write_timeout(), DZ_EC_DB_WRITE_FAILED,
                            "remove");
    return static_cast<uint64_t>(result.affected_rows());
}

ResultSet MysqlSession::find(std::string_view collection, const Filter& filter,
                             const FindOptions& options) {
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled =
        internal::compile_filter(filter.conditions(), schema, internal::kMysqlDialect);
    BoundParams params;
    internal::bind_filter_params(params, filter.conditions(), schema, compiled);
    std::vector<Column> columns;
    columns.reserve(schema.fields.size());
    for (const FieldSchema& field : schema.fields) {
        columns.push_back({std::string(field.name), field.type});
    }
    // 不限量（limit<0）的 LIMIT 文本由编译器按方言给出（mysql: 18446744073709551615）
    const std::string sql = internal::build_select_sql(schema, compiled, options.sort, options.offset,
                                                       options.limit, internal::kMysqlDialect);
    const boost::mysql::results result = conn_->run_prepared(
        sql, params.views(), conn_->read_timeout(), DZ_EC_DB_QUERY_FAILED, "find");
    std::vector<Row> rows;
    rows.reserve(result.rows().size());
    for (const boost::mysql::row_view& row : result.rows()) {
        std::vector<Value> values;
        values.reserve(schema.fields.size());
        for (size_t i = 0; i < schema.fields.size(); ++i) {
            values.push_back(internal::read_column(row[i], schema.fields[i].type,
                                                   schema.fields[i].name, schema.name));
        }
        rows.emplace_back(std::move(values));
    }
    return ResultSet(std::move(columns), std::move(rows));
}

ResultSet MysqlSession::aggregate(std::string_view collection, const Aggregation& aggregation) {
    const ResourceSchema& schema = require_schema(collection);
    const internal::CompiledFilter compiled =
        internal::compile_filter(aggregation.filter.conditions(), schema, internal::kMysqlDialect);
    BoundParams params;
    internal::bind_filter_params(params, aggregation.filter.conditions(), schema, compiled);
    std::vector<Column> columns;
    std::vector<ValueType> value_types;
    columns.reserve(aggregation.group_by.size() + 1);
    value_types.reserve(aggregation.group_by.size() + 1);
    for (const std::string& field_name : aggregation.group_by) {
        const FieldSchema* field = internal::find_field(schema, field_name);
        if (field == nullptr) {
            throw Exception(DZ_EC_DB_QUERY_FAILED, "unknown group field | field={} collection={}",
                            field_name, schema.name);
        }
        columns.push_back({std::string(field->name), field->type});
        value_types.push_back(field->type);
    }
    ValueType aggregate_type = ValueType::Int64;
    if (aggregation.op != AggregateOp::Count) {
        const FieldSchema* field = internal::find_field(schema, aggregation.field);
        if (field == nullptr) {
            throw Exception(DZ_EC_DB_QUERY_FAILED,
                            "unknown aggregate field | field={} collection={}", aggregation.field,
                            schema.name);
        }
        aggregate_type = aggregation.op == AggregateOp::Avg ? ValueType::Float64 : field->type;
    }
    columns.push_back({std::string(internal::aggregate_column_name(aggregation.op)), aggregate_type});
    value_types.push_back(aggregate_type);
    const std::string sql =
        internal::build_aggregate_sql(schema, aggregation.op, aggregation.field,
                                      aggregation.group_by, compiled, aggregation.sort,
                                      internal::kMysqlDialect);
    const boost::mysql::results result = conn_->run_prepared(
        sql, params.views(), conn_->read_timeout(), DZ_EC_DB_QUERY_FAILED, "aggregate");
    std::vector<Row> rows;
    rows.reserve(result.rows().size());
    for (const boost::mysql::row_view& row : result.rows()) {
        std::vector<Value> values;
        values.reserve(value_types.size());
        for (size_t i = 0; i < value_types.size(); ++i) {
            values.push_back(
                internal::read_column(row[i], value_types[i], columns[i].name, schema.name));
        }
        rows.emplace_back(std::move(values));
    }
    return ResultSet(std::move(columns), std::move(rows));
}

std::unique_ptr<Transaction> MysqlSession::begin_transaction() {
    if (read_only_) {
        throw Exception(DZ_EC_PERMISSION_DENIED, "session is read-only");
    }
    if (scope_ == Scope::Snapshot) {
        throw Exception(DZ_EC_PERMISSION_DENIED,
                        "begin_transaction inside snapshot is not allowed");
    }
    begin_scope(Scope::Transaction);
    return std::make_unique<MysqlTransaction>(*this);
}

std::unique_ptr<Snapshot> MysqlSession::begin_snapshot() {
    if (!has(owner_->capabilities(), Capability::SnapshotRead)) {
        throw Exception(DZ_EC_DB_UNSUPPORTED_CAPABILITY, "snapshot read is not supported");
    }
    begin_scope(Scope::Snapshot);
    return std::make_unique<MysqlSnapshot>(*this);
}

MysqlTransaction::~MysqlTransaction() {
    if (finished_) {
        return;
    }
    try {
        session_->end_scope(/*commit=*/false);
    } catch (...) {
    }
}

void MysqlTransaction::commit() {
    if (finished_) {
        throw Exception(DZ_EC_DB_TRANSACTION_FAILED, "transaction already finished");
    }
    session_->end_scope(/*commit=*/true);
    finished_ = true;  // 仅在 COMMIT 成功后失效句柄
}

void MysqlTransaction::rollback() noexcept {
    if (finished_) {
        return;  // noexcept 接口无法抛错; 已结束句柄的 rollback 为无操作
    }
    try {
        session_->end_scope(/*commit=*/false);
    } catch (...) {
    }
    finished_ = true;
}

MysqlSnapshot::~MysqlSnapshot() {
    try {
        session_->end_scope(/*commit=*/true);  // 只读事务 COMMIT 即结束
    } catch (...) {
        try {
            session_->end_scope(/*commit=*/false);
        } catch (...) {
        }
    }
}

std::unique_ptr<Database> open_mysql(const Config& config,
                                     std::span<const ResourceSchema> schemas) {
    return std::make_unique<MysqlDatabaseImpl>(internal::parse_mysql_options(config.options),
                                               schemas);
}

}  // namespace dztrader::db
