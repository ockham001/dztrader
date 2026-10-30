# 帧契约：行情网关状态

本文件覆盖 `DZ_FRAME_RTN_MD_STATUS` 一个帧。总则见《帧契约：通用规则》。

帧使用 `DzExtInstFrameHeader` 扩展头，`instance_id` 为来源行情进程名（如 `"dzmd_ctp"`）。

仅 RTN 推送，无 SET、无持久化、无文件 IO。行情进程在网关元信息或订阅统计变化时主动推送。

网关的动态状态（连接/登录/断开等）与步骤进度由 `RTN_PROGRESS`（契约 progress）覆盖，本帧不包含状态与进度字段。登录/连接**失败原因**由本帧 `last_error` 承载（见「失败语义」）。

---

## 接口类型识别

与契约 md-config（《行情网关配置》）完全一致：payload 格式由 `instance_id` 派生的 `interface_type` 决定，payload 中不携带类型字段。当前仅定义 CTP 类型。

---

## CTP 类型（interface_type = `ctp`）

### 数据结构

#### MdStatus

行情网关状态（全量结构，RTN payload 用）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `api_version` | string | CTP API 版本字符串，构造时设置一次 |
| `sys_version` | string | 登录响应中的系统版本；未登录/进入 Idle 时为空串 |
| `trading_day` | string | 交易日 `"YYYYMMDD"`，登录响应填充；未登录/进入 Idle 时为空串 |
| `login_time` | string | 登录时间 `"YYYY-MM-DD HH:MM:SS"`（服务器本地时间，由登录响应时间戳转换），登录响应填充；未登录/进入 Idle 时为空串 |
| `expected_subscribe_count` | uint | 期望订阅合约数（有订阅者的合约条目数） |
| `subscribed_count` | uint | 已确认订阅合约数 |
| `last_error` | object \| null | 最近一次登录/连接失败；`null` = 无未清空的失败。结构见「失败语义」 |

JSON 示例：

```json
{
  "api_version": "v6.7.2",
  "sys_version": "v6.7.2_20240105",
  "trading_day": "20260808",
  "login_time": "2026-08-08 08:45:32",
  "expected_subscribe_count": 5000,
  "subscribed_count": 5000,
  "last_error": null
}
```

**清空语义**：进入未连接态（`Idle`）时 `sys_version`/`trading_day`/`login_time` 清空、订阅统计归零并推送；登录失败/登录响应解析失败时 `login_time` 清空并推送（防止前端误显示过期登录信息）。`last_error` 的清空规则见「失败语义」，**不随 `Idle` 清空**。前端依赖全量覆盖，不自行推断清空时机。

**断线（`Disconnected`，SDK 自动重连中）期间的保留语义**：上述字段**不清空**——`trading_day` 的语义是当前交易日（由登录响应确定，是本交易日内全部业务数据的时间锚），断线不改变交易日；`sys_version`/`login_time` 同样保留，重连成功后由新登录响应覆盖。断线时推送本帧：`subscribed_count` 归零、`expected_subscribe_count` 保留。网关的连接/登录**状态**不经本帧传达（本帧无状态字段），前端状态感知以 `RTN_PROGRESS`（契约 progress）为准；断线时本帧推送的作用是同步订阅统计变化。

### 失败语义（`last_error`）

`last_error` 承载**最近一次登录/连接失败**，是本帧唯一持久失败原因（`NOTIFY_UI` 的即时文案同源但非真相源；前端持久展示必须读本字段）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `code` | string | 机器码，取值见下表 |
| `message` | string | 用户可读原因（中文）。CTP 返回的 `ErrorMsg` 原文（已转 UTF-8）；空串表示无详情，前端按 `code` 映射固定文案 |
| `error_id` | int | CTP `ErrorID`；仅 `login_rejected` 携带，其余为 0 |
| `timestamp` | int | 失败发生时间，Unix 秒级时间戳 |

`code` 取值：

| code | 触发场景 |
|---|---|
| `connect_timeout` | 30 秒内未收到 `OnFrontConnected`（前置连接超时；进程已断开回 `Idle`） |
| `login_rejected` | CTP 拒绝登录（`OnRspUserLogin` 的 `ErrorID != 0`），`error_id` 为 CTP 错误码 |
| `login_timeout` | 10 秒内未收到登录响应（登录超时） |
| `login_parse_error` | 登录响应缺失/解析失败（含交易日解析失败） |

**设置与清空**：

- 设置：上述失败发生时随本帧推送（全量）。
- 清空（置 `null` 并推送）：下一次连接/登录尝试开始时（`REQUEST_MD_CONNECT` 被受理即开始）；登录成功时。
- 进入 `Idle`（用户主动登出）**不清空**：保留最近一次失败供用户查看；进程重启后为空（新进程状态）。
- 失败后进程状态：`connect_timeout` 已断开回 `Idle`；其余失败仍持有前置连接，处于登录失败态，**可直接重试登录**（`REQUEST_MD_CONNECT` 在已连接/登录失败态等价于重新发起登录），不要求先登出。失败态 `RTN_PROGRESS` 的数值映射不变（与 `Connected` 同为 `current=2`），`desc` 为「登录失败」——状态判定仍以数值映射为准（契约 progress），`desc` 仅展示。

**前端义务**：

1. `last_error.timestamp` 变化且非 `null` 表示一次登录失败已结束：清除该实例的登录 pending，不等待超时兜底。
2. 卡片持久展示「上次失败：`message` + 时间」；`message` 为空时按 `code` 映射固定文案；**不得把 `code` 原文直接展示给用户**。`timestamp` 非法/缺失时不展示时间（不得渲染 1970）。
3. 失败态可重试判定：仅当 `last_error != null` 且 `RTN_PROGRESS` 处于「已连接/登录失败」数值档（`current=2`）时启用重试（下发 `REQUEST_MD_CONNECT`），不要求先登出；`Disconnected`（`current=1`）期间 `last_error` 可能保留，但此时进程自动重连中、不接受新的连接请求，前端**不得**据 `last_error` 启用重试（否则请求被拒、pending 悬挂至超时）。`Idle`（`current=0`）由常规「登录」按钮承担。
4. 登录成功后 `last_error` 清空，前端不得保留旧失败展示；断开/重连期间展示以本帧全量覆盖为准。

---

### DZ_FRAME_RTN_MD_STATUS

**语义**：行情进程上报当前网关状态（全量）
**数据流**：形态 4（总则 §4.2）——行情进程（帧头 `instance_id` = 来源）→ dzweb；无前端入口；镜像 `md_status` 域 → WS 消息 `md_rtn_status`
**Payload**：JSON（MdStatus），**始终全量**

**触发场景**：

1. 网关元信息、订阅统计或 `last_error` 变化时推送（如登录成功填充版本/交易日/登录时间，订阅统计随订阅进度变化，登录/连接失败写入原因，未连接态清空）
2. 行情进程启动初始化完成后上报
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：前端直接整体覆盖本地状态镜像

**镜像**：
- dzweb 更新该实例镜像的 `md_status` 域，并 WS 推送
