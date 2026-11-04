#ifndef DZTRADER_WEBUI_TD_READ_DB_H_
#define DZTRADER_WEBUI_TD_READ_DB_H_

/**
 * @file td_read_db.h
 * @brief 交易库（trade.db）只读连接 / schema 探测 / 降级原因码的唯一实现
 *        （契约 rest §2.9.8）。
 *
 * 委托列表（`TdOrdersQuery`）与统计重算（`TdStatsQuery`）共用本文件的只读打开 +
 * `PRAGMA user_version` 校验，**禁止两份实现**：reason 码是契约 rest §2.9.8 定义的
 * 对外协议（前端据此分支），两份实现漂移会让读者看到不一致的降级语义。
 *
 * 连接语义：每次查询独立打开（REST 回调在同一 IO 线程串行）、`busy_timeout` 200ms，
 * 析构即 `sqlite3_close`。`open()` 在打开成功后再校验 schema 版本
 * （`kTdOrdersReadSchemaVersion`，唯一声明在 `td_orders_query.h`）。
 */

#include <nlohmann/json.hpp>

#include <string>

struct sqlite3;

namespace dztrader::webui {

/// 只读连接等锁上限（契约 rest §2.9：200ms，超时归 busy 而非无限阻塞 IO 线程）。
inline constexpr int kTdReadBusyTimeoutMs = 200;

/// 降级原因码（契约 rest §2.9.8）。
inline constexpr const char* kTdReasonNotFound = "not_found";
inline constexpr const char* kTdReasonSchemaMismatch = "schema_mismatch";
inline constexpr const char* kTdReasonOpenFailed = "open_failed";
inline constexpr const char* kTdReasonBusy = "busy";
inline constexpr const char* kTdReasonQueryFailed = "query_failed";
/// 统计计数器未就绪（契约 rest §2.9.5，只有 stats 用；与 §2.9.8 的库降级区分）。
inline constexpr const char* kTdReasonNotReady = "not_ready";

/// sqlite 错误码 → 契约 rest §2.9.8 reason 码。
///
/// 等锁失败 → busy；库损坏 / 非 SQLite 文件 → open_failed（`sqlite3_open` 是惰性的，
/// 文件头非法只在首次读页暴露）；其余 → query_failed。只读查询的准备/执行失败共用本映射。
const char* td_sqlite_error_reason(int code);

/// 已打开且通过 schema 版本校验的只读连接（RAII：析构 `sqlite3_close`）。
class TdReadConnection {
public:
    /// 打开失败 / 库不存在 / schema 版本不符 → `connected()==false` 且 `reason()` 有效。
    static TdReadConnection open(const std::string& trade_db_path, int busy_timeout_ms);

    ~TdReadConnection();

    // 独占句柄：禁拷贝，可移动
    TdReadConnection(const TdReadConnection&) = delete;
    TdReadConnection& operator=(const TdReadConnection&) = delete;
    TdReadConnection(TdReadConnection&& other) noexcept;
    TdReadConnection& operator=(TdReadConnection&& other) noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const char* reason() const noexcept;  ///< nullptr = 可用
    [[nodiscard]] sqlite3* handle() const noexcept;

private:
    TdReadConnection(sqlite3* db, const char* reason) noexcept;

    void close() noexcept;

    sqlite3* db_ = nullptr;
    const char* reason_ = nullptr;  ///< nullptr = 连接可用
};

/// 降级体 `{"available":false,"reason":<code>}`（契约 rest §2.9.8）。
nlohmann::json td_degraded_body(const char* reason);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_READ_DB_H_
