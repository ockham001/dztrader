#ifndef DZTRADER_WEBUI_TD_STATS_QUERY_H_
#define DZTRADER_WEBUI_TD_STATS_QUERY_H_

/**
 * @file td_stats_query.h
 * @brief 统计计数器重算查询（契约 rest §2.9.5 / §2.9.8 / td-data-sync §5）。
 *
 * 与 `TdOrdersQuery` 同型：每请求独立只读连接（`TdReadConnection`）、`busy_timeout`
 * 200ms，同步查询、无共享状态。**单只读事务**（`BEGIN DEFERRED`）内一次取齐三样：
 * 聚合 4 值 + 活动（未成交）委托种子 + 每账户四表 `MAX(seq)` 水位——三者必须同一快照，
 * 否则会出现「快照没有却被水位判定已含」的静默误吞（契约 td-data-sync §5）。
 *
 * 表名/列名以 `libs/tdstore/src/schema_catalog.cpp` 为唯一真相源；schema 版本校验复用
 * `kTdOrdersReadSchemaVersion`（同一 `td_read_db` 实现）。
 */

#include "td_read_db.h"
#include "td_stats.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace dztrader::webui {

/// 统计重算查询（`GET /api/td/stats` 的计数器数据源）。
class TdStatsQuery {
public:
    /// @param trade_db_path 交易库路径提供者（空串 = 库不可用）
    /// @param busy_timeout_ms 只读连接等锁上限（§2.9.8：200ms）
    explicit TdStatsQuery(std::function<std::string()> trade_db_path,
                          int busy_timeout_ms = kTdReadBusyTimeoutMs);

    /// 从交易库重算（**单只读事务**内取聚合 + 活动委托 + 每账户 MAX(seq)，三者同一快照）。
    /// @param online_accounts 在线（Ready）账户集合（空 = 直接返回全 0 快照，不读库）
    /// @param trading_day 当前交易日 "YYYYMMDD"（nullopt = 无当前交易日 → 全 0 快照）
    /// @return nullopt = 库不可读 / 版本不符 / 查询失败（调用方保持旧计数器）
    std::optional<TdStatsSnapshot> recompute(const std::vector<std::string>& online_accounts,
                                             const std::optional<std::string>& trading_day) const;

private:
    /// 单只读事务内的聚合 4 值（契约 rest §2.9.5 口径）。
    bool query_values(sqlite3* db,
                      const std::string& trading_day,
                      const std::vector<std::string>& accounts,
                      TdStatsValues& values) const;

    /// 单只读事务内的活动（未成交）委托种子。
    bool query_active(sqlite3* db,
                      const std::string& trading_day,
                      const std::vector<std::string>& accounts,
                      std::vector<TdStatsActiveOrder>& active) const;

    /// 单只读事务内的每账户水位：四表 `MAX(seq)` 逐账户取最大（不带 date 过滤）。
    bool query_watermarks(sqlite3* db,
                          const std::vector<std::string>& accounts,
                          std::map<std::string, uint64_t>& watermarks) const;

    std::function<std::string()> trade_db_path_;
    int busy_timeout_ms_ = kTdReadBusyTimeoutMs;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_STATS_QUERY_H_
