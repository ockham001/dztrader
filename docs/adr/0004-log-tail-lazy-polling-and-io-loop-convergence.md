# ADR 0004: 日志 tail 采用惰性 500ms 轮询（事件驱动约束的受控例外）与 IO 循环线程收敛

## Status

Accepted

## Context

项目硬约束要求"禁止轮询，必须事件驱动（信号量/条件变量/等待原语），连 50ms 轮询都不可接受"。该约束已覆盖事件通道（SHM 帧）：`EventMonitor` 已迁移为命名信号量驱动的专用监听线程。但**日志文件 tail** 是唯一的例外场景：

1. **无现成信号源**：日志由各子进程 spdlog 自行写盘，dzweb 只是磁盘读者。文件增长没有类似 SHM 信号量的原生信号源，事件化需引入 OS 文件监视（Linux inotify / Windows ReadDirectoryChangesW，跨平台双实现）或改造所有写日志进程经 SHM 通知（逐条写会打爆事件通道，不可行）。
2. **现状性能缺陷**：改造前 `main.cpp` 用 `runEvery(0.05)` **无条件**每 50ms 轮询，且 `LogService::read_tail` 每次从文件字节 0 全量重读、靠行号跳过旧行——成本 O(文件大小)×订阅连接数，随日志增长线性恶化；且 `runEvery` 挂在 drogon 主循环上（见下述线程问题）。
3. **线程模型认知修正**：经查证 drogon 源码（`HttpAppFrameworkImpl.cc`），`setThreadNum(1)` 创建的是**一个 DrogonIoLoop 线程**承接所有连接回调（REST/WS）；`drogon::app().getLoop()` 返回的是**独立 static 主循环**，不参与连接分发。此前 `main.cpp` 注释"所有 REST/WS/定时器串行执行"是对线程模型的错误假设——导致 tail 定时器（主循环）与 WS 回调（IO 线程）并发访问 `sessions_`，属跨线程数据竞争（UB，可致新 timer 被误杀、日志静默断流）。

## Decision

1. **日志 tail 采用惰性 500ms 轮询**（用户明确裁决）：实时 tail 期间接受 500ms 轮询（原 50ms 放宽），**无任何客户端实时 tail 时关闭轮询**（零定时器唤醒）。间隔放宽至 500ms 后单 tick 成本极低，推送延迟上限 500ms 可接受。
2. **增量字节游标替代全量重读**：`LogService::TailCursor`（`byte_offset` + `line_no`）只读新增字节；订阅时刻用 `tail_baseline`（64KB 分块数换行）建立"只追新增"基线；轮换/截断（新文件 < 旧偏移）自动重置游标。每 tick 成本从 O(全文件) 降为 O(新增字节)。
3. **惰性 timer 收敛到 IO 循环**：`WsController` 的 `start_log_tail_timer`/`stop_log_tail_timer_if_idle` 用 `drogon::app().getIOLoop(0)`（惰性缓存，因 `getLoop()` 在 run() 前为 nullptr）注册/注销定时器；`poll_log_tail` 因此与 WS 回调同线程，`sessions_` 与 `log_tail_timer_active_` 全部单线程访问，消除跨线程 UB。
4. **本决策不豁免其他轮询**：除日志 tail 外的任何等待场景仍必须事件驱动（信号量/条件变量/OS 等待原语）。

## Consequences

- **Positive**：tail 每 tick 成本从 O(全文件) 降至 O(新增)；无订阅时零定时器唤醒（CPU≈0）；消除主循环/IO 线程跨线程数据竞争；CRLF 日志文件下推送行 parsed 与 REST 通道一致（`read_tail` 剥行尾 `\r`）。
- **Negative**：tail 推送延迟上限从 ~50ms 变为 ~500ms（已接受）；单 tick 吞吐上限 200 行 → 400 行/s（洪峰超出时游标持续推进最终追平，不丢行）。
- **Known residual**：`broadcast_to_all` 由 `g_broadcast_data_changed` 触发，其中 `register_raw` 帧在事件监听线程同步执行，可能跨线程触碰 `sessions_`——**仍属既有遗留**，本 ADR 不覆盖，另行评估。
- **后续可选项**：Phase 2 可用 OS 文件监视（inotify/ReadDirectoryChangesW）替换轮询彻底事件化，结构与 `EventMonitor` 一致；届时可撤回本例外。

## References

- 用户裁决：2026-08-16 全面审查（wave2）——"接受实时 tail 期间 500ms 轮询，无订阅时关闭轮询"
- drogon 源码：`HttpAppFrameworkImpl.cc`（`getLoop()` 静态主循环 / `ioLoopThreadPool_` / `getIOLoop(size_t)`）、`ListenerManager.cc`（连接 `setIoLoops` 全部分发到 IO 线程）
- 实现提交：`21c3b5e`（TailCursor 增量读 + 惰性 timer 初版）、`599f2d3`（timer 收敛到 IO 循环）、`aa68a84`（CRLF 剥除）
- 相关契约：《帧契约：webui-ws》§3（subscribe_log 逐连接订阅）
- 本地私有工作文档（不入库）：wave2 实施计划与 P3 backlog 位于 `docs/superpowers/plans/`，其"Wave2 已完成"节含完整 commit 清单，E 节含本次审查遗留项（E1-E7）
