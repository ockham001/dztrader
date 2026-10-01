/**
 * @file config.h
 * @brief 后端配置解析单点：环境变量 > configs/database.json 的 domain 段 > 内置默认
 *
 * ADR 0016 决策 3：`db::resolve_config(domain)` 是"选哪个库"的唯一入口，
 * SDK（dz_db_open_default / ingest 水位回补）与平台进程共用同一份解析规则。
 *
 * 优先级（高 → 低）：
 *   1. 环境变量：`DZTRADER_DB_BACKEND`（backend）、`DZTRADER_DB_OPT_<KEY>`（options）；
 *      `<KEY>` 允许 `[A-Z0-9_]`，映射为小写键（`DZTRADER_DB_OPT_BUSY_TIMEOUT_MS` ->
 *      `busy_timeout_ms`）；未知/非法键忽略（向前兼容新驱动选项）；空值视为未设置。
 *   2. `<DZTRADER_HOME>/configs/database.json` 的 domain 段；文件或段缺失时按内置默认
 *      自动生成（AGENTS.md 硬门禁 6：不提交模板）。段内 `backend` 为字符串，`options`
 *      为对象：值可以是字符串（原样）或数字/布尔（转字符串，如 `2048` -> `"2048"`，
 *      便于手写 `"port": 3306`）；数组/对象/null 视为配置错误。
 *   3. 内置默认：backend = `sqlite`；sqlite 未显式给 `path` 时解析为 `paths::td_db()`
 *      （= `<DZTRADER_HOME>/db/td.db`），其余键缺省由驱动内部默认
 *      （`busy_timeout_ms=5000` 等）；mysql 默认 `host=127.0.0.1`、`port=3306`、
 *      `user=`、`password=`、`database=dztrader`、`connect_timeout_ms=5000`。
 *
 * 不变量（读写两端必须同后端）：同一部署内 td 写端与策略读端必须指向同一后端与同一
 * 数据库。写 A 读 B 会使水位/回补链路静默失效（`seq` 不连续、快照不含已判定的行），
 * 混用后端不在支持范围。
 */
#ifndef DZTRADER_DB_CONFIG_H_
#define DZTRADER_DB_CONFIG_H_

#include <string_view>

#include <dztrader/db/database.h>

namespace dztrader::db {

/// 默认域：事务域（td）。读写两端共用该域，保证读到同一后端。
inline constexpr std::string_view kDefaultDomain = "td";

/**
 * @brief 解析某域（domain）的后端配置
 *
 * 该函数是后端选择的单点真相源；调用方不得自行拼装 `Config` 后再绕过本函数选库
 * （显式路径的 `dz_db_open(path)` 除外，其语义固定为 SQLite）。
 *
 * 副作用：`<DZTRADER_HOME>/configs/database.json` 缺失或 domain 段缺失时，用
 * `core::save_json_section` 原子写生成该段的默认配置（已存在的内容保留）。
 *
 * @param domain 配置文件的顶层段名；缺省 `"td"`（见 kDefaultDomain）
 * @return 合并后的配置；`options` 为字符串键值对
 * @throws Exception(DZ_EC_INVALID_PARAM) JSON 解析失败或结构非法（不静默回落默认——
 *         静默回落会连错库）
 * @throws Exception(DZ_EC_SYSTEM) 配置文件不可读或默认段写入失败
 */
[[nodiscard]] Config resolve_config(std::string_view domain = kDefaultDomain);

/**
 * @brief 后端驱动是否已可用（不打开连接）
 *
 * 查询驱动注册缝；`"sqlite"` 恒为 true，`"mysql"` 取决于 `DZ_WITH_MYSQL` 编译开关。
 * 供诊断与"配置指定了未编入的后端"时提前判错使用。
 *
 * @param backend backend 名（`"sqlite"` / `"mysql"`）
 * @return true 已注册；false 未注册或注册表初始化失败（永不抛出）
 */
[[nodiscard]] bool backend_available(std::string_view backend) noexcept;

}  // namespace dztrader::db

#endif  // DZTRADER_DB_CONFIG_H_
