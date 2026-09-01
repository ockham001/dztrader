/**
 * @file db_database.h
 * @brief DzDatabase 不透明句柄实现体 + 库查询内部实现
 *
 * api.cpp 的 C 接口 (dz_db_*) 仅做 ABI 包装: 参数校验 + 异常转 LastError,
 * SQL 全部落在此文件。查询结果装入 VectorResultSet 的行容器, 不新建 impl 类。
 */
#ifndef DZTRADER_STRATEGY_API_DB_DATABASE_H_
#define DZTRADER_STRATEGY_API_DB_DATABASE_H_

#include <memory>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>

#include "result_set_impl.h"

/** @brief 数据库句柄实现体（api.h 的 DzDatabase 为不透明指针） */
struct DzDatabase {
    std::unique_ptr<SQLite::Database> db;
};

namespace dztrader::strategy_api_internal {

/// 一次查询的完整结果 (columns + rows), 由 api.cpp 装入 VectorResultSet。
struct DbQueryResult {
    std::vector<ColumnMeta> columns;
    std::vector<Row> rows;
};

/**
 * @brief 只读打开 td 库 (SQLITE_OPEN_READONLY)
 * @param path 库文件路径
 * @return 非空句柄; 失败 (文件不存在/非库/权限) 返回 nullptr (调用方置 LastError)
 */
std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path);

/**
 * @brief 按账户/合约条件查询 order/trade 表 (同构: account_id + instrument_id)
 * @param table "orders" / "trades"
 * @param order_by_seq 按 seq 升序 (orders/trades 有 seq 列); commission/margin 无 seq 列传 false
 *
 * 列序 = SELECT * 的表定义 (与 apps/ctp/td/td_schema.cpp 的 v2 建表语句一致):
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
 * 列序: 0=account_id 1=trading_day 2=instrument_id 3=exchange_id 4=direction
 *       5=volume 6=frozen_volume 7=today_volume 8=yd_volume 9=price 10=seq
 * 行序 = seq 升序。空串条件表示不限定。
 */
DbQueryResult db_query_position(DzDatabase* db,
                                const std::string& account_id,
                                const std::string& instrument_id);

/**
 * @brief 按账户查询 trading_accounts 表 (单行或全量)
 *
 * 列序: 0=account_id 1=trading_day 2=balance 3=available 4=frozen
 *       5=commission 6=margin 7=withdraw_quota 8=deposit 9=withdraw 10=seq
 * 空串 account_id 表示不限定 (全量)。行序 = seq 升序。
 */
DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id);

/**
 * @brief 结构化通用查询
 * @param query  资源路径 ("order"/"trade"/"position"/"trading_account"/"commission"/"margin")
 * @param filter JSON 过滤串 (可为空): {"field": value} 或 {"field": {"$gte":..,"$lt":..}}
 *               支持算子 $gte/$gt/$lte/$lt/$in; 多字段 AND 组合。
 *               回补区间: {"seq": {"$gte": W, "$lt": S0}}。
 *
 * 资源 -> 表 列序 (SELECT *):
 *   commission (commission_rates): 0=id 1=account_id 2=instrument_id 3=product_code
 *       4=exchange_id 5=open_ratio_by_money 6=open_ratio_by_volume
 *       7=close_ratio_by_money 8=close_ratio_by_volume
 *       9=close_today_ratio_by_money 10=close_today_ratio_by_volume 11=date
 *   margin (margin_rates): 0=id 1=account_id 2=instrument_id 3=product_code
 *       4=exchange_id 5=hedge_flag 6=is_relative 7=long_margin_ratio_by_money
 *       8=long_margin_ratio_by_volume 9=short_margin_ratio_by_money
 *       10=short_margin_ratio_by_volume 11=date
 * 仅 seq 表 (orders/trades/positions/trading_accounts) 行序 = seq 升序;
 * commission/margin 无 seq 列, 不排序。未知资源/未知字段/非法 JSON 抛异常。
 */
DbQueryResult db_generic_query(DzDatabase* db,
                               const std::string& query,
                               const std::string& filter);

}  // namespace dztrader::strategy_api_internal

#endif  // DZTRADER_STRATEGY_API_DB_DATABASE_H_
