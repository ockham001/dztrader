/**
 * @file trade_db.h
 * @brief 交易库（trade.db）建库/重建门面（master 启动调用；ADR 0041）
 *
 * 职责：
 * - 首次启动建库：按 `build_schema_ddl()` 建四张交易表 + instruments + 索引，
 *   `PRAGMA user_version = kTdSchemaVersion`；
 * - 旧版本库 / 损坏库：改名保留（不删除）后新建空库——开发期无迁移链，旧 dev 库直接丢弃；
 * - 重复启动幂等：版本一致则只做幂等 DDL 自愈（缺失表/索引补建）；
 * - 供 master 播种共享 `order_id` 计数器读取库内 `MAX(order_id)`（走 idx_orders_order_id）。
 *
 * 子进程（td）不负责建库：master 先建库再拉起子进程（#245 起 td 直连 SQLiteCpp）。
 */
#ifndef DZTRADER_TDSTORE_TRADE_DB_H_
#define DZTRADER_TDSTORE_TRADE_DB_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace dztrader::tdstore {

/// 建库/重建结果（master 记录 ERROR/INFO 日志用）
struct TradeDbInitReport {
    enum class Status : uint8_t {
        Created,   ///< 首次建库（文件不存在）
        Existing,  ///< 已存在且版本一致（幂等启动）
        Rebuilt,   ///< 旧版本/损坏库已改名保留并新建
    };

    Status status = Status::Existing;
    int previous_version = 0;    ///< Existing/Rebuilt: 探测到的 user_version（0=无版本）
    std::string backup_path;     ///< Rebuilt: 旧库改名后的主文件路径（空=未改名）
    std::string rebuild_reason;  ///< Rebuilt: schema_version_mismatch / corrupt_or_unreadable
};

/// 建库/重建 + 版本校验（幂等）。
///
/// 分类（ADR 0041 §5 / ADR 0042 §3）：
/// - Created / Existing / Rebuilt：缺失建库、版本一致幂等自愈、旧版本或明确损坏
///   （SQLITE_CORRUPT / SQLITE_NOTADB）改名保留后新建；
/// - 可重试失败（并发锁 busy/locked、路径非普通文件、打开失败等）抛
///   std::runtime_error 且不改名、不建库 —— 调用方退避重连（健康库可能只是暂时不可用）。
TradeDbInitReport ensure_trade_db(const std::filesystem::path& path);

/// 读交易库 orders 表 MAX(order_id)（走 idx_orders_order_id；空表返回 0）。文件不存在抛异常。
int64_t read_max_order_id(const std::filesystem::path& path);

/// 旧库/坏库改名保留（主文件 + `-wal`/`-shm` 伴生文件一起改名，不删除旧文件）；
/// 返回改名后主文件路径（`<path>.old-<timestamp>[-N]`）。供建库门面与 td 写端坏库
/// 重建共用（ADR 0041 §5 / ADR 0042 §3）。文件系统失败抛 std::runtime_error。
std::string quarantine_trade_db(const std::filesystem::path& path);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_TRADE_DB_H_
