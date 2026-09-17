# ADR 0015: DB 统一接口端口-适配器化（服务/传输后置）

## Status

Accepted（2026-09-17）

## Context

- 需求：事务域（instruments/orders/trades/positions/trading_accounts）与归档域（tick/bar，未实现）
  将来都要能按配置选择后端，SQL 与非 SQL 都支持。
- 硬约束：策略 SDK 保持单文件分发，MySQL/Mongo 等驱动二进制不得进入 SDK（SQLite 已在内）。
- 现状：`libs/db` 只有 SQL 级接口（`Database` exec/query/prepare + SQLiteCpp 适配）；
  `libs/tdstore` 是域 store；SDK 的 `dz_db_*` 是类型化查询面（内部 `db_generic_query`
  走"拼 JSON→解析→拼 SQL"自产自销，非公共 API）。
- ADR 0012 D7 曾计划 Boost.MySQL + OpenSSL 静态链接进 SDK——被单文件约束否决。
- 部署形态（独立服务进程 vs 进程内嵌）尚无结论，且不应影响调用方代码。

## Decision

1. **端口-适配器**：定义域级存储接口（`dztrader::db`：`Database`/`Session`/`Collection`/`Row`/
   `Filter`/`Aggregation`/`Transaction`/`Snapshot`/`Capability`/`Config`）。接口不出现 SQL、
   不区分 SQL/非 SQL、不区分服务/内嵌/后端类型；`Database::open(config, schemas)` 是唯一耦合点。
2. **接口形态**：通用 collection 名 + 位置化 `Row`（`Value` variant）；Session 只有 6 个虚函数
   （upsert/remove/find/aggregate/begin_transaction/begin_snapshot），Database 侧入口
   （open/session/capabilities/migrate）；类型化 store（tdstore）
   降为其上 facade；接口不提供序列化格式（公共 C API 无 JSON 入口，代理线格式随部署形态 ADR 再定）。
3. **能力准入**：`Capability` 位标志（Transactions/SnapshotRead/Upsert/OrderedRangeScan）；
   事务域要求全部满足，启动时校验，不满足的后端（如单机 Mongo）拒绝配置。
4. **部署形态后置**：现阶段各进程内嵌使用同一接口；将来策略侧若因单文件约束需要驱动隔离，
   以"代理适配器"实现同一接口，独立进程与传输机制另行 ADR 决定。本 ADR 不选传输
   （nng/ipc 方案已排除）。
5. **C++20 冻结**（不升 23）；错误沿用 `dztrader::Exception` + 新增 `DZ_EC_DB_*` 错误码族（-6001..）。
6. **取代（Supersedes）ADR 0012 的 D5/D7**：D7"Boost.MySQL 编进 SDK"作废，MySQL 改为 libs/db 的
   一个适配器；D5"DB 抽象启动（SQL 级最小形态）"升级为域级接口；其余条款（统一单库路径、
   并发模型、迁移并发安全、多账户写策略、SDK 路径）继续有效。

## Alternatives

- **保持 SQL 级接口**：Mongo/DolphinDB 等无法实现 exec/query/prepare，达不到"非 SQL"目标。**否决**。
- **vnpy 式 per-domain 强类型接口**：vnpy 只有 bar/tick 两域故便宜；本平台域多（5+，归档域未计），
  每加域/字段要改所有驱动。仅借其驱动边界（一后端一实现、自持 schema、按名选择）。**否决**。
- **策略直连各后端驱动（vnpy 的 pip 模型）**：驱动随 SDK 分发，违反单文件约束。**否决**。
- **全服务化统一 IPC**：引入新子系统与策略可用性依赖，且部署形态尚无结论。**后置**（决策 4）。
- **分型双通路（平台直连 + 策略经服务）**：服务被否后不成立，统一为"同一接口 + 部署形态后置"。**否决**。
- **升 C++23（std::expected 等）**：收益小（异常已是项目约定），成本高（编译器门槛、Conan 全量
  重编、CI 重验）。**否决**。

## Consequences

- `libs/db` 重构：SQL 级 `Database`/`SqliteDatabaseRef` 与 SQLiteCpp 直接暴露被删除
  （P0 阶段结束时收敛，分 P0a/P0b/P0c 三次提交落地；同一阶段内不留跨阶段双轨）；
  SQLiteCpp 收纳为 sqlite 驱动内部实现。例外：`apps/ctp/td/td_prescan.*` 的 raw SQLiteCpp
  只读路径 P0 暂不迁移（后续 ADR 处理）。
- `tdstore` 变 facade；td `PersistWriter` 持 `Session`；SDK 内部查询走同一接口
  （`dz_db_*` C 形状不变）。
- schema 职责拆分：域层保留当前 schema 声明（`ResourceSchema` 与 records），迁移脚本
  （td schema v1..v5）随 P0 迁入 sqlite 驱动。
- 新增 `DZ_EC_DB_*` 错误码（`libs/strategy_api/include/dztrader/error.h`）；删除 SDK 内部
  "拼 JSON→解析"中转（gap 回补改类型化 `Filter`），`db_database.cpp` 不再直接使用 nlohmann。
- MySQL 项目重定义为 libs/db 适配器，不进 SDK。
- 归档域（tick/bar）将来以新增 collection + facade 落地，接口与驱动不改。
- 能力矩阵细节、代理适配器形态属后续 spec/ADR。
- 硬约束推论：非 SQLite 后端对"策略 ingest"可用之前，必须先落地 SQLite 镜像或代理适配器；
  在此之前 MySQL/非 SQL 配置只对平台进程生效（P1/P2 准入条件）。

## References

- 设计：`docs/superpowers/specs/2026-09-17-database-abstraction-design.md`
- 修订：`docs/adr/0012-unified-td-db-and-abstraction.md`（D5 升级、D7 作废）
- 参考：vnpy `vnpy/trader/database.py` 与 `vnpy_{sqlite,mysql,mongodb}` 驱动
