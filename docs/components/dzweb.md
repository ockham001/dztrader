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

## 交易数据读路径（#246/#247/#248/#249/#250，ADR 0043）

交易记录（委托 / 成交 / 持仓 / 资金）**不进 `MirrorStore`、不做记录镜像**：

- **REST 直查交易库**：`GET /api/td/*`（契约 rest §2.9）每请求新建只读连接、`busy_timeout=200ms`、请求结束即关闭；游标分页（默认 200 / 上限 1000，禁 OFFSET 深翻页）。库缺失 / 版本不符 / 打开失败 / 锁等待 / 查询失败一律 `200 + {"available":false,"reason":...}`，不 5xx、不阻塞单 IO 线程（②）。只读连接与 schema 探测只实现一份（`apps/webui/td_read_db.{h,cpp}`），全部列表端点与统计重算共用；列表参数解析 / 游标（`apps/webui/td_list_query.{h,cpp}`）与行形状 / 派生字段（`apps/webui/td_row_json.{h,cpp}`）同样各只有一份——**委托、成交、持仓与资金共用同一套**，成交、持仓与资金不另写一份。
- **契约要求除端点差异外只写一份的实现**（#249 抽出，资金端点 #250 共用，注册表第三源 #251 接入）：账户全集与锚点日在 `apps/webui/td_account_scope.{h,cpp}`——账户全集 = 在线（Ready）账户 ∪ **账户注册表** ∪ 四表中出现过的 `account_id`（含离线，三源取并集，契约 rest §2.9.1）；第三源由 `TdCtrl` 从 `TdAccountRegistry` 取 `account_ids()` 传入，两个 query 的 `execute` 都有 `registry_accounts` 入参（默认空 = 注册表未就绪，前两源兜底）。**锚点日 = 该账户 `positions` 与 `trading_accounts` 两张快照表的 `MAX(date)`**（ADR 0046；追加流表不参与，否则「查询失败」会被显示成「空仓」）。锚点日取行的 `(account_id, date)` 条件与绑定由同文件的 `td_anchor_pairs` / `td_bind_anchor_pairs` 生成（持仓与资金共用，禁止两份同形 SQL）。
- **持仓 / 资金端点口径与委托/成交不同**（契约 rest §2.9.1）：两者都覆盖**全部已登记账户（含离线）+ 各账户锚点日**；委托/成交才是「在线 + 当前交易日 + 18:00 时间锚」。持仓游标坐标 `(account_id, instrument_id, direction)` 升序，`volume=0` 的行在 SQL 层过滤（清零帧是库内残留、不是持仓事实）；资金端点**不分页**（`next_cursor` 恒 null、`limit`/`cursor` 被忽略），每个账户一行（无资金记录的账户返回 `has_data:false` 占位行，`date`/`seq`/资金字段全 null——不用 0 或更早日期的旧行冒充锚点日数据，契约 rest §2.9.4）。两行都没有 `sort_key`（表无 `time` 列）。
- **WS 只推增量**：委托事件 `td_order_report`、成交事件 `td_trade_report`、持仓事件 `td_position_info` 与资金事件 `td_trading_account`（帧转发，payload = 契约 rest §2.9.3 / §2.9.4 的行，与 REST 同 schema），以及建连补发 `td_recent`（每在线账户最近 300 条，**同一环容纳四类事件**、按入环先后从旧到新）；记录基线一律由 REST 拉取，`snapshot` 不含交易记录（契约 webui-ws §2.5）。
- **域内只保留三样状态 + 统计计数器**（`apps/webui/td_domain_service.{h,cpp}`）：在线（`ACCOUNT_STATUS` Ready）账户集合与每账户 `date`、WS 建连补发环、日切感知，外加统计计数器（下节）。Ready/Offline 翻转、日切（`date` 变化）、在线集合变化 → 广播 `data_changed{scope:"td"}` 让前端清空重拉。
- **前端列表行为只有一份实现**：`apps/webui/frontend/src/composables/tdListStore.ts` 的 `createTdListStore`（REST 基线 + 游标预取 + WS 增量 10Hz 批量合并 + 有界窗口 + 重置触发 + 降级退避），委托（`stores/tdOrders`）与成交（`stores/tdTrades`）是它的两份配置而非两份实现。**持仓不走该工厂**：它是绝对态（按业务键覆盖、无 `sort_key` 排序插入），工厂的合并算法依赖 `sort_key`；持仓 store 因此另写一份（`stores/tdPositions`）。**共用的是降级行为的两块唯一实现**：统一降级文案 `tdDegradedText` 与退避重试定时器 `createTdRetryTimer`（`tdRetryDelay` 定退避口径）——两个 store 各写一份退避会出现同一时刻两个面板用不同节奏重试；**不共用的**是合并算法（持仓是绝对态、无 `sort_key`，语义不兼容）。
- **前端持仓 / 资金 store**（#249/#250）：两者都是绝对态（按业务键覆盖、无 `sort_key` 排序插入），不走 `createTdListStore`；持仓（`stores/tdPositions`）与资金（`stores/tdTradingAccounts`）各一份 store。**共用的是降级行为的两块唯一实现**：统一降级文案 `tdDegradedText` 与退避重试定时器 `createTdRetryTimer`（`tdRetryDelay` 定退避口径；`dispose()` 卸载停用、`init()` → `reset()` 重新武装）——各写一份退避会出现同一时刻两个面板用不同节奏重试；**不共用的**是合并算法（各自业务键与行形状不同）与 `loadBaseline`（持仓要翻完全部分页、资金是单请求，分页语义不同，抽成同一生命周期反而要参数化行形状与信封解析）。
- **已知限制**：`td_recent` 有界（300 条/账户，四类事件共享），dzweb 重启后为空——记录基线以 REST 为准，补发只是收敛窗口内的加速；帧可能先于库提交到达（异步持久化），同一记录由前端按 `seq` 合并（契约 webui-ws §5）。持仓/资金帧只在账户 Ready 之后转发——登录查询阶段（Ready 前）产生的快照帧不进补发环，前端靠 Ready 后的 REST 基线拿到同一批数据；资金盘中不刷新（只在登录收尾链/失败补查链更新，契约 td-data-sync §7 已登记）。

