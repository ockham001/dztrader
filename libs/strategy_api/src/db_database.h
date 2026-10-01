/**
 * @file db_database.h
 * @brief DzDatabase 不透明句柄实现体 + 库查询内部实现
 *
 * api.cpp 的 C 接口 (dz_db_*) 仅做 ABI 包装: 参数校验 + 异常转 LastError,
 * 查询构造 (类型化 Filter/Aggregation + Session) 全部落在此文件。
 */
#ifndef DZTRADER_STRATEGY_API_DB_DATABASE_H_
#define DZTRADER_STRATEGY_API_DB_DATABASE_H_

#include <chrono>
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
 * @brief 只读打开 td 库 (显式 SQLite 路径, dz_db_open 专用, 语义不变)
 * @param path 库文件路径
 * @return 非空句柄; 失败 (文件不存在/非库/权限) 抛异常 (调用方转 LastError)
 */
std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path);

/**
 * @brief 按配置只读打开 td 库 (后端由配置决定)
 * @param config 由 db::resolve_config("td") 解析出的配置 (backend + options)
 * @return 非空句柄; 失败 (后端未注册/库不存在/连不上) 抛异常 (调用方转 LastError)
 *
 * 供 SDK 内部 ingest (open_td_db) 与公开 C 接口 dz_db_open_default 共用;
 * 会话固定 read_only, 列序 = tdstore::schemas()。
 */
std::unique_ptr<DzDatabase> db_open_readonly_config(const dztrader::db::Config& config);

/**
 * @brief ingest 库打开失败退避窗口（WP3b：MySQL 故障期热路径保护）
 *
 * 取 1000ms 的理由：MySQL `connect_timeout_ms` 默认 5000ms，gap 回补重试窗口内
 * 每个 `dz_next_event` 都会请求 ingest 库；无退避时事件循环被秒级阻塞。1s 把
 * "服务器不可达"期间的重试频率限制为每秒一次，恢复延迟上限同为一个窗口，
 * 对策略可见行为只有"降级不过滤"（语义不变）。SQLite 本地文件打开失败廉价，
 * 同样按此窗口重试，不额外区分后端。
 */
inline constexpr std::chrono::milliseconds kIngestFailureBackoffMs{1000};

/**
 * @brief ingest 库句柄缓存获取结果 (FIX-5/FIX-7: 热路径复用 + WP3b 退避)
 */
struct IngestDbAcquire {
    DzDatabase* db = nullptr;  ///< 非空 = 可复用句柄 (缓存持有, 调用方不得 close)
    bool report = false;       ///< true = 本次失败应输出诊断 (同一配置只置一次, FIX-3)
    std::string error;         ///< 失败原因 (report = true 时非空; 退避跳过时为空)
};

/**
 * @brief 取进程内缓存的 ingest 只读句柄 (不抛异常)
 *
 * 缓存键 = config 全等 (backend + options): 命中则复用同一 `DzDatabase`
 * (内部已持一个只读 Session), 配置变化则关闭旧句柄并按新配置重开——
 * `dz_next_event` → gap 回补重试窗口内每次调用都会请求库, MySQL 下不复用等于
 * 每次事件 TCP + 认证; 配置变化重开同时保证测试换 DZTRADER_HOME/环境变量后可隔离。
 *
 * 打开失败不缓存 (窗口过后按重新解析出的配置再试一次), 且同一配置只报一次
 * (report = true 仅首次; 该配置自己恢复后去重状态复位)。失败退避 (WP3b):
 * 同配置失败后 `kIngestFailureBackoffMs` 窗口内的调用直接判"库不可用"返回
 * (不发起连接、不重复诊断), 窗口过后重试、再失败再退避; 退避与去重均按配置隔离,
 * 换配置立即可试。配置解析失败不经过本函数 (FIX-3 路径在 open_td_db 中先于本调用),
 * 不受退避影响。
 *
 * 返回的句柄由进程级缓存持有, 生命周期到进程结束; Session 非线程安全,
 * 调用方须遵守"一线程一会话"既有契约 (dz_next_event 单线程)。
 *
 * @param config 由 db::resolve_config("td") 解析出的配置
 */
[[nodiscard]] IngestDbAcquire acquire_ingest_db(const dztrader::db::Config& config);

/**
 * @brief 仅测试: 覆盖打开失败退避窗口 (0ms = 关闭退避); 生产代码不调用
 */
void set_ingest_failure_backoff_for_test(std::chrono::milliseconds backoff);

/**
 * @brief 仅测试: 清空 ingest 缓存 (句柄/失败去重/退避/尝试计数) 并恢复默认退避
 */
void reset_ingest_cache_for_test();

/**
 * @brief 仅测试: 自上次 reset 起的真实打开尝试次数 (退避跳过的调用不计)
 */
[[nodiscard]] uint64_t ingest_open_attempts_for_test();

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
