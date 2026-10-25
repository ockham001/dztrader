# ADR 0034: 资源阈值越限降级为日志与色标，取消资源类 UI 通知

## Status

Accepted（2026-10-25）

Supersedes：`#41` spec 的用户故事 4/5/6/7/9，以及契约《帧契约：系统监控》§2.8 的「路径感知」段落与 `alerts[]` 字段。

## Context

`dzsysmon` 当前的阈值判定把「资源占用高」当作「系统将出故障」，对内存、交换、卷剩余空间发出 `NOTIFY_UI` **error + popup**。前端据此渲染 `aria-modal` 全屏遮罩（`App.vue` 的 `popup-overlay`），**用户必须点「确认」才能继续操作**。

三个事实说明这个等式不成立：

1. **资源占用高是常态，不是故障。** 开发机实测：swap 已用 3.3 GiB / 4.0 GiB = **83.5%**，默认阈值 `swap_used_pct = 50%` 下**永久越限**；同期物理内存使用率仅 42%，系统完全健康。Linux 上 swap 被大量使用是正常内存管理行为。日志实证：当日 `dzsysmon` 重启 3 次，每次启动即弹一次模态框（`threshold alert triggered | kind=swap_used`）。
2. **本机不专属本项目。** 服务器上可能并行运行其他服务，其对 CPU/内存的占用在预期之内。盯盘人员需要的是「能一眼看出的状态」，不是「必须处理才能继续看盘的中断」。
3. **弹窗语义被稀释。** 全仓共 11 处 `popup=true`，其中交易断线、认证失败、下单被拒等**真正需要立刻决策**的故障也在其中。资源占用混入同一通道后，用户会对弹窗脱敏，真正紧急的反而被忽略。

系统内已有正确先例：CPU 单核持续饱和（`cpu_sustain_used_pct=90` 连续 N 次采样）**从不发通知**，只在快照里标 `saturated=true`、UI 标红。这条路径已被验证可行，只是未推广到其他资源类指标。

外部实现同样是分离的两层：GNOME 由 `gsd-disk-space.c` 独立负责通知而 Nautilus 只画条；Windows 资源管理器容量条变红（剩 <10%）与 Storage Sense 通知是两套代码。**展示层与告警层分离是既有模式。**

## Decision

1. **资源类阈值判定不再产生 UI 通知**。内存使用率、交换使用率、卷容量（以及既有的 CPU 饱和）全部改为：**日志记录 + UI 色标**。`dzsysmon` 的 `NOTIFY_UI` 只保留真故障（历史库不可用、配置下发失败）。

2. **删除 `alerts[]` 快照字段与告警状态机的通知/冷却/恢复机制**。`AlertEvaluator` 保留其「状态跃迁 + 间隔重记」的核心语义，输出目标从 `NOTIFY_UI` 帧改为日志：进入越限 `WARN`、恢复 `INFO`、持续期每 `log_repeat_sec`（默认 600s）重记一条 `INFO`（含持续时长）。前端「资源告警」横幅与顶栏 `ALERT` 徽章一并删除。

3. **取消「关注路径」概念**。删除 `disk.watch_paths` 配置、快照 `watch_paths[]`、`volumes[].watched` 字段，以及 `collect_watch_paths` / `resolve_volume_mount_point` / `match_mount_point` / `normalize_path` 整条落卷解析链。卷容量监控范围改为**本机全部卷**（伪文件系统仍按既有清单过滤），告警范围不再需要「关注哪一卷」这一层。卷在 UI 上按使用率着色（≥70% 黄、≥90% 红），与顶栏既有阈值一致。

4. **`alerts` 配置段改名为 `thresholds`**，纳入全部阈值键：`cpu_sustain_used_pct`、`cpu_sustain_samples`、`memory_used_pct`、`swap_used_pct`、`volume_free_pct`、`log_repeat_sec`。删除 `volume_free_bytes`（绝对容量形态）。读到旧 `alerts` 段时**迁移**其值到 `thresholds` 并写回，不报错。

5. **`volume_free_bytes` 删除的理由**：全部主流实现均以百分比为主判据（Zabbix 80/90% used、PRTG 25%/10% free、Datadog 建议 10%、Netdata 90% used、GNOME 5% free）；绝对容量在 Windows（`LowDiskSpaceMinimum` 400 MB）与 GNOME（`free-size-gb-no-notify` 1 GB）中都只作**抑制项**，从不是独立触发条件。而 `OR` 语义下的 1 GiB 绝对阈值会让总容量小于 1 GiB 的卷（ESP、`/boot` 等）在**永远充足时也永久越限**——本机 `/boot/efi` 实测 99.4% 空闲仍命中。

