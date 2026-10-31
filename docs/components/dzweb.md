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
            pages/bytes prefetch -> close_old_pages
```

- params 全零时跳过 prefetch 但照常 release/touch/close（上报页索引 + 防 swap）
- api-only 模式（通道缺失）：调度直接 no-op
- 两半无共享状态、无顺序要求；writer 半边与写帧同线程串行，PagePool 无需加锁
- 替换合并语义由 core::TimerQueue 的 schedule_after_replace 保证（libs/core 单测覆盖）
- 与 md/td 的实现差异：错误包容为两段独立 try——reader 半边抛异常不阻断 writer 半边维护
  （md/td 是单一 try 包两半，reader 抛则 writer 跳过）；此差异系双线程拆分的自然结果，属有意为之

## 系统监控历史查询（#79）

历史查询**不走帧、不读镜像**：`dzweb` 以只读连接直接打开 `$DZTRADER_HOME/db/sysmon.db`，经 REST
`GET /api/sysmon/history` 提供（契约 sysmon §6.1、契约 rest §2.8）。`dzsysmon` 是唯一写者，`dzweb`
只读——两侧共用 `libs/sysmonstore`（schema 与行编解码的唯一真相源，ADR 0033）。

- **线程**：REST 回调在 ② 网络 IO 线程（单线程串行），每次请求新建只读连接、请求结束即关闭。
  `HistoryReader` 非线程安全，因此不缓存跨请求连接。
- **降级**：库缺失 / schema 版本不符 / 文件损坏一律 `200 + {"available":false,"reason":...}`，
  不抛、不 5xx、不返回伪造数据（三种 reason 与 `HistoryAvailability` 一一对应）。
- **不动实时链路**：历史端点不写 `MirrorStore`、不广播 WS、不发帧；前端历史视图也不订阅 WS，
  因此切换视图不影响 WS 连接与实时快照镜像。
- 实现分层（查询语义在共享模块、HTTP 装配在 `sysmon_history_service`）见
  [dzsysmon.md](dzsysmon.md) 的「历史查询」节——查询语义由 schema 形状决定，属共享模块，
  不是 dzweb 的私有逻辑。

## 监听与暴露面（#201）

| 项 | 现状 |
|---|---|
| 默认监听 | `server.listen = "0.0.0.0"` + `server.port = 8080`（`webui.json`，缺失时自动生成） |
| 传输 | **无 TLS**：只有 `addListener`，不加载证书；口令 / JWT / REST / WS 全为明文 |
| 凭证 | JWT 由前端存 `localStorage`；默认管理员口令 `88888888` |
| 运行期可改 | **否**：`listen` / `port` 仅在启动时读取，改配置文件后重启生效（UI 上「不可变，改配置文件重启生效」标签准确） |
| 启动告警 | 非回环监听时打一条 WARN（`webui management plane exposed to network \| listen=... tls=disabled ...`） |

**默认值不改**是维护者 2026-10-29 的决定（局域网多机盯盘），本组件文档只固化事实与告警口径：

- 回环判定唯一入口是 `is_loopback_listen`（`config.cpp`）：只认 `localhost`、IPv4 `127.0.0.0/8`
  字面量、IPv6 `::1`（含 `[...]` 形式）；通配地址、其他网卡地址、主机名、空串与带端口写法一律
  判为暴露 —— 方向保守，宁可多报不可漏报。
- WARN 是**纯可观测性**：不改变监听地址、不改变退出码、不影响 `/health` 与 API-only 降级路径。
- `webui.json` 自动生成时 `server._comment` 写明风险与改法（JSON 无注释语法，加载器忽略未知键）。
- 反向代理 TLS / 默认口令处置不属于本组件范围：TLS 需另立 ADR，口令见「密码处理加固」票（#202）。
