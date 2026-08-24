# dzweb 组件架构（webui 后端）

> 本文是 dzweb 进程级组件页，与 `dztraderd.md` / `dzmd_ctp.md` / `dztd_ctp.md` 对称。
> 线程模型与帧处理归属以本文 + [帧契约](../frame_contracts/README.md) 为准；前端规范见 ADR 0005。

## 技术栈

C++20 · drogon（HTTP/WS）· nlohmann/json · msgpack-cxx · 自研 shm 库。前端 Vue3（`frontend/`）。

## 线程模型（经用户确认锁定，见 ADR 0005）

| # | 线程 | 职责 | 锚点 |
|---|---|---|---|
| ① | SHM 监听线程 | 阻塞在 NamedSemaphore；醒来后读帧 → 解码/分发（json 帧解码在本线程，handler 投递②；raw 帧入口在本线程、领域服务内部再投递②）；驱动预加载定时器 tick；60s 上报 reader 页索引 | `event_monitor.cpp` |
| ② | 网络 IO 线程 | 运行期**全部** REST/WS 回调、全部业务逻辑、MultiWriter 全部写操作（E1 修复确立；例外见③与启动序列）、日志 tail 惰性定时器（500ms，非独立线程） | `setThreadNum(1)`，main.cpp |
| ③ | drogon 主循环线程 | 框架占位：acceptor 宿主 + 启动窗口兜底投递（FIFO 保证早于 startListening 执行完，零连接期）；另：启动序列中 `write_query_all` 经 ShmWriter 写帧发生在主线程（监听已启动、run() 未开始，零连接期） | main.cpp `app().run()` |

全链路无轮询、无业务锁（单 IO 线程天然串行）。**前提约束：`setThreadNum(1)` 不可调大**——
refresh_subscribers / 预加载 writer 半边等 MultiWriter 操作的串行性均依赖单 IO 线程。

## 对象所有权矩阵

| 对象 | 独占线程 | 说明 |
|---|---|---|
| `shm::Reader`（dzevent） | 监听线程① | next_frame / release_old_pages / 预加载 reader 半边 |
| `shm::MultiWriter`（dzevent, 名 "dzweb"） | IO 线程② | ShmWriter 写控制帧、领域服务 RTN、refresh_subscribers、预加载 writer 半边 |
| MirrorStore / ProcessMirror / sessions_ | IO 线程② | 单写者 store |

## 事件通道数据流

```
master ──写帧+notify──> dzevent ──①读帧──> FrameRouter.dispatch
    ├─ register_json 帧: ①解码 -> 投递②执行 handler（镜像更新/WS 推送/notify_ui 写帧）
    ├─ register_raw 帧:   ①入口 -> 领域服务内部投递②执行实际动作
    │                     （例外: PRELOAD_EVENT_SHM 的 reader 半边有意留在①，见下节）
    └─ PRELOAD_EVENT_SHM: ①解析 -> 随机延迟后 reader 半边就地 + writer 半边投②（见下节）
dzweb REST ──> ShmWriter ──写控制帧──> dzevent ──> master/子进程（均在②）
```

## 事件通道预加载（PRELOAD_EVENT_SHM 被动响应）

实现：`event_preloader.h/.cpp`（`EventChannelPreloader`），方法名对齐 `md_api_scheduled.cpp`。
master 是唯一发起方（周期 `check_interval_min` tick / `preload_points` 时间点匹配，见
`apps/master/shm_manager_maintenance.cpp`）；dzweb 与 dzmd/dztd 同为被动接收方。

```
① 收广播(PRELOAD_EVENT_SHM, payload=DzShmPreload{bytes,pages,reserved})
     └─ random_jitter(0,5s) 后挂入本地 TimerQueue（tag="event_shm_maint"，重调度自动替换）
① 定时到期 on_event_shm_timer:
     ├─ reader 半边就地: pages/bytes prefetch -> release_old_pages
     └─ writer 半边 queueInLoop -> ② maintain_writer_shm:
            pages/bytes prefetch -> close_old_pages -> touch_write_position
```

- params 全零时跳过 prefetch 但照常 release/touch/close（上报页索引 + 防 swap）
- api-only 模式（通道缺失）：调度直接 no-op
- 两半无共享状态、无顺序要求；writer 半边与写帧同线程串行，PagePool 无需加锁
- 替换合并语义由 core::TimerQueue 的 schedule_after_replace 保证（libs/core 单测覆盖）
- 与 md/td 的实现差异：错误包容为两段独立 try——reader 半边抛异常不阻断 writer 半边维护
  （md/td 是单一 try 包两半，reader 抛则 writer 跳过）；此差异系双线程拆分的自然结果，属有意为之