## Alternatives

- **(B) 保留弹窗但大幅提高阈值**（内存 95%、交换 90%、磁盘 5%）：否决。问题不在阈值数值，而在**「资源占用」与「系统故障」共用打断通道**这个错误等式。提高阈值只是把误报推后，无法阻止它再次发生（换台机器、换个工作负载就复发）。
- **(C) 降级为 toast 通知**（`level=warning, popup=false`）：否决。toast 仍是**主动**推送；盯盘时持续的资源类 toast 同样干扰专注。且顶栏徽章已常驻显示 CPU/内存/各卷百分比并按 70%/90% 变色，「不在监控页就看不到」的缺口已被覆盖。交换指标没有徽章，但交换异常会在内存与磁盘症状上先显现。
- **(D) 阈值级别做成可配置**：否决。加密配置面换来的是「用户自己决定要不要被自己打扰」，而正确答案对所有人一致：资源占用不该打断。
- **(E) 保留 `watch_paths` 只在 UI 隐藏**：否决。「关注路径」的成本不在显示，而在整条落卷解析链（跨平台、符号链接、bind mount、大小写规则）的维护与测试。需求消失后保留它是留尸体。
- **(F) 顺手删掉全部资源指标**：否决。需求是「换表达方式」，不是「不监控」。展开 V 与历史曲线仍需要全部读数。

## Consequences

- **`dzsysmon` 不再需要「哪个卷承载关键数据」的知识**：`main.cpp` 的 `resolve_watch_paths`、`alerts.cpp` 的 `watched_mounts_of` 路径、`snapshot_builder` 的卷过滤参数全部消失。
- **契约改动**：§2 快照字段表删除 `watch_paths`/`alerts` 行、`volumes` 行去掉 `watched`；§2.4 删除 `watch_paths[]` 段落、`volumes[]` 字段表去掉 `watched`；§2.8 整节由「阈值告警」改写为「阈值越限与日志」；§3.1 配置段 `alerts` → `thresholds` 并删 `volume_free_bytes`。§8 的「阈值告警范围」段落同步改写。
- **历史库**：`volume` 域的 `watched` 维度不再写入。已落库的该维度行**不做主动清理**（`kSchemaVersion` 是表结构版本，删注册表条目不是结构变更），随保留期自然淘汰；`resolve_metric("volume.watched")` 此后返回 invalid（前端从不查询该指标，无功能影响）。
- **可观测性缺口**：资源高水位不再有「始于何时」的 UI 呈现。需要时查日志（`logs/dzsysmon_<date>.log`，保留 30 天），持续时长由间隔重记的条目推断。**这是有意取舍**：本系统的定位是量化交易系统，不是资源监控系统。
- **弹窗通道此后语义纯净**：仅交易/行情断线、认证失败、下单拒绝、进程强杀超时、配置/历史故障会弹窗。用户可重新信赖「弹窗 = 需要我立刻做点什么」。
- **温度传感器标识**（`<name>#<hwmonN>:<label>`）保持不变。它是历史库 `temperature` 维度的键，改格式会让已落库历史全部失配。人可读标签以 additive 字段另行提供。

## References

- 讨论日期：2026-10-25（dzsysmon UI/告警调整 grill）
- 外部证据：GNOME `gsd-disk-space.c`（`free-percent-notify` 0.05、`free-size-gb-no-notify` 1 GB、`min-notify-period` 10 min）；node_exporter node-mixin（avail 5%/3% + `predict_linear`）；Zabbix 官方模板（used 80/90%）；Netdata `health.d/disks.conf`（used 90%/98%）；PRTG（free 25%/10%，硬编码）；Windows Server 2003 `DiskSpaceThreshold`（10%）与 `LowDiskSpaceMinimum`（400 MB，AND 关系）
- 相关文档：[契约 sysmon](../frame_contracts/sysmon.md)、[dzsysmon 组件设计](../components/dzsysmon.md)、[契约 notify-ui](../frame_contracts/notify-ui.md)
- 相关 ADR：[0023](0023-sysmon-in-process-periodic-sampling.md)（进程内周期采样）、[0024](0024-sysmon-psutil-as-reference-only.md)（psutil 仅作参考）、[0025](0025-internal-process-category.md)（Internal 进程类别）、[0033](0033-sysmon-history-store-sharing.md)（历史库共享）
