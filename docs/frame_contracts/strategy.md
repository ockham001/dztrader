# 帧契约：策略

本文件覆盖 `DZ_FRAME_STG_USER_INPUT`、`DZ_FRAME_STG_USER_OUTPUT`、`DZ_FRAME_SET_LOGICAL_POSITION`、`DZ_FRAME_STG_SCHEDULE` 四个帧，以及策略 SDK（`dz_next_event`）对平台帧的拦截/放行总则。其他帧总则见《帧契约：通用规则》；SDK 全量透传的 `DZ_FRAME_ACCOUNT_STATUS` 帧语义见《帧契约：账户登录状态》。

- `STG_USER_INPUT` / `STG_USER_OUTPUT`：策略与 UI 之间的交互文本/数据帧，使用 `DzExtInstFrameHeader` 扩展头，`instance_id` = **裸策略名**（如 `stg_demo`，策略间唯一）。
- `SET_LOGICAL_POSITION`：**basic 帧**（仅 `DzFrameHeader`，无扩展头），策略身份在 payload `strategy_id` 字段，同样为裸策略名。
- `STG_SCHEDULE`：策略定时器触发帧（`dz_schedule_*`），**仅 SDK 本地合成、不写共享内存**，经 `dz_next_event` 返回；basic 布局 `DzFrameHeader` + `DzScheduleEvent{timer_id}`（16 字节），指针有效期至下一次 `dz_next_event`/`dz_release`。

**身份边界**（总则 §5）：本契约全部帧的身份字段（帧头 `instance_id`、payload `strategy_id`）一律用**裸策略名**——策略帧只属于策略域，不会与其他进程混淆，策略名在全部策略中唯一。`stg.<name>` 前缀**仅用于内存中的订阅者/信号量/reader/md 订阅者身份**（与系统组件区分），不出现于策略帧、展示层与 SDK 接口。

**页大小约束**：本契约两个变长帧（`STG_USER_INPUT`/`STG_USER_OUTPUT`）的总大小（帧头+扩展头+payload）不得超过通道单页大小；`STG_USER_OUTPUT` 侧由策略 SDK 写侧（`dz_output_ui`）按 `min(1MB, 页大小−帧头开销)` 截断 payload，超出部分丢弃；`STG_USER_INPUT` 侧 UI 投递方须遵循同一单页上限。

类型层真相源：`libs/strategy_api/include/dztrader/struct.h`、`libs/core/include/dztrader/core/core_struct.h`（`DzLogicalPosition`）。

---

## DZ_FRAME_STG_USER_INPUT

**语义**：向指定策略进程投递用户输入（UI → 策略）
**数据流**：形态 3（总则 §4.2）——前端 → dzweb → 目标策略进程（帧头 `instance_id` = 目标裸策略名，如 `stg_demo`）；无 RTN，单向投递（fire-and-forget）
**Payload**：变长 UTF-8 文本或 JSON（策略自行组织格式），无固定 schema

**时序**：无同步响应、无 RTN；本契约不规定策略的响应义务，前端不设 pending

**约束**：
- 定向帧，仅 `instance_id` 匹配的策略进程处理（策略 SDK 按自身裸策略名匹配）
- 策略侧接收 API 由 SDK 提供（`on_user_input` 回调，见 `strategy_base.h`）；SDK 按 `instance_id` == 本策略名 定向过滤后再回调
- 发送方（dzweb）未知策略是否存活时，帧可能无人认领被丢弃（总则 §4.1）

**镜像**：不进 dzweb 镜像

---

## DZ_FRAME_STG_USER_OUTPUT

**语义**：策略向 UI 自主输出数据（策略 → UI，类似 stdout），对应 `dz_output_ui`
**数据流**：形态 6 变体（总则 §4.2）——策略进程 → dzweb 转发 → 前端（帧头 `instance_id` = 来源裸策略名）；无前端入口；无 RTN
**Payload**：变长 UTF-8 文本或 JSON（策略自行组织格式），无固定 schema

