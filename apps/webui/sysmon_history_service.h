#ifndef DZTRADER_WEBUI_SYSMON_HISTORY_SERVICE_H_
#define DZTRADER_WEBUI_SYSMON_HISTORY_SERVICE_H_

/**
 * @file sysmon_history_service.h
 * @brief 系统监控历史查询（契约 sysmon §6.1）：HTTP 参数 → 只读历史库查询 → REST JSON。
 *
 * 分层（与仓库既有做法一致）：
 * - **查询语义**（指标键解析、层级选择、点数上限、缺口归并）在 `libs/sysmonstore` 的
 *   `history_query.h`——它们由 schema 形状决定，放这里会让 schema 知识出现第二份；
 * - **本类**只做三件事：把 HTTP query 参数解析成入参、打开只读连接、把行序列化成 JSON。
 *
 * 降级契约（§6.1.3）：库缺失 / schema 版本不符 / 文件损坏一律 `available=false` +
 * `reason`，**不抛异常、不返回 5xx、不返回伪造数据**。参数非法才是 400（由调用方判定）。
 *
 * 线程约定：每次调用独立打开只读连接（`HistoryReader` 非线程安全，而 REST 回调在同一
 * IO 线程串行，连接不跨请求复用），因此无共享状态、无锁。
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace dztrader::webui {

/// `/api/sysmon/history` 的已校验请求参数（解析与校验在 `SysmonHistoryService::parse`）。
struct SysmonHistoryQuery {
    std::string metric;      ///< 原始指标键（回显用）
    std::string dimension;   ///< 维度标识（标量指标为空串）
    int64_t from_ts = 0;     ///< 起始时刻（Unix 秒，含）
    int64_t to_ts = 0;       ///< 结束时刻（Unix 秒，不含）
    std::string level;       ///< `auto` / `raw` / `minute`（已校验）
    int64_t max_points = 0;  ///< 每序列点数上限（已钳制在合法区间内）
};

/// 参数解析结果：`error` 非空表示应回 400（契约 sysmon §6.1「参数非法」）。
struct SysmonHistoryParseResult {
    std::optional<SysmonHistoryQuery> query;
    std::string error;  ///< 为空表示成功
};

/**
 * @brief 历史查询与降级降级的唯一实现（契约 sysmon §6.1）。
 */
class SysmonHistoryService {
public:
    /**
     * @param db_path 历史库路径（`$DZTRADER_HOME/db/sysmon.db`）
     * @param busy_timeout_ms 只读连接等待超时（写入方提交时读取方等待而非失败）
     * @param now_ts 当前墙钟秒（注入便于测试默认时间范围）
     */
    SysmonHistoryService(std::filesystem::path db_path,
                         int busy_timeout_ms = 2000,
                         int64_t now_ts = 0);

    /**
     * @brief 解析并校验 HTTP query 参数（契约 sysmon §6.1）。
     *
     * @param params 原始 query 参数（drogon 的 `getParameters()`）
     * @return 成功时 `query` 有值；失败时 `error` 非空（调用方回 400 `bad request`）
     */
    [[nodiscard]] SysmonHistoryParseResult parse(
        const std::function<std::string(std::string_view)>& param) const;

    /**
     * @brief 执行查询并序列化成 REST 响应 JSON（契约 sysmon §6.1）。
     *
     * @param query 已校验的参数
     * @return 响应体；降级时为 `{"available":false,"reason":...}`，绝不抛异常
     */
    [[nodiscard]] nlohmann::json query(const SysmonHistoryQuery& query) const;

private:
    std::filesystem::path db_path_;
    int busy_timeout_ms_ = 2000;
    int64_t now_ts_ = 0;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSMON_HISTORY_SERVICE_H_
