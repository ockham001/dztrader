# ADR 0024: 以 psutil 为参考实现，不引入第三方系统信息库

## Status

Accepted（2026-10-19）

> 卷/盘符 ↔ 物理盘映射的要求由 [ADR 0035](0035-sysmon-no-volume-disk-mapping.md) 取代（2026-10-26）；其余决策继续有效。

## Context

`dzsysmon` 需要跨平台（Windows 10/Server 2016 及以上、Linux）采集 CPU、内存、磁盘、网络四类指标。可选的现成实现：

| 候选 | 许可 | 状态 | 问题 |
|---|---|---|---|
| psutil | BSD-3-Clause | 活跃（发布版 7.2.2，8.0 开发中） | C 部分是 CPython 扩展（`PyObject`、方法表、GIL），不能直接用于 C++ |
| libstatgrab | 库 LGPL / 工具 GPL | 活跃 | 商业分发有链接义务；不覆盖 Windows 磁盘 I/O 与每核细节 |
| SIGAR | Apache-2.0 | 2023-12 后基本停更 | 无 Windows 现代 API 覆盖 |
| libgtop | GPL-2.0 | 活跃 | 许可不可用 |
| node_exporter | Apache-2.0 | 活跃 | Go、Linux 优先，仅可作语义参考 |

psutil 的真正价值不在代码，而在两件事：**该调哪个 OS API**（Windows：`GetSystemTimes`、`NtQuerySystemInformation`、`GetPerformanceInfo`、`IOCTL_DISK_PERFORMANCE`、`GetIfEntry2`、`GetSystemPowerStatus`；Linux：`/proc/{stat,meminfo,vmstat,diskstats,net/dev,cpuinfo,zoneinfo}`、`/sys/class/hwmon`、`/sys/class/power_supply`），以及**哪些边界必须处理**（32 位计数器回绕、`/proc` 字段在老内核缺失、`MemAvailable` 的内核 bug 与 LXC 失真、`/proc/diskstats` 四种历史行格式、分区重复计数、Windows 磁盘计数器需 `diskperf` 使能）。这些是十余年生产验证的结果，从零重新发现成本极高。

## Decision

1. **psutil 只作规格说明与算法参考**：读它来决定「调哪个 API、处理哪些边界」，用本项目的 C++20 代码自行实现。**不翻译其 C 代码，不嵌入 CPython 运行时。**

2. **改写自 psutil 的部分保留出处**：文件头注明来源与原始函数；仓库根放 `NOTICE` 记录 BSD-3-Clause 归属。BSD-3 第 2 条要求**二进制分发**时在随附文档中复现版权声明、条件与免责声明——随产品发布 `NOTICE` 即满足。

3. **遵守 BSD-3 第 3 条（不得背书）**：不得以 psutil 作者或贡献者名义推广本项目；UI 与文档**禁止**出现「psutil-powered」类措辞。

4. **不引入 libstatgrab / SIGAR / libgtop**（理由见 Alternatives）。

5. **把 psutil 的行为不变量搬为本项目单测的 oracle**，而不是自己重新想测试用例。重点条目：percent ∈ [0,100]、`used/free/available` 三者一致性、累计计数器单调、每核序列数 == 逻辑核数、`Σ(per-CPU times) == aggregate`（防重复计数）、计数器回绕语义。

## Alternatives

- **逐段翻译 psutil 的 C 代码**：否决。其 C 层与 CPython 强耦合（模块方法表、`PyObject` 返回、异常映射、GIL 管理），转换成本高于重写；且我们的采样调度、聚合、缺口标记、丢弃策略它没有，仍需自建。
- **引入 libstatgrab**：否决。库部分 LGPL——商业/闭源分发需承担动态链接与可重链接义务；且 Windows 侧恰好是短板（本项目最需要参考的磁盘 I/O 身份映射它不解决）。
- **引入 SIGAR**：否决。2023 年底后无实质维护，且不覆盖 Windows 现代 API。
- **嵌入 CPython 运行 psutil**：否决。为一个监控进程引入整个 Python 运行时，部署面、启动时间与 7×24 故障面都不成比例。
- **完全从零、不读 psutil**：否决。会重复踩已知的坑（尤其是 Linux `MemAvailable` 的三个回退分支与 Windows 32 位计数器回绕）。

## Consequences

- 两平台合计约 **2500–3000 行手写 C++**（不含进程级指标）；其中风险集中在三块，约 800 行：Linux 内存可用量计算、Linux `hwmon` 温度、Windows 磁盘 I/O 的卷↔物理盘身份映射。
- 需维护一份 `NOTICE` 随二进制分发；需在 CI 或开发清单中检查「禁止 psutil 背书措辞」。
- Windows 侧**要比 psutil 多做一步**：psutil 的 `disk_io_counters` 以 `PhysicalDriveN` 为键且**没有**逻辑卷↔物理盘映射；本项目要用 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 自建映射，否则 UI 上按盘符展示的磁盘 I/O 无法对应。
- `MemAvailable` 的取舍须照抄 psutil 而非自行发明：**内存使用率 = `(total − available) / total`，绝不用 `total − free`**（Linux 上 `total − free` 会把 page cache 全算作已用，误差可达 30% 以上）。

## References

- 讨论日期：2026-10-19（dzsysmon 设计 grill）
- 参考源码：本地 checkout `/home/ockham/projects/psutil/psutil`（`__version__ = "8.0.0"` 开发快照，尚未执行 8.0 的 `_psutil` 单模块合并 #2883）；发布版 `release-7.2.2` 用于对照
- 许可文本：psutil 仓库根 `LICENSE`（BSD 3-Clause）
- 相关文档：[ADR 0023](0023-sysmon-in-process-periodic-sampling.md)（采样时钟与线程模型）