### 统计计数器（#247，契约 rest §2.9.5）

「未成交 / 总」统计是 dzweb 的**内存计数器**，不直查库、不进记录镜像、无 WS 推送：

- **口径固定**（唯一真相源是契约 rest §2.9.5）：`total_orders` / `total_volume` = 当前交易日**全部**在线账户委托（含撤单、废单，与未过滤委托列表行数一致）；`unfilled_orders` / `unfilled_volume` = `Submitting` / `NotTraded` / `PartTraded` 的笔数与剩余量（`volume - volume_traded`）。stats 不接受 `accounts` 过滤。
- **重算 = 全量重算**（`apps/webui/td_stats_query.{h,cpp}`）：**单只读事务**内取聚合 4 值 + 活动（未成交）委托 + 每账户四表 `MAX(seq)` 水位，三者同一快照。触发点：`TdStatsQuery` 注入后立即一次、账户 `Ready`、在线集合变化（含 Offline 下线）、日切、周期对账（默认 30s，`kTdStatsReconcileIntervalSec`）。
- **增量 = `ORDER_REPORT`**（`apps/webui/td_stats.{h,cpp}`）：计数器保留「活动委托小表」（= 当日活跃挂单数，有界）+ 每账户 **重放守卫水位**（四表 `MAX(seq)`，仅重算写入、增量永不推进——它是计数器内部去重，**不是** SDK ingest 那种记录水位，契约 td-data-sync §2 第 4 条）。帧 `seq ≤ 水位` 直接跳过（登录「重推最后一条」用原 seq，靠这条避免重复计数）——安全性来自契约 td-data-sync §2 的 seq **前缀不变量**。活动表内同一 `order_id` 的状态更新只做剩余量差量，重复应用幂等。
- **为什么不做全量镜像**：未成交集合有界且状态单向（终态不回退），终态委托不会再收到**状态变化**的 `ORDER_REPORT`，因此无需为终态保留状态去重记录（ADR 0043 决策 1/4）。**例外（#248）**：终态委托仍可能收到均价刷新帧（成交均价变化 + 新 seq），计数器按「不在活动表 = 新委托」处理会短暂多计一笔，≤1 个对账周期自愈（见下条）。
- **降级**：唯一降级是计数器未就绪 → `200 {"available":false,"reason":"not_ready"}`（契约 rest §2.9.5）。计数器就绪后交易库短暂不可读**不降级**——内存计数器仍有效，重算失败只保持旧值（与 §2.9.8 的列表降级区分）。
- **精度边界**（契约 §2.9.5 已声明）：两次重算之间，已终态委托因成交均价变化而收到的刷新 `ORDER_REPORT` 会被计成新委托一次（+1 笔 +volume），≤1 个对账周期（30s）后自动纠正；异步持久化窗口内计数器可能短暂领先/落后已提交数据，稳态一致。**刷新帧的条件是「该笔成交使均价变化且委托行已在基准中」**（#248，见 `dztd_ctp.md`「成交均价」），因此「部分成交后撤单仍有后续成交」或「终态后迟到的成交」会触发它——属预期，不是计数器缺陷。
- **前端**：无 WS 推送，按需轮询（≤1Hz，契约 webui-ws §5 第 10 条）；`data_changed{scope:"td"}` 与日切后清空重拉，降级时展示统一提示而不用 0 冒充数据。

