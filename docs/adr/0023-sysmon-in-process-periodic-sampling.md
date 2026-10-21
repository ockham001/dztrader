# ADR 0023: dzsysmon 进程内的周期采样——对「无轮询」原则的刻意偏离

## Status

Accepted（2026-10-19）

## Context

[dztraderd.md](../components/dztraderd.md)「设计原则」把「**无轮询**：一切事件源均由信号量或回调驱动，绝不采用轮询」写成硬约束。子进程退出由 `ChildProcess::async_wait` 回调感知，通道唤醒由 `NamedSemaphore` 驱动，运行期不做心跳探测——主进程侧确实可以完全事件驱动。

但资源采集无法这样实现：**所有速率型指标的语义都是「两次采样求差」**——CPU 使用率（`/proc/stat` 累计 jiffies、`GetSystemTimes` 累计 FILETIME）、磁盘吞吐（`/proc/diskstats`、`IOCTL_DISK_PERFORMANCE` 累计计数）、网络速率（`/proc/net/dev`、`GetIfEntry2` 累计计数）。操作系统不提供「CPU 使用率变成 37% 了」这样的变更通知；变更通知也无法表达「区间平均值」。参考实现 psutil 同样只能周期采样。

因此需要一个明确的判断：**这个偏离被允许到什么边界**，以及采样时钟怎么实现才不在 7×24 尺度上累积误差。

关联事实（设计期侦察所得）：

- 事件通道是**广播 + 全员唤醒**：任何进程写任何一帧都会唤醒全部已注册进程（`general.md` §4.1）。因此实时推送速率不能等于采样速率，否则每个策略进程会被高频唤醒。
- `dztraderd` 的全部周期性工作已经跑在单个 `io_context` 上（60s PageCleaner、`check_interval_min` 维护、md 就绪 watchdog、重启退避、关闭超时）；`dzweb` 另有 500ms 的 `trantor::TimerId` 日志 tail 定时器。asio 定时器在本仓已是既有实践，`steady_timer` 走单调时钟、免疫 NTP 跳变。
- 动态修改采集间隔必须**即时生效**：用户明确要求「由 60s 改为 1s 时不能等几十秒才生效」，这排除了 `sleep(interval)` 循环。

## Decision

1. **周期采样仅允许存在于 `dzsysmon` 进程内部。** `dztraderd` ↔ `dzsysmon` 之间仍 100% 事件驱动：信号量唤醒、子进程退出回调、定向 `DZ_FRAME_SHUTDOWN`，与其余子进程一致。本 ADR 不改变主进程的任何事件模型。

2. **采样时钟用 asio `steady_timer`**，并遵守三条纪律：
   - deadline 按「上次 deadline + interval」推进（漂移校正），不用「now + interval」；
   - 错过 tick 时**跳过并记缺口标记**，不补采（补采会伪造出密集采样）；
   - handler 内不做任何阻塞 I/O。

3. **计量间隔与记录间隔分离。** 计量固定 1s 连续运行（速率型指标必须有短窗口，否则 15 分钟平均会把 30 秒尖峰完全抹平）；记录间隔可配（默认 1min，特殊期为 1s，也可 15min/1h）。每条记录保存该窗口的 min/max/avg/last，**尖峰由 max 保留**。

4. **历史落盘在独立线程执行**（有界队列 + 批量提交，模型取自 `apps/ctp/td/td_persist_writer.cpp` 的 `PersistWriter`），采集线程只入队、永不阻塞。理由：SQLite 的 WAL checkpoint 与 fsync 在磁盘繁忙时可阻塞数十至数百毫秒，若在 `io_context` 线程上执行，会同时拖住采集（历史出现缺口）与帧处理（`SHUTDOWN` 无法及时响应 → 被主进程强杀 → 重启循环）。

## Alternatives

- **用 OS 变更通知替代轮询**（Windows WMI/ETW 事件订阅、Linux inotify 等）：否决。计数器语义要求两次采样求差，变更通知给不了区间平均值；Linux 无通用等价物；且 WMI/ETW 的事件通路本身更不稳定、更难跨平台统一。
- **让 `dztraderd` 自己采集**：否决。主进程承担编排职责，不应引入周期性系统调用；且违背「一个职责一个进程」的进程模型。
- **`sleep(interval)` 循环**：否决。动态改间隔需等一整轮才生效（用户明确排除），且无法做漂移校正，长时间运行后采样相位会持续偏移。
- **实时推送速率与采样速率一致**：否决。事件通道广播会唤醒全部进程，1Hz 推送等于给每个策略进程每秒一次无谓唤醒，违背「核心路径极致性能」。
- **把落盘放在 `io_context` 线程（接受历史缺口）**：可行但否决。用户明确把「足够稳定」列为第一优先级，磁盘卡顿不得传染到采集与帧处理。

## Consequences

- **Positive**：速率型指标准确，尖峰不丢失；动态改间隔即时生效；采样抖动与磁盘延迟都不传染到交易路径；主进程的事件驱动原则未被破坏。
- **Negative / Known residual**：
  - 引入本仓第一个「内部周期采样」进程。将来若再加同类进程（如更细粒度的硬件传感器采集），必须引用本 ADR 说明其同样属于进程内例外，不得扩散到主进程或网关进程。
  - 计量固定 1s 意味着 `dzsysmon` 每秒有一次定时器唤醒，`/proc/stat` 读取约 10μs 量级，相对系统噪声可忽略；但这是本进程的常驻成本，需要在 README/契约中记录。
  - 时钟跳变（NTP 步进）会让历史桶边界失真，需在历史数据中打「时间跳变」标记，不得假装连续。

## References

- 讨论日期：2026-10-19（dzsysmon 设计 grill）
- 源码位置：`apps/master/shm_manager_maintenance.cpp`（asio 维护定时器先例）、`apps/master/process_supervisor.cpp`（watchdog/退避/关闭超时先例）、`apps/ctp/td/td_persist_writer.cpp`（`PersistWriter` 线程模型）、`apps/webui/log_service.cpp`（500ms 亚秒定时器先例）
- 相关文档：[dztraderd.md](../components/dztraderd.md)（设计原则「无轮询」）、《帧契约：通用规则》§4.1（事件通道广播语义）