**约束**：
- `instance_id` = 来源裸策略名（与 dzweb 进程镜像 key 一致，零转换关联）
- 与 `NOTIFY_UI`（契约 notify-ui）的区别：`NOTIFY_UI` 是通知消息（有级别/弹窗/时间戳，进通知缓存）；`STG_USER_OUTPUT` 是策略自主上行输出（无级别无弹窗，不进缓存）；是否响应 UI 输入由策略自行决定，本契约不定义配对关系
- 接收方（dzweb）当前未消费本帧（`dz_output_ui` 写帧但 dzweb 无对应处理，见 README 范围与遗留）；契约定义语义，实现滞后由 general §11.3 checklist 跟踪

**镜像**：不进 dzweb 镜像

---

## DZ_FRAME_SET_LOGICAL_POSITION

**语义**：策略上报自身逻辑持仓（策略认为应持有的仓位），用于 UI 监控策略持仓与账户实际持仓的偏差，对应 `dz_set_logical_position`
**数据流**：形态 4（总则 §4.2）——策略进程 → dzweb（basic 帧，策略身份在 payload `strategy_id`）；无前端入口；无 RTN
**Payload**：struct `DzLogicalPosition`（真相源：`libs/core/include/dztrader/core/core_struct.h`，非 JSON；字段表不重复）

**约束**：
- basic 帧：仅 `DzFrameHeader` + struct payload，无扩展头；来源策略由 payload `strategy_id` 携带（总则 §5：帧头无 `instance_id` 的帧不得依赖帧头路由）
- `strategy_id` = 裸策略名，与 `DzOrderReq.strategy_id`/`DzOrderReport.strategy_id`（TD 透传裸名）一致，供 dzweb 关联订单 ↔ 逻辑持仓
- `account_id` / `instrument_id` 为空串表示通配（清空该维度全部逻辑持仓，`net_volume` 忽略固定为 0），语义与 SDK 侧 `dz_set_logical_position` 文档一致
- 接收方（dzweb）当前未消费本帧，契约定义语义先行

**镜像**：dzweb 未来按 `strategy_id` 维护逻辑持仓镜像（当前未实现）

---

## DZ_FRAME_STG_SCHEDULE

**语义**：策略定时器触发通知，对应 `dz_schedule_after/every/at/daily`（`libs/strategy_api/include/dztrader/api.h`）
**数据流**：**不走共享内存**——定时器是策略进程内的 SDK 机制，触发帧由 SDK 本地合成（模拟 shm 帧布局），经 `dz_next_event` 返回；不广播、无 RTN、不进镜像
**Payload**：struct `DzScheduleEvent{timer_id}`（真相源：`libs/strategy_api/include/dztrader/struct.h`）

**语义要点**：

- `timer_id` = `dz_schedule_*` 返回的稳定 ID，周期/每日定时器每次触发的 `timer_id` 不变，`dz_schedule_cancel` 用同一 ID
- 帧指针仅在下一次 `dz_next_event`/`dz_release` 前有效；本地缓冲 32 槽，缓冲满时触发**推迟投递、绝不丢帧**（等待用户下一次领取腾槽补投）
- 定时器推进点唯一在 `dz_next_event`（通道无用户帧时 tick）；`dz_wait` 纯等待（无定时器无限阻塞 / 有定时器等待至最近到期，已到期立即返回免系统调用）；`dz_next_md` 不驱动定时器。契约要求策略调用 `dz_next_event` 消费事件流——定时器是事件流的一部分，不调用则不触发
- 一次性（after/at）触发后自动取消；周期（every）按到期点+interval 重排不漂移，停滞期间错过的整周期不补触发；每日（daily）每次触发后按 wall clock 重算次日（自动适应时区/夏令时）
- 时间点参数为距午夜毫秒数（本地时区），如 14:55:00 → 53,700,000