## 交易账户配置与注册表（#251）

交易网关的**配置**与**账户级细状态**是**镜像域**（与交易记录相反）：`td_config` / `td_status`
进 `snapshot`、由增量消息更新，前端基线 = `snapshot` + 增量推送，不需要额外 REST 基线拉取。

| 项 | 帧 / WS | 镜像 domain 与 key | 对应契约 |
|---|---|---|---|
| 网关配置 | `TD_RTN_CONFIG` → WS `td_rtn_config` | `td_config`，key = 网关进程名（如 `dztd_ctp`），payload 整体透传（dzweb 不改写 `ext`） | td-config §2.4/§4/§8、webui-ws §2.1/§2.2 |
| 账户细状态 | `TD_RTN_STATUS` → WS `td_rtn_status` | `td_status`，key = **网关名:账户ID**（如 `dztd_ctp:account_001`），一个帧一个账户 | td-status §2.1/§3、webui-ws §2.1/§2.2 |

- **领域服务**：`apps/webui/td_config_domain_service.h` / `td_status_domain_service.h`，形态与
  `md_config_domain_service.h` / `md_status_domain_service.h` 同型（镜像 + 广播，无额外状态）。
  md 侧把 `source` 内嵌在 payload 且 `instance_id` 为空；td 侧按契约用 `instance_id` 承载归属
  （网关进程名 / 网关名:账户ID），payload 原样透传。
- **账户注册表**（`apps/webui/td_account_registry.{h,cpp}`，契约 td-config §5 / rest §2.9.9）：
  `account_id → {gateway, enabled}` 的平台侧**投影**，`account_id` 全局唯一（跨网关，ADR 0045）。
  - **内存优先 + platform.db 尽力持久化**（表 `td_account_registry`，只有 `account_id` /
    `gateway` 两列）：写失败只记 ERROR、不抛、不阻塞账户操作；启动加载失败则从空开始、随各
    网关配置回报重建（已知降级：最多少覆盖一个网关）。
  - **帧为权威，整组 reconcile**：`RTN_TD_CONFIG` 到达即按网关整组替换（先清该网关旧条目再
    重建），**只读通用层** `account_id` / `enabled`，不读 `ext`。配置帧 schema 异常（缺
    `accounts` 数组）只置「已收到过配置帧」标志、不 reconcile——用空集合 reconcile 会把该
    网关账户从 UI 抹掉。
  - **`enabled` 不持久化**（契约 rest §2.9.9 已知降级）：重启后从 DB 恢复的条目 `enabled`
    未知 → 回 `true`（不把账户误标为禁用），该网关配置帧到达后 reconcile 出真实值。
  - **`available` 判定**：收到过配置帧 **或** 启动加载恢复出条目即可用；两者皆否时
    `GET /api/td/accounts` 回 `available:false` + `reason:"no_gateway_snapshot"`——此时
    「无账户」不可信（与「确实没有账户」不可区分），前端不得当空列表。
  - **网关移除清理**：main 的 `RTN_PROCESS_CONFIG` handler 在全量进程配置帧的条目消失时调用
    `prune_gateways_not_in()`，只对 `dztd_*` 生效，**同时清 platform.db 该网关行**（否则重启
    后又被加载回来）；账户集合变化时广播 `data_changed{scope:"td"}`。
