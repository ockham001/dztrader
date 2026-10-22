# ADR 0018: 策略侧 K 线生成与交易小节数据模型

## Status

Accepted（2026-10-18）。决策 1 已由 [ADR 0031](0031-tick-bar-synthesis-component.md) 取代（2026-10-22）；决策 2/3/4/5 继续有效。

## Context

- 策略进程只收到 tick（`dz_next_md`），K 线由各策略自行计算（`architecture.md`「K线各策略自行计算」）。
  若由数据存储进程聚合 K 线再写 SHM 推给策略，tick→处理进程→bar 帧→策略要两次写帧+唤醒，
  即使都在微秒级也凭空增加策略等待；而单根 K 线聚合本身只是纳秒~微秒级计算。用户明确否决该路径。
- 旧实盘版本（2023-08 前）的 K 线生成器依赖每合约元数据 `tradeSession`（连续交易小节）+
  `callAuction1/2`（集合竞价时刻），用字符串时间预生成"分钟→各周期 bar id"映射。
  新项目的 `instruments` 表（ADR 0011）不含交易小节字段，CTP 也不提供；这些数据只能平台维护。
- 新项目 `DzTick.date` 已是 CTP 交易日（夜盘 tick 归属次日）、`time` 是距午夜整数秒——
  整数时间前提已具备，不需要旧版的字符串时间方案。
- 历史 K 线归档域（tick/bar 存储与查询）尚未实现（ADR 0015），`dz_db_query_bar` 目前是 stub。

## Decision

1. **K 线不在引擎/帧层生成，不做 SHM bar 推送，也不提供统一 C API 计算。** 由策略进程内
   SDK header-only 单周期模板生成（C++ only；C 策略自行聚合）：
   - 周期先支持 1/3/5/15 分钟、1 小时、1 日；其余后续扩充（周期以秒数传入，不加新 API）。
   - 多周期 = 策略实例化多个生成器，或自行用短周期合成；模板不提供"一个结构同时出多周期"。
   - `DzBar` 内含 `instrument_id/period_sec/trading_day/begin_time/OHLC/volume/turnover/open_interest`；
     回调形态（open/update/close 三粒度、update 可编译期探测跳过）在实现时定，不改 `StrategyBase`。
2. **交易小节是平台维护元数据，放独立 `platform.db`**（dzweb 写 / SDK 与归档只读 / 用户可直接改库；
   `td.db`、`webui.db` 都不动）。**大类优先**：品种行覆盖交易所级默认行，不按合约维护。
3. **结构参考掘金 `TradingSession` 的"连续交易小节 + 集合竞价窗口"二分法，但用本项目风格**：
   整数秒（`begin_sec`/`end_sec`，夜盘允许跨午夜）、`seq` 交易日顺序、集合竞价显式
   `kind`（开盘/收盘）；支持一个交易日多次集合竞价。策略经新 C API 按 `instrument_id` 查询
   （SDK 内部解析 `exchange_id + product_code`），返回定长 POD 结构而非结果集。
4. K 线合并语义（开盘竞价并入其后小节首根 bar、收盘竞价并入其前末根 bar、小节结束时刻 tick 归前一根、
   1 小时/1 日边界对齐）由模板实现并以文档+测试锁定，不进帧契约。
5. 历史 K 线归档域另行 spec（含 schema、生产者、查询）；本版本不实现，
   `dz_db_query_bar` 保留但返回 `DZ_EC_NOT_IMPLEMENTED`，不做"看起来能用"的假接口。

## Alternatives

- **数据存储进程聚合后写 bar 帧推策略**：两次 SHM 写+唤醒，策略多等一个聚合+IPC 周期；K 线语义还会
  进帧契约。**否决**（用户性能论证）。
- **引擎内建 K 线生成（`StrategyBase` 直接有 `on_bar_*`）**：引擎从"帧分发器"变成市场语义组件，
  需新增周期配置 API、依赖交易小节库；周期/合并规则的选择应属策略。**否决（本版本）**；
  将来若有强烈需求，可在不改引擎的前提下用模板适配。
- **统一 C API 计算 K 线**：无法覆盖策略级定制（周期、合并规则、异形 K 线），且把 bar 语义固化进 ABI。
  **否决**。
- **每合约维护交易小节**：新品种上市要逐合约维护，手工成本高且易漏。**否决**。
- **照抄掘金结构的字符串时间**：本项目 tick 已是整数时间，字符串解析与跨平台格式无收益。**否决**。

## Consequences

- 本版本只落 ADR + GitHub Issue（设计定稿），实现（`platform.db` 迁移、交易小节 C API、模板、WebUI 维护页）
  等真实使用需求触发。
- 归档域与 `dz_db_query_bar` 不在本版本；策略预热用历史 K 线需等归档域 spec。
- 策略侧 K 线口径（交易小节/集合竞价）有唯一真相源（platform.db），模板与未来归档生产者共用。
