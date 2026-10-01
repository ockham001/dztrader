# ADR 0016: 策略进程内嵌 SQL 驱动（SQLite + Boost.MySQL）

## Status

Accepted（2026-10-01）

## Context

- ADR 0015 定下端口-适配器接口（`db::Database`/`Session`/`Row`/`Filter`，`Database::open(config, schemas)`
  为唯一耦合点），但决策 4 把部署形态后置，Consequences 明确"MySQL/Mongo 等驱动二进制不得进入 SDK"；
  ADR 0012 D7 同结论（Boost.MySQL 编进 SDK 被"单文件分发"否决）。
- 用户 2026-10-01 决策：**SQLite 与 Boost.MySQL 直接以代码内嵌进策略接口进程**；Mongo、原版
  MySQL（libmysqlclient 系）本轮不处理；将来若能把驱动以**动态库 + 环境变量**注入策略进程，
  它们同样可以内嵌，否则退化为**独立服务进程**。
- 现状：`Database::open` 内是 `if (backend == "sqlite")` 硬编码分支，没有驱动注册缝；
  SDK 只读硬编码 `<DZTRADER_HOME>/db/td.db`；`libs/db` 无 MySQL 驱动。
- 仍然成立的前提：**SDK 单文件分发**（策略二进制自包含、无运行时依赖），故 Boost.MySQL 及其
  传递依赖必须静态链入；实测（spike）依赖闭包 = `Boost::charconv` + `OpenSSL` + `ZLIB`。

## Decision

1. **驱动注册缝**：`libs/db` 内新增 `internal::DriverRegistry`（backend 名 → 工厂函数）。
   `Database::open` 只做"懒注册 + 查表"，未注册抛 `DZ_EC_DB_UNSUPPORTED_BACKEND`；
   内置驱动由 `builtin_drivers` 显式注册（不用静态初始化对象，规避链接器丢弃与初始化顺序问题）。
2. **双驱动编入**：`sqlite`（既有 SQLiteCpp）与 `mysql`（Boost.MySQL 同步 API + `any_connection`）
   都编入 `dzdb` 静态库，随 SDK 单文件分发。CMake 选项 `DZ_WITH_MYSQL`（默认 `ON`）。
3. **后端选择单点**：新增公开头 `dztrader/db/config.h`，`db::resolve_config(domain)` 优先级
   **环境变量**（`DZTRADER_DB_BACKEND` / `DZTRADER_DB_OPT_<KEY>`）> `<DZTRADER_HOME>/configs/database.json`
   的 domain 段（缺失自动生成默认）> **内置默认**（sqlite + `paths::td_db()`）。
   公开面由 3 个头文件扩为 4 个。**读写两端必须同后端**：由同一份配置保证，混用不在支持范围。
4. **驱动来源与驱动实现解耦**：同一注册缝是将来"动态库插件"与"代理适配器"（策略侧实现
   `Database`，内部 IPC 到独立服务进程）的**内部插入点**；`db::Database` 接口不变。
   本轮**只落地进程内缝**：插件形态还需要 C ABI 层（`abi_version` / 结构体 `size` / 句柄
   `destroy`）与"插件加载失败"错误码，代理形态还需要传输协议，两者另立 ADR；在那之前
   注册缝只承载内置驱动（sqlite/mysql），不要把它当作已可用的插件 ABI。
5. **MySQL 语义对齐 SQLite**：String 列 `utf8mb4` / `utf8mb4_bin`（对齐 SQLite 的 BINARY 比较）；
   绑定参数按 schema 声明列类型归一化（对齐 SQLite TEXT affinity）；
   `upsert` 用 `INSERT ... ON DUPLICATE KEY UPDATE col=VALUES(col)`（MySQL 8 与 MariaDB 通吃）。
   `migrate()` 只做**当前 schema 的幂等 DDL + 版本表**（`CREATE TABLE IF NOT EXISTS`），
   历史 v1..v5 迁移不在 MySQL 侧重放；库内版本落后于代码时**显式报错**，不静默跑错 schema。
