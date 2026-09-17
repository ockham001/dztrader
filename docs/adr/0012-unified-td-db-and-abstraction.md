# ADR 0012: td 数据统一单库 + DB 抽象启动

## Status

Accepted（2026-09-15）

## Context

td 持久化现状：

- 每个 td 进程一个 SQLite 文件：`$DZTRADER_HOME/flow/<实例名>/<实例名>.db`（`td_api.cpp` writer + prescan）；
- 策略 SDK 直接只读该文件（ingest 断档回补），且**路径硬编码** `dztd_ctp`（`api.cpp`）；
- dzweb 后端按 td 进程名拼路径只读；
- `libs/db` 只有 SQLite 实现（`Connection` 直接包 `SQLite::Database`），无后端抽象；
- 事故场景：多 td 网关（未来 CTP/IB/加密并存）时，合约等无账户维度的数据没有"该读哪个库"的答案。

同时决定为合约信息引入 DB 查询 API（ADR 0011），其查询不带 account_id，无法按库路由。

## Decision

1. **统一单库**：所有 td 网关共用一个 SQLite 文件 `paths::td_db()` = `<DZTRADER_HOME>/db/td.db`；
   `flow/<实例名>/` 仅保留场所 API 流文件。
2. **并发模型**：WAL + `synchronous=FULL` + `busy_timeout=5000`；多进程写串行化，读不阻塞写。
   WAL 转换需独占锁，有界重试后仍失败则**降级为当前 journal 模式并告警**继续运行（不阻塞启动）。
3. **迁移并发安全**：`MigrationManager::apply` 改为 `BEGIN IMMEDIATE` 单事务（版本复查 + 迁移 + 提交），
   多 td 进程并发首开安全（SQLiteCpp `TransactionBehavior::IMMEDIATE`）。
4. **多账户写策略**：合约信息为交易所级数据，单表按 `instrument_id`；会话各自 `INSERT OR REPLACE` 幂等，
   `updated_at` 每次推进；不按账户分副本、不做"挑一个账户"选举。
   新增不变量：**`account_id` 跨 td 网关全局唯一**，由 master 配置校验。
5. **DB 抽象启动**（本轮最小形态）：（Superseded by ADR 0015：D5 由 ADR 0015 升级为域级端口-适配器接口）
   - `libs/db` 新增后端接口 `Database`/`Statement`/`Transaction`/`QueryResult`/`BindValue` + SQLite 适配器
     （含包装既有连接的 `SqliteDatabaseRef`，供 PersistWriter 过渡期共用）；
   - 新库 `libs/tdstore` 承载 td 规范记录（`InstrumentRecord`）、td schema/迁移与 store 操作；instruments 为首个用例；
   - orders/trades/positions/rates 本轮继续走 PersistWriter 原生 SQLiteCpp，随 MySQL 项目迁移。
6. **SDK 路径**：`open_td_db` 改 `paths::td_db()`，删除硬编码网关名；不再需要注入 td 实例名。
7. **MySQL 路线（后续独立实施）**：Boost.MySQL（BSL-1.0、header-only、协议级实现、不依赖 libmysqlclient、
   项目已用 Boost 1.90）+ OpenSSL 内部静态链接，编译进策略 SDK 单一动态库；`DZ_WITH_MYSQL` 编译开关。
   Mongo 无 Boost 等价物（mongocxx 重且 ABI 敏感），缓。（Superseded by ADR 0015：D7 由 ADR 0015 作废）

## Alternatives

- **保持每进程一库 + master 注入 td 实例名**：策略能路由，但合约等全局数据被切碎，dzweb/多账户查询需逐个库拼；
  且"查询不带账户"的接口在多库下语义依然模糊。**否决**。
- **DB 服务进程 + IPC（后端完全可配，含 Mongo）**：形态最干净，但引入新子系统（协议/迁移/运维），
  本轮合约改造无法承载。**缓**（Mongo 或远程访问需求出现时再立项）。
- **策略直连各后端驱动（类 vnpy）**：Python 单进程 pip 模型可行；本平台策略为独立进程 + 用户自编译 + 静态链接 SDK，
  驱动会传递进每个策略二进制并引入 ABI/凭据/连接管理成本。Boost.MySQL 编译进 SDK 动态库即可规避，无需此路。**否决**。
- **只抽象写端**：策略读端仍锁定 SQLite，后端实际换不了，抽象空转。**否决**。
- **内容相同则跳过写入**：会冻结 `updated_at`（刷新完成的观测点；刷新已随 ADR 0014 删除，该观测点现为登录/重连全量 upsert 的观测点），且当前量级（万级行/登录）批量事务毫秒级，无必要。**否决**。

## Consequences

- 数据文件路径变更：`flow/<实例名>/<实例名>.db` → `db/td.db`；旧 dev 库废弃（文档给清理说明）。
- 所有消费端改路径：td_api（writer/prescan）、策略 SDK、dzweb td_data_service、SDK 集成测试。
- 新增 master 配置校验：`account_id` 重复即配置错误。
- 过渡期同一进程内可能同时存在原生 SQLiteCpp 与 `db::Database` 两条连接（合约走接口，其余走原生）；
  MySQL 项目完成全部迁移后收敛为一条。
- tdstore 成为 td schema/记录的单一真相源，apps/ctp/td 不再拥有 schema。
- **未落实（已登记）**：`account_id` 跨 td 网关的全局唯一校验待 master 侧实现（本次仅文档登记
  Decision 4 的不变量，td 侧无校验；重复 account_id 的启动拦截缺失）。

## References

- 设计：`docs/superpowers/specs/2026-09-15-instrument-query-unified-td-db-design.md`
- 合约查询决策：`docs/adr/0011-instrument-info-query-model.md`
- 路径布局：`libs/core/include/dztrader/core/path.h`
