# ADR 0035: dzsysmon 不做卷/盘符 ↔ 物理盘映射

## Status

Accepted（2026-10-26）

Supersedes：ADR 0024 Consequences 中「Windows 侧要比 psutil 多做一步……自建『逻辑卷/盘符 ↔ 物理盘』映射」的要求，以及《帧契约：系统监控》§8 里「映射由后续票 #103 提供」的承诺。

## Context

ADR 0024（2026-10-19，dzsysmon 设计 grill）提出：Windows 与 Linux 都要额外实现「逻辑卷 / 盘符 ↔ 物理盘」映射，否则「UI 上按盘符展示的磁盘 I/O 无法对应」。当时的前提是**路径感知的磁盘告警**：主进程关注共享内存目录与数据库目录，告警须按「关注卷所在的物理盘」聚合 I/O，因此映射是告警链的前置条件；ADR 0024 也把 Windows 卷↔物理盘身份映射列为三块高风险实现之一。

此后两个已落地的决策使该前提消失：

1. **ADR 0034（2026-10-25）删除「关注路径」概念**：`disk.watch_paths`、快照 `watch_paths[]` / `volumes[].watched`、落卷解析链与资源类 UI 通知全部移除，卷容量范围改为本机全部卷；契约 §8 同时写明「磁盘 I/O 无阈值越限消费」。告警再也不需要「哪个卷承载关键数据」，映射失去了唯一的功能消费者。
2. **#76 的实际 UI 不是「按盘符展示磁盘 I/O」**：监控页是「卷容量」与「每物理盘 I/O」两个独立区块，`disk_io[].device` 自身即物理盘的稳定标识（Windows `PhysicalDrive<N>`、Linux 整盘名）；ADR 0024 描述的对照关系与实现形态不符。

#103（2026-10-23 由 #76 收尾审查按 ADR 0024 原文创建）因此只剩「把卷容量与承载它的物理盘 I/O 关联展示」这一个展示层增强，没有当前需求作为消费者。

## Decision

**不实现卷/盘符 ↔ 物理盘映射。** 保持当前离线形态：

- 卷容量按挂载点/盘符独立展示（`volumes[]`，#75 / #122）；
- 磁盘 I/O 按物理盘独立展示（`disk_io[]`，`device` 为物理盘标识，#76）；
- 两者不做关联，不新增 `volumes[].physical_disks` 或等价字段。

理由：

- **没有消费者**：告警链已随 ADR 0034 删除；UI、历史查询与阈值判定都只按单一维度使用数据。为无消费者的关联引入 additive 字段，等于给契约增加永久维护面。
- **成本高、风险集中**：Windows 需按卷查询 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`（动态磁盘 / Storage Spaces 可跨多盘，字段必须是数组），Linux 需处理 LVM / md / dm / loop 设备回溯到整盘；还要两平台真机 oracle 与契约 / 类型 / schema / 前端全链同步。ADR 0024 已把这块列为风险最高的实现之一。
- **收益仅是展示关联**：卷容量与盘 I/O 各自已经诚实可用（不可用一律标不可用、不用 0 冒充），值班时按容量判断空间、按盘判断 I/O，不需要知道卷属于哪块盘。

## Alternatives

- **(A) 按 #103 原文实现两平台映射**：否决。无消费者，且把 ADR 0024 风险清单里最不确定的一块变成永久代码。
- **(B) 先做 Windows、Linux 后补**：否决。字段会在两平台语义不同，契约长期半实现，比不做更糟。
- **(C) 关票但保留 ADR 0024 原要求**：否决。仓库里留一个 Accepted 决策要求做、台账却关掉的事项，未来审查（#76 那轮就是如此）会再次把它当欠账翻出；必须用新 ADR 明确取代。
- **(D) 仅在前端按挂载源字符串猜物理盘**：否决。Linux 挂载源可能是 `/dev/mapper/*`、`UUID=`、网络文件系统等，字符串猜测必然产出错误关联；不做比猜错好。

## Consequences

- **#103 关闭为 not planned**；ADR 0024 保持 Accepted 不修改，仅由本 ADR 取代其卷盘映射要求。
- **契约与文档同步**：《帧契约：系统监控》§2.4 / §8 与 [dzsysmon 组件文档](../components/dzsysmon.md)的「已知限制」删掉「由后续票 #103 提供」，改为「不做该映射」并给出理由。
- **不再有悬空承诺**：全仓不再有「卷/盘符 ↔ 物理盘映射由后续票提供」的表述。
- **能力边界明确**：卷容量与磁盘 I/O 是两个独立维度；任何「卷 → 盘」的关联展示都不在本项目范围。未来若出现真实需求，按新 spec 重新立项，不复用 #103。
- ADR 0024 的其余决策（psutil 仅作参考、BSD-3 归属、内存口径、计数器回绕等）不受影响。

## References

- 相关 issue：[#103](https://github.com/ockham001/dztrader/issues/103)（本 ADR 关闭）、[#76](https://github.com/ockham001/dztrader/issues/76)（磁盘 I/O 实现）、[#120](https://github.com/ockham001/dztrader/issues/120)（交互收敛）
- 相关 ADR：[0024](0024-sysmon-psutil-as-reference-only.md)（本 ADR 取代其卷盘映射要求）、[0034](0034-sysmon-resource-thresholds-log-only.md)（删除关注路径与资源告警）
- 相关契约：[《帧契约：系统监控》](../frame_contracts/sysmon.md) §2.4 / §8