6. **SDK 接入**：新增 `DZ_API DzDatabase* dz_db_open_default(void)`（按配置解析打开，后端由配置决定）；
   `dz_db_open(path)` 语义不变（显式 SQLite 路径，向后兼容）；SDK 内部 ingest 路径
   （水位/回补）改走配置解析，SQLite 默认下与今天的 `paths::td_db()` 完全等价。

## Alternatives

- **维持 ADR 0015 原案（MySQL 只进平台进程，SDK 走 SQLite 镜像或代理）**：策略侧要多一条
  双写镜像/代理链路与一致性故障面；用户已直接选择内嵌。**否决（本轮）**。
- **首轮即插件化（一后端一 `.so`）**：驱动集合已确定（sqlite/mysql），插件化要引入 ABI/版本/
  加载失败的降级设计而本轮零收益。**后置**（缝已留，见决策 4）。
- **独立服务进程 + IPC（全后端统一）**：新子系统 + 策略可用性依赖；保留为"无法动态库注入"时的
  路径，本轮不实现。**后置**。
- **MySQL Connector/C++（libmysqlclient）**：重、ABI 敏感、许可复杂；Boost.MySQL 是 BSL-1.0、
  header-only、项目已用 Boost 1.90。**否决**。
- **升 C++23（std::expected 等）**：收益小、成本高（编译器门槛、Conan 全量重编）。**否决**。

## Consequences

- SDK 单文件新增**静态**依赖：`Boost::charconv`、`OpenSSL::SSL/Crypto`、`ZLIB::ZLIB`
  （spike 未 strip 约 8 MB）；`DZ_WITH_MYSQL=OFF` 时完全不引入。
- 策略进程连 MySQL 需要凭据：`configs/database.json`（`.gitignore` 覆盖 `configs/`）或环境变量；
  凭据不得入库。**注意（评审修正）**：master 会把父进程环境整体复制给每个子进程
  （`child_process.cpp`），故 `DZTRADER_DB_OPT_PASSWORD` 对本用户的所有子进程可见
  （`/proc/<pid>/environ`）；环境变量不是保密通道。推荐做法：凭据只给需要的进程、用最小权限
  数据库账号、跨主机一律 TLS；"子进程 env 白名单化"与"配置文件 0600"登记为后续项。
- **库身份不可自证（评审修正）**：MySQL 下"同一个库"从 `paths::td_db()` 单一文件变成
  host/port/database 字符串，读写两端可能"同后端不同库"而无人察觉。写端发布库身份戳
  （host/db/schema_version）+ 读端启动比对登记为后续项；本轮以"同一份 `database.json`/
  环境变量"为约定，并在 SDK 侧对"非 sqlite 后端"输出一次性醒目诊断。
- 平台侧（td 写端、dzweb 读端）本期仍默认 SQLite：`database.json` 默认值不变，故行为零变化；
  MySQL 写路径与 dzweb MySQL 读路径为后续阶段（见 spec §9）。
- `dz_db_query_bar`（归档域）与本轮无关，接口未动。
- 取代 ADR 0015 决策 4 中"驱动隔离"的取向与 ADR 0012 D7；ADR 0015 的接口三件套
  （决策 1/2/3/5）继续有效，其 Consequences 中"非 SQLite 后端启用前必须先落地 SQLite 镜像或
  代理适配器"这一准入条件，对 **MySQL** 由本 ADR 解除（改为驱动内嵌），对其他后端仍有效。

## References

- 设计：`docs/superpowers/specs/2026-10-01-embedded-sql-drivers-design.md`
- 修订：`docs/adr/0015-database-interface-port-adapter.md`（D4 取向被本 ADR 修订）、
  `docs/adr/0012-unified-td-db-and-abstraction.md`（D7）
- 契约：`docs/frame_contracts/instrument.md`（`dz_db_*` 查询语义）
