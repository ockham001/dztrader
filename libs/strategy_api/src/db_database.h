/**
 * @file db_database.h
 * @brief DzDatabase 不透明句柄实现体 + 交易库查询内部实现
 *
 * api.cpp 的 C 接口 (`dz_db_*`) 仅做 ABI 包装: 参数校验 + 异常转 LastError,
 * 查询构造 (类型化 Filter/Aggregation + Session) 全部落在此文件。
 *
 * 依赖方向（ADR 0044 决策 1）：本文件与 `src/db/` 是 SDK **私有** DB 层；
 * 平台侧（td / tdstore / dzweb）不链接端口，交易数据一律直连 SQLite。
 */
#ifndef DZTRADER_STRATEGY_API_DB_DATABASE_H_
#define DZTRADER_STRATEGY_API_DB_DATABASE_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <dztrader/data_type.h>
#include <dztrader/db/database.h>

#include "result_set_impl.h"

/**
 * @brief 数据库句柄实现体（api.h 的 DzDatabase 为不透明指针）
 *
 * 用 `shared_ptr` 持有的是**入口**（`db::Database`，文档保证线程安全、进程内共享）与
 * **本次调用独占的 Session**（`db::Session` 非线程安全、"一线程一 Session"）。
 * 默认库入口 `dz_db_open_default` 只缓存 `db::Database`，Session 每次新建，因此句柄之间
 * 不共享连接，`dz_db_close` 对任何句柄都只是销毁本次返回的包装（无重复关闭/悬垂）。
 */
struct DzDatabase {
    std::shared_ptr<dztrader::db::Database> database;
    std::shared_ptr<dztrader::db::Session> session;
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
 * @brief 只读打开交易库（显式 SQLite 路径; `dz_db_open` 与内部 ingest 共用同一实现）
 * @param path 库文件路径
 * @return 非空句柄; 失败 (文件不存在/非库/权限) 抛异常 (调用方转 LastError)
 */
std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path);

/**
 * @brief 交易库固定路径（`<DZTRADER_HOME>/db/trade.db`, ADR 0041）
 *
 * `dz_db_open_default` 与 SDK 内部 ingest 都只用这一个路径：交易库不再按配置选择
 * 后端/路径（ADR 0044 决策 3），`database.json` 的交易段不再生效。
 */
[[nodiscard]] std::string trade_db_path();

/**
 * @brief 默认库的新句柄（底层入口进程级懒打开并缓存，Session 每次新建）
 *
 * **首次调用**才建立 `db::Database` 入口（`dz_init` 不打开默认查询库，ADR 0044 决策 3），
 * 此后同进程内复用该入口；**每次调用新建 Session**（`Session` 非线程安全、一线程一
 * Session），因此每次返回的句柄都可独立使用、可跨线程各持一个、并可独立 `dz_db_close`。
 * 与 ingest 句柄分开缓存：ingest 是内部借用句柄（永不 close），本入口返回给调用方。
 *
 * @return 非空句柄所有权（失败抛异常）
 */
std::unique_ptr<DzDatabase> default_db_handle();

/**
 * @brief 仅测试: 释放默认库的进程级连接缓存（测试换 `DZTRADER_HOME` 后调用,
 *        否则 Windows 上被占用的库文件删不掉）; 生产代码不调用
 */
void reset_default_db_cache_for_test();

/**
 * @brief 仅测试: 默认库入口的真实打开尝试次数（含失败；证明 `dz_init` 没有提前打开它）
 */
[[nodiscard]] uint64_t default_db_open_attempts_for_test();

/**
 * @brief ingest 库打开失败退避窗口（WP3b：故障期热路径保护）
 *
 * 取 1000ms 的理由：gap 回补重试窗口内每个 `dz_next_event` 都会请求 ingest 库；
 * 无退避时若库不可用（磁盘错误 / 权限 / 被独占），事件循环被逐次打开失败拖慢。
 * 1s 把重试频率限制为每秒一次，恢复延迟上限同为一个窗口，对策略可见行为只有
 * "降级不过滤"（语义不变）。
 */
inline constexpr std::chrono::milliseconds kIngestFailureBackoffMs{1000};

/**
 * @brief ingest 库句柄缓存获取结果 (FIX-5/FIX-7: 热路径复用 + WP3b 退避)
 */
struct IngestDbAcquire {
    DzDatabase* db = nullptr;  ///< 非空 = 可复用句柄 (缓存持有, 调用方不得 close)
    bool report = false;       ///< true = 本次失败应输出诊断 (同一路径只置一次, FIX-3)
    std::string error;         ///< 失败原因 (report = true 时非空; 退避跳过时为空)
};