**约束**：

- SDK 内部任务（SHM 预加载随机延迟）与用户定时器共用队列，但内部 ID 对用户不可见，`dz_schedule_cancel`/`dz_schedule_cancel_all` 结构上无法误删
- `dz_schedule_cancel_all` 仅清用户定时器；参数非法返回 `DZ_TIMER_INVALID` + `DZ_EC_INVALID_PARAM`，取消无效 ID 返回 false + `DZ_EC_TIMER_NOT_FOUND`

---

## 策略 SDK 事件拦截总则（dz_next_event）

**处理优先级**：用户帧 > 定时器帧 > 内部帧。用户帧（TD 回报/用户输入/SHUTDOWN）直接返回、零计时器开销；通道无用户帧时（通道空或 32 上限让位）触发到期定时器并返回定时器帧；内部帧在扫描用户帧时顺带消费（轻量处理，预加载重活由随机延迟定时器承担）。

**白名单（返回给策略用户）**：

- `TD_ORDER_RPT`(2000)/`TD_TRADE_RPT`(2001)：按 payload `strategy_id` 定向——仅 `strategy_id` == 本策略裸名的帧放行；`strategy_id` 为空（外部单/手工单，非任何策略所下）与其他策略的回报一律拦截丢弃（td 网关按下单 `DzOrderReq.strategy_id` 回填，见契约 td-order）
- 其余 TD 回报帧 2002–2017（`TD_POSITION_INFO`/`TD_TRADING_ACCOUNT`/`TD_GATEWAY_STATUS`/`TD_INSTRUMENT` 等）：暂不按策略过滤，全量放行
- `ACCOUNT_STATUS`(2018)：同上不按策略过滤，全量透传（payload 无 `strategy_id`，账户级广播帧）；SDK 引擎分发 `on_account_status` 回调（帧语义见《帧契约：账户登录状态》）
- `STG_USER_INPUT`（3001，定向本策略）：SDK 按 `instance_id` == 裸策略名过滤
- `REQUEST_SHUTDOWN`（12，`instance_id` == 裸策略名）：SDK 完成内部清理（取消内部预加载定时器、清定时器帧缓冲）后放行，策略用户可据此优雅退出（`REQUEST_SHUTDOWN_ALL`(20) 已移除：全项目无写入/消费端）
- 本地合成的 `STG_SCHEDULE`（3003）

**拦截（SDK 内部消费，不返回策略用户）**：

- `PRELOAD_EVENT_SHM`（11）/ `PRELOAD_MD_SHM`（17，`instance_id` 匹配本策略行情源）：随机 0–5s 延迟后执行预加载（契约 shm）
- `UPDATE_SHM_EVENT_SUBSCRIBER`（21）：SDK 内部 `refresh_subscribers()`
- `NOTIFY_MD_STARTED`（1007，本策略行情源）：SDK 自动补订阅期望集合
- 非本策略/空 `strategy_id` 的 `TD_ORDER_RPT`/`TD_TRADE_RPT`；非本策略 `instance_id` 的 `STG_USER_INPUT`/`REQUEST_SHUTDOWN`
- 其余平台帧（日志/SHM 配置、进程控制、md 控制、TD 控制 21xx、`STG_USER_OUTPUT`/`SET_LOGICAL_POSITION` 他策略回声等）：丢弃（`TD_QUERY_ACCOUNT_STATUS` 是 SDK 写端帧——由 `dz_query_account_status` 发出，非读端白名单成员）

**防饿死上限**：每次 `dz_next_event` 调用最多连续消费 32 条内部帧，超过则本次让位（优先返回已到期定时器帧，否则 NULL，下次调用继续处理）；用户帧随时立即返回，绝不被吞。

---

## 变更流程

本契约随策略帧实现演进；修改必须执行 general §11.3 变更 checklist。
