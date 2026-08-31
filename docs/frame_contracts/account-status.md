# 帧契约：账户登录状态

本文件覆盖 `DZ_FRAME_ACCOUNT_STATUS` 与 `DZ_FRAME_TD_QUERY_ACCOUNT_STATUS` 两个帧。总则见《帧契约：通用规则》。

两帧均为 **basic 广播帧**（仅 `DzFrameHeader`，无扩展头，无 `instance_id`），走事件通道（总则 §4.1），身份在 payload。帧头无 `instance_id` 的帧不得依赖帧头路由（总则 §5）。

类型层真相源：`libs/strategy_api/include/dztrader/struct.h`（`DzAccountStatus`、`DzAccountState` 三态枚举）、`libs/core/include/dztrader/core/core_struct.h`（`DzAccountStatusReq`）。

四端职责：

| 端 | `DZ_FRAME_ACCOUNT_STATUS`（推送） | `DZ_FRAME_TD_QUERY_ACCOUNT_STATUS`（查询） |
|---|---|---|
| td 网关 | 写端（权威） | 读端（权威应答） |
| master | 写端（兜底）+ 读端（建镜像） | 读端（兜底应答） |
| 策略 SDK | 读端（全量透传 → `on_account_status`） | 写端（`dz_query_account_status`） |
| dzweb | 不消费（无镜像、无 WS 映射） | 不消费 |

---

## DZ_FRAME_ACCOUNT_STATUS

**语义**：账户登录状态推送（td 内部 11 态聚合为三态）
**数据流**：形态 5（总则 §4.2）——basic 广播帧（身份在 payload `account_id` + `gateway_name`）；写端 = td 网关（权威）与 master（兜底）；读端 = 策略 SDK（全量透传 → `on_account_status`）与 master（建镜像）；dzweb 不消费（无镜像、无 WS 映射）
**Payload**：struct `DzAccountStatus`（104B，真相源：`libs/strategy_api/include/dztrader/struct.h`，非 JSON；字段表不重复）

**三态**（`DzAccountState`，真相源 `libs/strategy_api/include/dztrader/data_type.h`）：

`DZ_ACCOUNT_OFFLINE(0)` / `DZ_ACCOUNT_LOGGING_IN(1)` / `DZ_ACCOUNT_READY(2)`

映射（真相源：`apps/ctp/td/td_state.h` 的 `account_state_of`）：

| 三态 | td 内部态（TdState） |
|---|---|
| Offline | Idle、Disconnected |
| LoggingIn | Connecting、Connected、Authenticating、Authenticated、LoggingIn、LoggedIn、Confirming、LoadingInstruments（登录全过程） |
| Ready | Ready（= TdHealth::Up，可下单可查询） |

**触发场景**（写端 = td 网关，场景 7 为 master）：

1. td 启动读完配置：`config_.accounts` 全量各推一条（无会话 = Offline）
2. 三态翻转（与 `TD_RTN_STATUS`/`RTN_PROGRESS` 同步触发）：内部 11 态细变不推；三态去重（per-account 推送缓存）；Offline 例外恒推（断开/删除语义依赖必达）
3. 盘中连接/断开账户：新会话按状态机推进 LoggingIn→Ready；断开删会话前先推 Offline
4. 盘中配置变更致账户集变化（如新增未连接账户）：全量重推，无会话账户推 Offline（策略立即感知新账户存在且未登录）
5. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）：全量重推，重启自愈
6. 2115 查询应答（td 权威，应答规则见下节）
7. master 兜底：td 退出（崩溃/停止）代推 Offline（`gateway_name` = 真实网关名，镜像保留待重启快照重新确认）；2115 非空查询镜像未命中回 Offline（`gateway_name` = ""，非权威应答标记）

**重推与去重**：场景 1/4/5/6 为全量重推路径，**绕过三态去重**（全量重推必达——重启快照/查询应答必须到达重启后的新订阅者，稳态 READY 账户也不例外）；仅场景 2 的翻转推送去重。

**字段语义**：

- `trading_day`：距纪元天数（`DzDate`）；Ready 时 = 登录返回交易日，Offline 时 = 0（非法/缺失交易日回落 0）
- `gateway_name`：td 权威帧 = 网关进程名（如 `dztd_ctp`）；空串 = master 兜底应答（非权威标记），消费方据此区分权威/兜底

**约束**：

- basic 广播帧，所有事件通道读者可见；策略侧由 SDK 全量透传（不按策略过滤，见《帧契约：策略》拦截总则）
- 与 `TD_RTN_STATUS`（2104）并行分工：2104 面向 dzweb UI 全量细节（11 态细状态 + 登录进度，`instance_id` = "网关名:账户ID"），2018 面向策略/master 三态聚合
- master 兜底与 td 权威应答可能重复，消费方幂等取最新
- 竞态窗口：td 启动快照未到时 master 可能假阴性 Offline（td 快照到达自愈）；td 崩溃到 master 补推之间策略可能已下单（Offline 仅通知，不改变订单路径）

**镜像**：不进 dzweb 镜像；master 维护内存镜像 `网关名→账户集`（仅非空 `gateway_name` 帧入镜像，兜底应答回声不入——防自锁）；td 退出保留镜像（重启快照重新确认），remove 流程清除（清除时无主动推送，策略经 2115 兜底感知）

---

## DZ_FRAME_TD_QUERY_ACCOUNT_STATUS

**语义**：账户状态查询请求（策略 SDK `dz_query_account_status` 写入）
**数据流**：形态 5（总则 §4.2）——basic 广播帧；写端 = 策略 SDK；读端 = td 网关（权威应答）+ master（兜底应答）；dzweb 不消费
**Payload**：struct `DzAccountStatusReq`（32B，真相源：`libs/core/include/dztrader/core/core_struct.h`；`account_id` 空串 = 所有账户）

**触发场景**：策略主动查询（唯一触发；无 RTN 配对——应答即 `DZ_FRAME_ACCOUNT_STATUS` 帧，fire-and-forget）

**应答规则**：

- td（权威）：`account_id` 空 = 对 `config_.accounts` 全量应答；非空且在配置 = 应答该账户（含无会话 Offline）；非空且不在配置 = 不回（归属判断交 master）
- master（兜底）：非空且不在任何镜像 = 回 Offline（`gateway_name` = ""）；空 `account_id` = 不兜底（全量查询由各 td 权威应答覆盖，td 对自身配置有最终解释权）

**约束**：

- 历史帧不重放（事件通道 Reader 从最新写位置开始）：策略启动时初始状态必须主动查询（`dz_query_account_status`），不能依赖推送回放
- 镜像命中但 td 已退出（崩溃路径保留镜像）时，master 不兜底、td 无应答——该窗口由策略超时兜底感知
- 无应答超时兜底由策略用 `dz_schedule_after` 自行实现（SDK 不做超时机制）

**镜像**：不进任何镜像（查询帧；应答帧的镜像语义见 `DZ_FRAME_ACCOUNT_STATUS`）