/**
 * @brief 取进程内缓存的 ingest 只读句柄 (不抛异常)
 *
 * 缓存键 = 库文件路径: 命中则复用同一 `DzDatabase` (内部已持一个只读 Session)。
 * `dz_next_event` → gap 回补重试窗口内每次调用都会请求库，不复用等于反复打开同一个
 * SQLite 文件。
 *
 * 打开失败不缓存 (窗口过后再试一次), 且同一路径只报一次 (report = true 仅首次;
 * 该路径恢复后去重状态复位)。失败退避 (WP3b): 同路径失败后 `kIngestFailureBackoffMs`
 * 窗口内的调用直接判"库不可用"返回 (不发起打开、不重复诊断), 窗口过后重试、再失败
 * 再退避; 退避与去重均按路径隔离。
 *
 * 返回的句柄由进程级缓存持有, 生命周期到进程结束; Session 非线程安全,
 * 调用方须遵守"一线程一会话"既有契约 (dz_next_event 单线程)。
 *
 * @param path 交易库文件路径（`trade_db_path()` 或测试临时库）
 */
[[nodiscard]] IngestDbAcquire acquire_ingest_db(const std::string& path);

/**
 * @brief 仅测试: 覆盖打开失败退避窗口 (0ms = 关闭退避); 生产代码不调用
 */
void set_ingest_failure_backoff_for_test(std::chrono::milliseconds backoff);

/**
 * @brief 仅测试: 立即结束当前失败退避窗口 (不改变退避时长); 生产代码不调用
 */
void expire_ingest_backoff_for_test();

/**
 * @brief 仅测试: 清空 ingest 缓存 (句柄/失败去重/退避/尝试计数) 并恢复默认退避
 */
void reset_ingest_cache_for_test();

/**
 * @brief 仅测试: 自上次 reset 起的真实打开尝试次数 (退避跳过的调用不计)
 */
[[nodiscard]] uint64_t ingest_open_attempts_for_test();

/**
 * @brief 账户锚点日（ADR 0046）
 *
 * 锚点日 = 该账户 `positions` 与 `trading_accounts` **两张快照表**的 `MAX(date)`；
 * **不含** `orders`/`trades` 追加流表（理由见 ADR 0046：追加流先于快照落库会让
 * 查询失败的当天锚点误推进到当日，从而把旧快照显示成空仓）。
 *
 * @return account_id -> "YYYYMMDD"; 无快照行的账户不在表中 (锚点日 = null)
 */
std::unordered_map<std::string, std::string> account_anchor_days(DzDatabase* db);

/**
 * @brief 按账户/合约/交易日查询委托或成交（同构: account_id + instrument_id + date）
 *
 * @param table "orders" / "trades"
 * @param account_id 空串 = 不限定账户
 * @param instrument_id 空串 = 不限定合约
 * @param date 具体交易日（距纪元天数）; `DZ_DATE_NA` = 账户锚点日
 *
 * `date = DZ_DATE_NA` 时：指定账户 → 该账户锚点日；不指定账户 → 每账户**各自**锚点日
 * （无快照行的账户不出行）。锚点日由 {@link account_anchor_days} 取得。
 *
 * 返回列 = `tdstore` schema v1 声明序中的**业务列 + `seq`**（不含平台内部列
 * `gateway` / `ext`，ADR 0044 决策 2；列序与列名单源 = `tdstore::schema_catalog.h`）。
 * 行序 = seq 升序。空串条件表示不限定。异常抛给调用方。
 */
DbQueryResult db_query_order_trade(DzDatabase* db,
                                   const std::string& account_id,
                                   const std::string& instrument_id,
                                   const std::string& table,
                                   DzDate date);

/**
 * @brief 按账户/合约/交易日查询持仓（`positions` 表）
 *
 * 列 = positions 表业务列 + `seq`（无 `gateway`/`ext`）；`date` 语义同
 * {@link db_query_order_trade}。行序 = seq 升序。
 */
DbQueryResult db_query_position(DzDatabase* db,
                                const std::string& account_id,
                                const std::string& instrument_id,
                                DzDate date);

/**
 * @brief 按账户/交易日查询资金（`trading_accounts` 表）
 *
 * 列 = trading_accounts 表业务列 + `seq`（无 `gateway`/`ext`）；`date` 语义同
 * {@link db_query_order_trade}。空串 account_id 表示不限定（全部账户各自的锚点日）。
 * 行序 = seq 升序。
 */
DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id, DzDate date);

/**
 * @brief 按账户 + seq 区间查询资源表 (回补用)
 * @param resource 资源路径 ("order"/"trade"/"position"/"trading_account")
 * @param from 区间下界 (含)
 * @param to 区间上界 (含)
 *
 * 行序 = seq 升序。未知资源抛异常。返回列 = 声明全列（内部解码按列名取用，
 * 不受 C API 业务列投影影响）。
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
std::unordered_map<std::string, uint64_t> db_query_max_seq_by_account(DzDatabase* db,
                                                                      const std::string& resource);

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
