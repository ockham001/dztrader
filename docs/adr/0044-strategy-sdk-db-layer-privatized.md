# ADR 0044: 策略 SDK 数据库层私有化与交易查询参数调整

## Status

Accepted（2026-11-03）

## Context

- [ADR 0016](0016-strategy-sdk-embedded-sql-drivers.md) / [ADR 0019](0019-strategy-sdk-db-backend-scope.md) 把 SQLite + MySQL 作为 SDK 内嵌 DB 能力，`libs/db` 是平台公共端口；[ADR 0041](0041-sqlite-only-trade-store-schema-v1.md) 已把平台侧交易数据收敛为本地 SQLite 直连。
- 行情数据（合约 / bar / tick）将来仍可能需要 sqlite / mysql 等多后端组合，本轮不动其设计。
- 交易查询 API 需要按交易日过滤：多日表后原有的"全量 / 单行"语义不再成立。

## Decision

1. **`libs/db` 私有化**：移入 `libs/strategy_api/src/db/`（端口、驱动、SQLite 实现随迁），平台进程不再依赖；MySQL 能力保留在 SDK 内，本轮不删除。
2. **公开 API 名称与用法不变**（`dz_db_open_default` 默认库语义收敛见决策 3）：`dz_db_query_order` / `dz_db_query_trade` / `dz_db_query_position` / `dz_db_query_trading_account` 增加 `DzDate date` 参数；`DZ_DATE_NA` 表示账户锚点日（指定账户时取该账户四表 `MAX(date)`；`account_id` 为空时按每账户各自锚点日过滤），只返回该交易日行；返回业务列 + `seq`，不返回 `gateway` / `ext`。
3. **交易库固定本地 SQLite**（`trade.db`）；`dz_db_open_default()` 收敛为"固定打开本地 `trade.db`、首次调用懒打开并缓存进程级句柄"（SDK 初始化不打开任何 DB；`dz_db_open(path)` 保留显式路径语义；ingest 侧维持懒打开 + 失败退避）。`database.json` 不再承担"交易数据选后端"，仅保留 SDK 内部配置用途；archive / 行情库的默认库入口待归档域设计时另定。
4. 查询列与 `tdstore` schema 单源保持同步（schema v1 后不再有 `id` / `trade_date` / `commission` 等列）。

**为什么不直接删除 `libs/db`**：SDK 行情侧多后端尚未定案；私有化即可让平台保持纯 SQLite，又不预判行情侧结论。

## Consequences

- SDK 内部机械迁移；SDK 测试与构建调整；[ADR 0016](0016-strategy-sdk-embedded-sql-drivers.md) / [ADR 0019](0019-strategy-sdk-db-backend-scope.md) 的相关部分被取代。
- SDK 文档与冻结查询列清单需更新；`database.json` 语义收窄。
- 已知限制：MySQL 侧无交易数据使用场景，仅为行情域预留。