- **REST**（契约 rest §2.9.7/§2.9.9，`apps/webui/td_account_controller.{h,cpp}`）：
  - `GET /api/td/accounts`：账户注册表全集（**任意已认证用户**，不是 admin——与全部交易读端点
    权限一致）。账户配置页的**唯一账户列表来源**：跨网关账户全集 + 唯一性预检需要
    `account_id → gateway`，单网关的配置镜像拼不出来。
  - `POST` / `PUT /{account_id}` / `DELETE /{account_id}`（admin）：写
    `TD_REQ_MODIFY_CONFIG`（`ShmWriter::write_td_modify_config`），响应 `200 {"ok":true}`
    **只表示已写入事件通道**，生效以 `RTN_TD_CONFIG` 为准（成功推新值、失败推回滚旧值）。
  - **预检与机器码**（错误体 `{"error","code"}`，契约 rest §1 形状）：
    - POST：admin(403 `admin_required`) → 网关是进程配置中的 `dztd_*`(400 `invalid_gateway`)
      → Running(409 `gateway_not_running`) → `account_id` 非空且 ≤31 字节(400) →
      注册表查重（**任何**网关含已禁用/离线）(409 `account_exists`)。
    - PUT/DELETE：admin(403 `admin_required`) → 注册表解析 `account_id → gateway`
      (404 `account_not_found`) → `dztd_*`(400 `invalid_gateway`) → Running(409
      `gateway_not_running`)。**顺序与 POST 不同是有意的**：PUT/DELETE 的 gateway 只能从注册表
      解析，契约 §2.9.7 的字面顺序在这两个方法上不可机械照搬。
    - **503 不属于 §2.9.7 的预检 code 族**：事件通道不可用 / 写帧失败时回通用失败体
      `{"error":"shm writer not available"|"shm write failed"}`、**不带 `code`**——那是"请求
      未被受理"（基础设施不可用），不是预检失败。
  - **写端点不改注册表**：删除/新增以 `RTN_TD_CONFIG` 到达为准（reconcile），否则网关拒绝时的
    回滚会让 UI 显示错误的账户集合。

## 监听与暴露面（#201）

| 项 | 现状 |
|---|---|
| 默认监听 | `server.listen = "0.0.0.0"` + `server.port = 8080`（`webui.json`，缺失时自动生成） |
| 传输 | **无 TLS**：只有 `addListener`，不加载证书；口令 / JWT / REST / WS 全为明文 |
| 凭证 | JWT 由前端存 `localStorage`；默认管理员口令 `88888888` |
| 运行期可改 | **否**：`listen` / `port` 仅在启动时读取，改配置文件后重启生效（UI 上「不可变，改配置文件重启生效」标签准确） |
| 启动告警 | 非回环监听时打一条 WARN（逐字文案见下方「告警日志」） |

**默认值不改**是维护者 2026-10-29 的决定（局域网多机盯盘），本组件文档只固化事实与告警口径：

- 回环判定唯一入口是 `is_loopback_listen`（`config.cpp`）：只认 `localhost`、IPv4 `127.0.0.0/8`
  字面量、IPv6 `::1`（含 `0:0:0:0:0:0:0:1` 与 `[...]` 形式）；通配地址（`0.0.0.0` / `::`）、
  其他网卡地址、其他主机名、空串与带端口写法一律判为暴露 —— 方向保守，宁可多报不可漏报。
- WARN 是**纯可观测性**：不改变监听地址、不改变退出码、不影响 `/health` 与 API-only 降级路径。
- `webui.json` 自动生成时 `server._comment` 写明风险与改法（JSON 无注释语法，加载器忽略未知键）。
  注意它只在**首次生成**时写入：已存在的配置文件不会补注释（存量部署需手工加）。
- 反向代理 TLS 不属于本组件范围（需另立 ADR）；口令处理见下方「口令与会话（#202）」。

### 告警日志

`listen` 非回环时，`addListener` 之前打一条（日志文本的唯一真相源是本文件；README 只描述语义）。
下面是**节选**——真实行还带时间戳、`file=main.cpp:<行号>`、`pid`、`tid` 等固定字段，控制台
pattern 与文件 pattern 也不同（见 `libs/log/include/dztrader/log/log.h`）：

