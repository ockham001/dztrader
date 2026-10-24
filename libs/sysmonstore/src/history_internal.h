#ifndef DZTRADER_SYSMONSTORE_SRC_HISTORY_INTERNAL_H_
#define DZTRADER_SYSMONSTORE_SRC_HISTORY_INTERNAL_H_

/**
 * @file history_internal.h
 * @brief 历史库实现内部共享件（pimpl 定义、SQL 片段、聚合绑定、文件大小、版本读取）。
 *
 * 不安装、不对外暴露：schema 与行编解码的公开面在 `schema.h` / `records.h`，
 * 本文件只服务于写入/读取/保留几个翻译单元之间避免重复 SQL 拼装。
 *
 * `HistoryStore::Impl` / `HistoryReader::Impl` 在这里给出完整定义，使 SQLiteCpp 类型
 * 只出现在实现侧——公开头 `history_store.h` 不暴露 SQLite 头，消费方
 * （`dzsysmon` / `dzweb`）不会被动继承 SQLite 的编译选项。
 */

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {

/// 写入方实现：一条连接 + 预编译语句（落盘线程独占）。
struct HistoryStoreImpl {
    std::filesystem::path path;
    std::unique_ptr<SQLite::Database> db;
    std::unique_ptr<SQLite::Statement> scalar_upsert;
    std::unique_ptr<SQLite::Statement> dimension_upsert;
    std::unique_ptr<SQLite::Statement> mark_insert;
};

/// 读取方实现：只读连接（查询线程独占）。
struct HistoryReaderImpl {
    std::filesystem::path path;
    std::unique_ptr<SQLite::Database> db;
};

// pimpl 的实际定义：公开头里只前置声明了 `struct Impl`。
struct HistoryStore::Impl final : HistoryStoreImpl {};
struct HistoryReader::Impl final : HistoryReaderImpl {};

namespace internal {

/// 宽表 5 个聚合列后缀（顺序固定，与 schema.cpp 建表列序一致）。
inline constexpr std::string_view kAggregateSuffixes[] = {"min", "max", "avg", "last", "n"};

/// 把标识符按 SQL 规则加双引号转义（标识符均来自编译期常量，仍防御性转义）。
[[nodiscard]] std::string quote_identifier(std::string_view identifier);

/// 宽表 UPSERT 语句（同桶后写覆盖，使重试不产生重复行）。
[[nodiscard]] std::string scalar_upsert_sql();

/// 宽表 SELECT 列清单（紧接 `SELECT ` 之后，顺序与读取方解析顺序一致）。
[[nodiscard]] std::string scalar_select_columns();

/// 长表 UPSERT 语句。
[[nodiscard]] std::string dimension_upsert_sql();

/// 标记插入语句。
[[nodiscard]] std::string mark_insert_sql();

/**
 * @brief 绑定一个聚合的 5 个值（min/max/avg/last/n）。
 *
 * `integral` 为 true 时 min/max/last 按整数绑定（字节数/计数），否则按实数；
 * `avg` 恒为实数、`n` 恒为整数（与建表列类型一致，避免 SQLite 亲和性截断均值）。
 *
 * @param stmt      目标语句
 * @param index     1-based 起始参数位（引用传入，返回时已推进 5）
 * @param aggregate 聚合量（`count > 0`）
 * @param integral  是否整型指标
 */
void bind_aggregate(SQLite::Statement& stmt,
                    int& index,
                    const MetricAggregate& aggregate,
                    bool integral);

/// 绑定 NULL 到 5 个聚合参数位（该桶内指标无有效样本）。
void bind_null_aggregate(SQLite::Statement& stmt, int& index);

/// 写入一条宽表行（使用 `impl.scalar_upsert`；调用方负责事务）。
void write_scalar_row(HistoryStoreImpl& impl, const ScalarRecord& record);

/// 写入一条长表行（使用 `impl.dimension_upsert`；调用方负责事务）。
/// 未在 schema 注册的域/指标名跳过并告警（schema 是唯一真相源）。
void write_dimension_row(HistoryStoreImpl& impl, const DimensionRecord& record);

/// 插入一条标记（调用方负责事务）。
void insert_mark(const SQLite::Database& db, const HistoryMark& mark);

/// 从查询结果当前位置解析一行宽表记录（列序与 `scalar_select_columns` 一致）。
[[nodiscard]] ScalarRecord read_scalar_row(const SQLite::Statement& stmt);

/// 库文件总字节数（主文件 + `-wal` + `-shm`；不存在计 0）。
[[nodiscard]] int64_t history_file_bytes(const std::filesystem::path& path);

/// 读取 `sysmon_meta` 中的 schema 版本；表/行不存在返回 0。
[[nodiscard]] int read_schema_version(SQLite::Database& db);

/// 建目录（父目录为空时无操作）。
/// @throws std::runtime_error 创建失败
void ensure_parent_dir(const std::filesystem::path& path);

}  // namespace internal
}  // namespace dztrader::sysmonstore

#endif  // DZTRADER_SYSMONSTORE_SRC_HISTORY_INTERNAL_H_
