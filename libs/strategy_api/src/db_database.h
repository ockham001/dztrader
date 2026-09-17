/**
 * @file db_database.h
 * @brief DzDatabase 不透明句柄实现体 + 库查询内部实现
 *
 * api.cpp 的 C 接口 (dz_db_*) 仅做 ABI 包装: 参数校验 + 异常转 LastError,
 * 查询构造 (类型化 Filter/Aggregation + Session) 全部落在此文件。
 */
#ifndef DZTRADER_STRATEGY_API_DB_DATABASE_H_
#define DZTRADER_STRATEGY_API_DB_DATABASE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <dztrader/db/database.h>

#include "result_set_impl.h"

/** @brief 数据库句柄实现体（api.h 的 DzDatabase 为不透明指针） */
struct DzDatabase {
    std::unique_ptr<dztrader::db::Database> database;
    std::unique_ptr<dztrader::db::Session> session;
};

namespace dztrader::strategy_api_internal {

/// 一次查询的完整结果 (columns + rows), 由 api.cpp 装入 VectorResultSet。
struct DbQueryResult {
    std::vector<ColumnMeta> columns;
    std::vector<Row> rows;
};

/// 后端无关查询结果 (db::ResultSet) -> SDK 内部结果; 列类型映射 db::ValueType -> DzColumnType
DbQueryResult to_db_query_result(dztrader::db::ResultSet result);

/**
 * @brief 只读打开 td 库
 * @param path 库文件路径
 * @return 非空句柄; 失败 (文件不存在/非库/权限) 返回 nullptr (调用方置 LastError)
 */
std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path);

/**
 * @brief 按账户/合约条件查询 order/trade 表 (同构: account_id + instrument_id)
 * @param table "orders" / "trades"
 * @param order_by_seq 按 seq 升序 (orders/trades 有 seq 列)
 *
 * 列序 = tdstore::orders_schema()/trades_schema() 的字段声明序 (即建表列序):
 *   orders:     0=id 1=account_id 2=trading_day 3=order_id 4=order_ref
 *               5=external_order_id 6=is_external 7=instrument_id 8=exchange_id
 *               9=direction 10=position_effect 11=price_type 12=status
 *               13=price 14=volume 15=volume_traded 16=volume_canceled
 *               17=insert_time 18=update_time 19=error_id 20=error_msg
 *               21=strategy_id 22=remark 23=seq
 *   trades:     0=id 1=account_id 2=trading_day 3=trade_id 4=order_id
 *               5=instrument_id 6=exchange_id 7=direction 8=position_effect
 *               9=price 10=volume 11=trade_time 12=trade_date 13=commission
 *               14=strategy_id 15=seq
 * 空串条件表示不限定。异常抛给调用方。
 */
DbQueryResult db_query_order_trade(DzDatabase* db,
                                   const std::string& account_id,
                                   const std::string& instrument_id,
                                   const std::string& table,
                                   bool order_by_seq);

/**
 * @brief 按账户/合约条件查询 positions 表
 *
 * 列序 = tdstore::positions_schema() 字段声明序:
 *       0=account_id 1=trading_day 2=instrument_id 3=exchange_id 4=direction
 *       5=volume 6=frozen_volume 7=today_volume 8=yd_volume 9=price 10=seq
 * 行序 = seq 升序。空串条件表示不限定。
 */
DbQueryResult db_query_position(DzDatabase* db,
                                const std::string& account_id,
                                const std::string& instrument_id);

/**
 * @brief 按账户查询 trading_accounts 表 (单行或全量)
 *
 * 列序 = tdstore::trading_accounts_schema() 字段声明序:
 *       0=account_id 1=trading_day 2=balance 3=available 4=frozen
 *       5=commission 6=margin 7=withdraw_quota 8=deposit 9=withdraw 10=seq
 * 空串 account_id 表示不限定 (全量)。行序 = seq 升序。
 */
DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id);

/**
 * @brief 按账户 + seq 区间查询资源表 (回补用)
 * @param resource 资源路径 ("order"/"trade"/"position"/"trading_account")
 * @param from 区间下界 (含)
 * @param to 区间上界 (含)
 *
 * 行序 = seq 升序。未知资源抛异常。
 */
DbQueryResult db_query_seq_range(DzDatabase* db,
                                 const std::string& resource,
                                 const std::string& account_id,
                                 uint64_t from,
                                 uint64_t to);

/**
 * @brief 按账户分组求 MAX(seq) (水位装载用, 终检发现 E: 替代 SELECT * 全行物化 —
 * 库随历史线性增长时全表装载线性恶化; 聚合查询只物化每账户一行)。
 * @param resource "order"/"trade"/"position"/"trading_account" (仅 seq 表)
 * @return account_id -> MAX(seq)。表缺失/查询失败抛异常 (调用方逐表容错)。
 */
std::unordered_map<std::string, uint64_t> db_query_max_seq_by_account(
    DzDatabase* db, const std::string& resource);

/**
 * @brief 单账户 MAX(seq) (水位重建用, 终检发现 E: 替代该账户全行物化)。
 * @param resource 同 db_query_max_seq_by_account
 * @return 该账户 MAX(seq); 无行返回 0。表缺失/查询失败抛异常。
 */
uint64_t db_query_account_max_seq(DzDatabase* db,
                                  const std::string& resource,
                                  const std::string& account_id);

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_DB_DATABASE_H_