```text
webui management plane exposed to network | listen=0.0.0.0 port=8080 tls=disabled advice="use 127.0.0.1 for local-only access, or a strong password, network isolation, or a TLS reverse proxy"
```

- `listen` 为回环地址（如 `127.0.0.1`）时**不打**该行。
- **受 `webui.json` 的 `log.level` 约束**：该 WARN 在 `LogConfig::load()` 应用日志级别之后发出，
  因此把级别设成 `error` 时它会被静默丢弃——这是刻意不绕过日志级别的结果（告警不该比用户设定的
  日志策略更吵），需要时把级别调回 `info`/`warning`（默认即 `info`）。
- 默认口令告警（`default admin password in use`）是另一条独立 WARN，两者互不替代。
  该行**不回显口令值**（#202），只说事实与改法。

## 口令与会话（#202）

维护者 2026-10-31 拍板的四条决策，落在这里作为**行为真相源**（跨进程可观察语义在契约
[rest](../frame_contracts/rest.md) §1/§2.1/§2.2 与 [webui-ws](../frame_contracts/webui-ws.md) §1/§2.3）：

| 项 | 决定 | 实现位置 |
|---|---|---|
| 口令强度 | 最小长度 **12**，纯长度策略（不强制复杂度）；**服务端强制**，前端只提示 | `password_policy.h` 的 `kMinPasswordLength` / `is_password_acceptable` |
| 自助改密 | 必须校验**旧密码**；管理员重置**不需要**旧密码（admin 已鉴权，属正常设计） | `user_controller.cpp` `change_password` / `reset_password` |
| 默认口令 | **保留** `88888888` + 首次登录强制改密（不改成安装随机生成） | `db_init.cpp` / `main.cpp` 启动时按实际 hash 置位 |
| 会话吊销 | **token 版本号**：JWT 带 `ver` claim，改密/重置/禁用时 `users.token_version` 自增 | `jwt.cpp` `jwt_sign`/`jwt_verify`、`session_guard.h` `check_session` |

要点：

- **强度常量只有一处**：`kMinPasswordLength`。默认可令 `88888888` 只有 8 位，因此**改密后无法
  再设回默认口令**；旧实现里「改回默认口令 → 重置 ack 标志」的分支随之成为不可达代码并被删除。
- **强制改密是后端门禁**：`must_change_password=1` 的账户除 `POST /api/auth/change-password`
  与 `GET /api/user/me` 外，全部 `/api/*` 回 403 `password_change_required`；WS 直接拒连。
  前端弹窗只是让路径可走完，**不是**安全边界（契约 rest §1）。
- **自助改密会让当前会话失效**（后端自增 `token_version` 并踢掉该用户全部 WS 连接），
  前端改密成功后必须 `logout` 并回登录页——这是协议要求，不是体验细节。
- **默认口令告白名单一来源**：文案常量 `kDefaultPasswordWarningMessage`（`config.h`），
  REST 登录响应与 WS `default_password_warning` 都下发它，前端只展示不拼接（#202 之前
  两端各写一份 `88888888` 文案，改一端必漂移）。
- **升级影响**：新增 `users.token_version` / `users.must_change_password` 两列（`db_init.cpp`
  幂等迁移，默认 0）。存量 token 无 `ver` claim，按 0 解释，与迁移后 DB 默认值相等 →
  升级**不会**把全部存量会话打成 401；`must_change_password` 默认 0 也不改存量用户行为。

### listen 的取值约束

`server.listen` 直接交给 drogon 的 `addListener`，而 drogon **只接受 IP 字面量**：主机名
（含 `localhost`）与 `[...]` 方括号写法会在启动期 FATAL 退出
（`Failed to parse IP address ... - ListenerManager.cc`），进程以非零码结束、不会降级。
可用的回环写法是 `127.0.0.1`（或 `127.0.0.0/8` 内其他地址）与 `::1`；通配写法是 `0.0.0.0` 与 `::`。
`is_loopback_listen` 判的是「语义上是否仅本机可达」，**不**替代这项取值校验——本票不加启动前
校验，写错仍然是启动失败（与改动前一致）。
