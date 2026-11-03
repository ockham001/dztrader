# ADR 0041: 交易数据本地 SQLite 单库（schema v1）

## Status

Accepted（2026-11-03，交易数据架构调整）

## Context

- 现有 td 数据写入 `$DZTRADER_HOME/db/td.db`，经 `libs/db` 端口（[ADR 0012](0012-unified-td-db-and-abstraction.md) 统一单库、[ADR 0015](0015-database-interface-port-adapter.md) 端口-适配器）与 MySQL 路线（[ADR 0016](0016-strategy-sdk-embedded-sql-drivers.md) / [ADR 0019](0019-strategy-sdk-db-backend-scope.md)），schema 已迭代到 v5 迁移链。
- 需求已澄清：委托 / 成交 / 资金 / 持仓等纯账户交易数据只用**本地 SQLite**（不为交易数据引入 MySQL）；合约信息属行情域、本轮不迁；量级 1 万成交 / 账户 / 日、常驻 1–3 账户、压力按 20 账户；历史永久保留且按交易日查询；备份 / 归档按 SQLite 单文件方式（快照 + 应用级增量，已单独验证可行）。
- 现有实现的问题：时间列用 epoch 秒拼接（`update_time` 以交易日拼日期，夜盘排序错位）；合约 / 费率删除后的迁移链已无兼容价值；`positions` / `trading_accounts` 是"当前态一行"，无法按交易日回看；柜台差异靠逐列扩展，无法容纳多柜台；帧字段 `date` 与 DB 列 `trading_day` 命名不一致，跨层排查成本高。

## Decision

1. **交易库**：`$DZTRADER_HOME/db/trade.db`（原 `td.db` 改名），只存账户交易数据 `orders` / `trades` / `positions` / `trading_accounts`；`instruments` 暂留，行情域落地后迁 `market.db`。数据库文件命名统一为：`trade.db`、`market.db`（预留）、`platform.db`（原 `webui.db`）、`sysmon.db`（不变）、`cache/children.db`（不变）。
2. **四表统一带 `date`、跨交易日累积**：业务唯一键分别为 `orders (account_id, date, order_id)`、`trades (account_id, date, exchange_id, trade_id)`、`positions (account_id, date, instrument_id, direction)`、`trading_accounts PK (account_id, date)`，`instruments` 主键 `(instrument_id)`；"当前"只是最新交易日的筛选。`orders` 键带 `date` 做容错：`order_id` 仍全局唯一，但计数器异常 / 跨机导入时跨天同号不会互相覆盖；`trades` 键带 `exchange_id`：`trade_id` 只保证交易所内唯一，多交易所账户跨所同号不会互相覆盖。`gateway` 不进唯一键（账户身份与生命周期规则见 [ADR 0045](0045-trading-account-identity-lifecycle-and-accuracy.md)）。持仓 / 资金按"账户锚点日"（该账户四表 `MAX(date)`）过滤；当日行重建、零行与收敛规则见 ADR 0045。
3. **表列 = 帧业务字段 + `seq` + `gateway`(TEXT) + `ext`(JSON)**：`gateway` = 写入来源的 td 实例名（与 `ACCOUNT_STATUS.gateway_name` 同义），仅审计 / 定位用，不进键、不参与查询过滤；柜台差异（CTP 的 front_id / session_id / order_ref / order_sys_id / cancel_time / error_* 等）全部放进 `ext`，不再为单个柜台加列；每个柜台在自己的文档里定义 ext 键。`orders` 额外带**派生列 `avg_price`**（成交均价）：柜台不直接提供（CTP 订单回报无均价字段），由 td 从成交回报实时累计 `Σ价×量 / Σ量`，帧 `DzOrderReport` 带同名字段；无成交时为空。每笔成交到达后若均价变化，补一次订单记录更新（新 `seq`）并推帧，不依赖柜台回调顺序；重启后由登录日装载的当日成交按 `order_id` 重建。成交明细仍全量落 `trades`（均价重建、重放去重、seq、对账与成交页都用它）。外部单 / 手工单 / 同步单用 `DzStrategyId` 保留值（`/external` `/manual` `/sync`）表达，`orders.strategy_id` 原样保存，不设 `is_external` 列。DB / 帧保留 double **全精度**、不做显示舍入（`avg_price` 可随时由 `trades` 重算）；显示精度是视图决策，由 UI 按合约 `price_tick` 格式化（[ADR 0043](0043-dzweb-direct-db-query-and-ws-push.md)）。
4. **时间与字段命名**：统一为「`date`（交易日 = 登录会话交易日）+ 交易所真实时钟秒（0..86399）」；数据契约字段名统一 `date`（帧结构体 / DB 列 / REST·WS 键 / SDK 参数同名，注释与契约写明"交易日"）。表示层固定：帧 `DzDate`（距纪元天数）、DB TEXT `YYYYMMDD`、JSON string `YYYYMMDD`；`DzDate` / `DZ_DATE_NA` 类型与宏不变，`Bar.trading_day` 一并改 `date`，`DzTick.date` 已是该命名。删除 epoch 时间列与 `trade_date`；18:00 锚点只出现在排序 SQL 表达式里，不改写真实时间。`md-status` 的 JSON 键 `trading_day` 一并改 `date`（涉及 `apps/ctp/md/`，只改 payload 的 JSON 键，已确认）。
5. **平台侧删除 `libs/db` 依赖**：td 写端、tdstore 直连 SQLiteCpp，dzweb 沿用现有 sqlite3 C API 只读直连；schema v1 不再保留迁移链，`PRAGMA user_version` 不符即旧库改名重建（开发期无兼容义务）；建库 / 重建由 master 负责（子进程等待重试或降级）。
6. **`order_id` 是唯一身份**：所有进程统一使用共享 shm 计数器（`CHANNEL_NAME_ORDER_ID`），删除 td 私有计数器；master 启动时按库内 `MAX(order_id)`（走索引）播种，td 侧 `ensure_at_least` 保留为幂等兜底。外部标识（CTP 的 `order_ref` / FrontID / SessionID / OrderSysID）只作为 `ext` 中的**引用**：用于登录重放匹配、撤单定位、排查 / 对账；不建唯一约束、不参与 upsert / 去重键，任何路径不得用它反改 `order_id`；匹配不到 → 按外部单处理并分配新 `order_id`（ADR 0045）。
7. `seq` 保留：账户级、四类共享、跨日累积；同时作为增量备份水位。

