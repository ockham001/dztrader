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

/// 建库/重建 + 版本校验（幂等）。失败抛 std::runtime_error（master 记录 ERROR 并降级）。
TradeDbInitReport ensure_trade_db(const std::filesystem::path& path);

/// 读交易库 orders 表 MAX(order_id)（走 idx_orders_order_id；空表返回 0）。文件不存在抛异常。
int64_t read_max_order_id(const std::filesystem::path& path);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_TRADE_DB_H_
