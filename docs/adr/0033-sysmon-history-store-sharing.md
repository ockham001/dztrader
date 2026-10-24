# ADR 0033: 历史库 schema 与行编解码归属共享模块 `libs/sysmonstore`

## Status

Accepted（2026-10-24）

## Context

`dzsysmon` 把实时指标落成本地历史库（#78），`dzweb` 以只读方式直接打开同一个库提供历史查询（#79，用户已定方案：历史查询不走帧）。因此**两个进程共享同一个 SQLite 文件与同一套表结构**——写入方是唯一写者、读取方只读。

这带来一个必须当场定下归属的问题：schema 与行编解码放在哪里？

约束与事实：

- 本仓已有「共享存储层供多进程读写」的先例（`libs/tdstore`：td 记录与 schema 声明由写端 td 网关与读端共用），但那是**经由 `dzdb` 端口**（`Database`/`Session`/`Row`）的通用抽象，支持 sqlite/mysql 切换。
- 历史库需要的能力端口不暴露：`WITHOUT ROWID` 复合主键、`DELETE ... LIMIT` 分段删除、跨维护周期的聚合合并、WAL checkpoint 控制。若强行走 `dzdb`，要为 sqlite 专有能力在通用端口上加一组只有单后端能实现的声明——比直接用驱动更糟。
- 历史库只此一种后端（本地单文件），不存在 td 库那种「策略进程可切 MySQL」的需求。
- **唯一一份定义**是硬要求：schema 出现第二份定义（例如 dzweb 里另写一份 `CREATE TABLE` 或列清单），两进程就必然漂移，而漂移的后果是读出错位数据——比「历史不可用」危险得多。

## Decision

新建共享静态库 **`libs/sysmonstore`**，承载历史库的**唯一** schema 定义、行编解码、写入方门面、只读读取方门面与落盘线程。`dzsysmon`（写）与 `dzweb`（读，#79）都链接它。

具体取舍：

1. **schema 由一张指标注册表机械生成**（`kScalarMetrics[]` / `kDimensionDomains[]` → `build_schema_ddl()`），DDL、INSERT/SELECT 列清单、行编解码三处不会漂移；新增指标只改注册表。schema 版本号 `kSchemaVersion` 只增不改。
2. **直接用 SQLiteCpp，不经 `dzdb` 端口**。理由见 Context：历史库要的能力端口不提供，且不需要后端切换。公开头不暴露 SQLiteCpp 类型（pimpl），消费方不被动继承 SQLite 编译选项。
3. **宽表 + 长表并存**：固定标量进宽表（一行一个桶），按维度展开的指标（CPU 核 / 卷 / 盘 / 网卡 / 温度传感器）进长表。理由：维度元数随机器与热插拔变化，固定列会把表结构钉死在某一台机器上；而 CPU 核序号不保证连续，维度标识必须是不透明文本。
4. **表名不放进帧契约**。《帧契约：系统监控》负责快照 payload 的形状；历史库的表结构与列语义由本模块承载（`schema.h` / `records.h` 是唯一真相源）。两者只在「快照里的指标被记录成什么」这一层对应。

## Alternatives

- **把 schema 放进 `apps/sysmon`，dzweb 依赖它**：否决。dzweb 依赖一个 app 目录会颠倒依赖方向（apps 之间不互相依赖），且写入方的进程实现细节（配置、帧）会被读取方继承。
- **放进 `libs/db`（`dzdb`）**：否决。`dzdb` 是 sqlite/mysql 可切换的通用端口，历史库只需要 SQLite；为保留/压缩所需的能力在通用端口上加声明，会让「能力位」变成说谎（声明了却只有单后端能实现）。
- **两个进程各自实现一份 schema/编解码**：否决，违反「接口/协议只有一个真相源」（AGENTS §三）。漂移的后果是读出错位数据。
- **历史查询走帧（dzweb 发查询帧给 dzsysmon）**：否决（用户在 #41 设计定稿时已定直读）。会增加一份帧契约、把 dzweb 的查询延迟绑到 dzsysmon 的响应上，且 dzsysmon 崩溃时历史完全不可查——而历史恰恰是崩溃排查工具。

## Consequences

- **Positive**：schema 单一定义，两进程不可能漂移；指标扩展只改注册表一处；dzweb 无需感知 SQLite 之外的东西；历史库与实时链路解耦（dzsysmon 崩溃不影响历史可查）。
- **Negative / Known residual**：
  - 引入第二个「共享存储层」库（`libs/sysmonstore` 与 `libs/tdstore` 并存）。两者抽象层级不同（前者直连驱动、后者经端口），**这是有意差异**：历史库不需要后端切换，td 库需要。新增第三个存储层前先评估能否复用这两者之一。
  - 库文件由两个进程以不同模式打开（写/只读），WAL 与 `busy_timeout` 是唯一并发保障；写入方的长事务会挡住读取方（已用 `maintenance_batch` 分段缓解，但队列满时仍可能出现短暂的读等待）。
  - schema 版本升级必须是「新版本写、旧读取方降级」路径，不能原地改列（见 `SchemaMismatch` 处理）；跨版本迁移工具本次未做（版本 1 是首个版本）。

## References

- 讨论日期：2026-10-24（#78 实施）
- 源码位置：[libs/sysmonstore](../../libs/sysmonstore/)（`schema.h` 注册表 / `records.h` 行模型 / `history_store.h` 门面）
- 相关文档：[dzsysmon.md](../components/dzsysmon.md)（历史写入与保留）、[《帧契约：系统监控》](../frame_contracts/sysmon.md)（快照形状）、ADR 0023（进程内周期采样与落盘线程模型）、ADR 0015/0016（`dzdb` 端口与后端选择）