**为什么不用另一条路（保留 dzdb 后端抽象或引入 MySQL）**：交易数据纯本地、无远程消费方；抽象只增加迁移、驱动与故障面，而备份 / 归档 / 查询全部是 SQLite 单文件模式。保留它没有收益，且会阻止"直连简单化"。

## Consequences

- 正面：schema 与查询按交易日建模；柜台差异不再扩散到列与迁移；平台侧只剩一种数据库实现；命名统一（`date`）后帧 / DB / REST·WS / SDK 不再各叫各的；备份 / 归档路线与 SQLite 单文件一致。
- 负面 / 已知限制：旧 dev 库直接丢弃；`instruments` 多柜台同名冲突本轮不处理（随行情域迁移解决）；DB 故障期间进程退出会丢内存积压（见 [ADR 0042](0042-persist-failure-backpressure-and-retry.md)）；持仓 / 资金精度与收敛窗见 ADR 0045。
- 文档 / 契约：`td-data-sync`（登录日装载、重建、seq、绝对态表述、`ORDER_REPORT` 新增 `avg_price`、成交去重键含 `exchange_id`）、`account-status`（`date`）、`td-order`（外部单保留策略标识）、`instrument`（路径与暂留）、`rest` / `webui-ws`（`date`、降级语义、委托 + 成交两 tab 的字段）等同步更新；新增 `td-config` / `td-status` 契约（[ADR 0043](0043-dzweb-direct-db-query-and-ws-push.md)）。
- Supersedes：[ADR 0012](0012-unified-td-db-and-abstraction.md) / [ADR 0015](0015-database-interface-port-adapter.md) / [ADR 0016](0016-strategy-sdk-embedded-sql-drivers.md) / [ADR 0019](0019-strategy-sdk-db-backend-scope.md) 中与"交易数据后端可切换、MySQL 路线"相关的部分；其余结论（统一单库、tdstore 为 schema 真相源）保留。

## References

- 账户身份 / 生命周期 / 准确性边界：[ADR 0045](0045-trading-account-identity-lifecycle-and-accuracy.md)；读路径：[ADR 0043](0043-dzweb-direct-db-query-and-ws-push.md)；SDK 侧：[ADR 0044](0044-strategy-sdk-db-layer-privatized.md)；持久化失败策略：[ADR 0042](0042-persist-failure-backpressure-and-retry.md)。
