# 帧契约：交易网关状态

本文件覆盖 `DZ_FRAME_TD_RTN_STATUS` 一个帧。总则见《帧契约：通用规则》。

帧使用 `DzExtInstFrameHeader` 扩展头，**账户级帧**：`instance_id` = **账户 ID**（如 `"00001"`，柜台真实投资者代码，总则 §5），不携带网关名（[ADR 0056](../adr/0056-td-account-identity-investor-code-and-display-name.md)）。仅 RTN 推送，无 SET、无持久化、无文件 IO；交易网关在状态或状态字段变化时主动推送。

与《帧契约：账户登录状态》的分工：`ACCOUNT_STATUS` 面向策略/master 的三态聚合（Offline/LoggingIn/Ready）；本帧面向 UI 的细状态（12 态 + 登录信息 + 进度 + 最近一次失败），按账户上报。

**实现注记**：扁平结构（`date`/`api_version`/`sys_version` 在顶层、无 `ext`）与通用化 schema（`interface_type` + `ext`）的分层已随 #251 落地（`date` 改名随 #243）；dzweb 已消费本帧：按 `instance_id`（= 账户 ID）维护镜像 domain `td_status` 并 WS 推送 `td_rtn_status`（映射见契约 webui-ws §2.2）。

---

## 1. 接口类型识别

帧 payload 格式由**接口类型**决定，**payload 中不携带类型字段**。与《帧契约：交易网关配置》§1 的派生算法相同（去掉 `dztd_` 前缀 → tail 第一个 `_` 之前即 `interface_type`），但**输入不再是 `instance_id`**——`instance_id` 现在是账户 ID，本身不含接口类型信息：

- **dzweb**：只提供映射与透传——`GET /api/td/accounts`（契约 rest §2.9.9）给出 `account_id → gateway` 的注册表投影，账户级帧 payload 原样广播、不改写 `ext`；dzweb 本身不派生 `interface_type`。
- **前端解析层**：用帧头 `instance_id`（= 账户 ID）在已加载的账户注册表里查归属网关（契约 td-config §5），再按上面的算法从网关名派生 `interface_type`，据此选择卡片 schema；注册表未命中（账户已删 / 注册表降级）时回落配置镜像 `td_config`（唯一包含该账户的网关，契约 webui-ws §2.2），两者都未命中时不猜接口类型、也不自造账户行——等该网关配置帧到达后按配置镜像关联（未知字段忽略，总则 §8）。
- **后台进程**：每个交易网关进程只发自己的账户、只按自身接口类型构造 payload，不涉及识别。

| instance_id（账户 ID） | 注册表解析出的网关 | interface_type |
|---|---|---|
| `00001` | `dztd_ctp` | `ctp` |
| `00002` | `dztd_ctp_2` | `ctp` |
| `10001` | `dztd_xtp` | `xtp` |

---

## 2. CTP 类型（interface_type = `ctp`）

### 2.1 数据结构

#### TdStatus

交易网关账户级细状态（全量结构，RTN payload 用）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `account_id` | string | 账户 ID（柜台真实投资者代码，**与 `instance_id` 相同**；per-account 状态上报用） |
| `state` | string | 网关内部状态，取值见 2.2；与《帧契约：账户登录状态》三态聚合的映射由后者定义 |
| `date` | string | 会话交易日 `"YYYYMMDD"`（交易日，不是自然日）；未登录/进入 `Idle` 时为空串 |
| `login_time` | string | 登录时间 `"YYYY-MM-DD HH:MM:SS"`（服务器本地时间，由登录响应时间戳转换）；未登录/进入 `Idle` 时为空串 |
| `progress_desc` | string | 进度描述（展示文本，UTF-8）；无描述时为空串 |
| `progress_min` | int | 进度最小值（交易网关为 0） |
| `progress_max` | int | 进度最大值（交易网关为 10） |
| `progress_current` | int | 当前进度值 |
| `last_error` | object \| null | 最近一次登录链失败；`null` = 无未清空的失败。结构见 2.5 |
| `ext` | object | 柜台特有字段（CTP schema 见 2.3） |

> `progress_*` 与 `RTN_PROGRESS`（契约 progress）同源同值；本帧携带是历史兼容，前端进度展示以契约 progress 为唯一入口。

JSON 示例：

```json
{
  "account_id": "00001",
  "state": "Ready",
  "date": "20260808",
  "login_time": "2026-08-08 08:45:32",
  "progress_desc": "ready",
  "progress_min": 0,
  "progress_max": 10,
  "progress_current": 10,
  "last_error": null,
  "ext": {
    "api_version": "v6.7.2",
    "sys_version": "v6.7.2_20240105",
    "unknown_order_status": 0,
    "unknown_direction": 0,
    "unknown_price_type": 0,
    "unknown_position_effect": 0
  }
}
```

### 2.2 状态枚举（`state`）

`state` 取值为交易网关内部状态机（12 态，值以发送方进程状态机定义为唯一真相源；CTP：`apps/ctp/td/td_state.h` 的 `TdState`，JSON 为枚举名字符串）：

| 值 | 说明 |
|---|---|
| `Idle` | 未启动/已登出，初始状态 |
| `Connecting` | API 已初始化，等待前置连接 |
| `Connected` | 前置已连接，准备认证 |
| `Authenticating` | 认证请求已发，等待响应 |
| `AuthFailed` | **认证失败态**：停在 3/10，原因见 2.5 `last_error` |
| `Authenticated` | 认证通过，准备登录 |
| `LoggingIn` | 登录请求已发，等待响应 |
| `LoggedIn` | 登录成功，准备确认结算单 |
| `Confirming` | 结算单确认已发，等待响应 |
| `LoadingInstruments` | 合约加载进行中 |
| `Ready` | 初始化完成，可下单可查询 |
| `Disconnected` | 网络断开，接口库自动重连中 |

**失败态语义**（ADR 0059）：登录链上任一段失败后**停在它自己那一段**——`progress_current` 取该段的数值、**不回退**到上一段，`desc` 描述该段失败。失败态只回答「在哪一段失败」，段内具体原因由 `last_error` 承载（2.5），二者在同一次状态转移写出、同一帧发布。失败态**不占端点**：既不是 `progress_min` 也不是 `progress_max`。

**失败态集合与取值**（当前只落认证段；登录 / 结算确认 / 合约加载三段随 #495 / #496 补齐）：

| 失败态 | `progress_current` | `progress_desc` | 原先回退到 |
|---|---|---|---|
| `AuthFailed` | 3 | 认证失败 | `Connected`（2） |

三态聚合映射（Offline/LoggingIn/Ready）以《帧契约：账户登录状态》为准：失败态归 `LoggingIn`（会话未就绪）。

### 2.3 CTP `ext`

| 字段 | 类型 | 说明 |
|---|---|---|
| `api_version` | string | 接口库版本字符串，构造时设置一次 |
| `sys_version` | string | 登录响应中的系统版本；未登录/进入 `Idle` 时为空串 |
| `unknown_order_status` | int | 本会话累计收到的**不认识**的柜台委托状态次数 |
| `unknown_direction` | int | 本会话累计收到的不认识的持仓方向次数 |
| `unknown_price_type` | int | 本会话累计收到的不认识的委托价格类型次数 |
| `unknown_position_effect` | int | 本会话累计收到的不认识的开平标志次数 |

**未知枚举计数口径**（issue #367）：

- 四类**分开计数**，不合并成一个总数：「我们没跟上柜台」与「柜台给了坏数据」原因不同、
  处置也不同（前者补映射，后者查数据源）。
- 计数是**会话生命周期累计值**，单调不减，日切与断线重连都不清零；`Idle` 期间保留
  （与 `api_version` 同性质：诊断信息，不是会话业务数据）。
- 计数只统计「读不懂」，不统计柜台自己上报的 `OrderStatus='a'(Unknown)`——后者是柜台
  明确的暂态语义，不是我们的映射缺口。

### 2.4 清空与保留语义

- **进入 `Idle`**（登出/手动停止）：`date`、`login_time` 清空，`api_version`/`sys_version` 按 2.3 清空（`api_version` 为进程生命周期常量可保留），`progress_*` 归零，推送。`last_error` **不清空**（保留供回看，见 2.5）。
- **登录失败/登录响应解析失败**：`login_time` 清空并推送（防止前端误显示过期登录信息）。
- **断线（`Disconnected`，接口库自动重连中）期间**：上述字段**不清空**——`date` 的语义是当前会话交易日（由登录响应确定，是本交易日全部业务数据的时间锚），断线不改变交易日；`sys_version`/`login_time` 同样保留，重连成功后由新登录响应覆盖。
- **日切**：会话交易日变化（含日切后重新登录）时以新 `date` 全量推送；前端以字段值变化为准，不自行推断清空时机。

### 2.5 失败语义（`last_error`）

`last_error` 承载**最近一次登录链失败**，是本帧唯一的持久失败原因（`NOTIFY_UI` 的即时文案同源但非真相源；前端持久展示必须读本字段）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `code` | string | 机器码，取值见下表 |
| `message` | string | 用户可读原因（中文）。CTP 返回的 `ErrorMsg` 原文（已转 UTF-8）；空串表示无详情，前端按 `code` 映射固定文案 |
| `error_id` | int | CTP `ErrorID`；仅 `auth_rejected` 携带（无 `rsp_info` 时为 `-1`），其余为 0 |
| `timestamp` | int | 失败发生时间，Unix 秒级时间戳 |

`code` 取值（当前只接通认证段的两个码；登录 / 结算确认 / 合约加载各段的码随 #495 / #496 补齐）：

| code | 触发场景 | 对应失败态 |
|---|---|---|
| `auth_rejected` | CTP 拒绝认证（`OnRspAuthenticate` 的 `ErrorID != 0`），`error_id` 为 CTP 错误码 | `AuthFailed` |
| `auth_timeout` | 10 秒内未收到认证响应（认证超时） | `AuthFailed` |

**超时与柜台拒绝同等对待**（ADR 0059 决策 5）：两者进同一失败态、写同一 `last_error`，靠 `code` 区分。用户不需要关心「柜台拒了」还是「没人理」——两者对处置的差别远小于「停在哪一段」。

**设置与清空**：

- 设置：上述失败发生时随本帧推送（全量），与转失败态在同一次状态转移写出。
- 清空（置 `null` 并推送）：**下一次登录尝试被受理时**（`TD_CONNECT` 被受理 = 状态机 `on_connect`）；**转 `Ready` 时**。
- 进入 `Idle`（用户主动登出）**不清空**：保留最近一次失败供用户回看（与 md 一致）；进程重启后为空（新进程状态）。

**重推判据**：`last_error.timestamp` 参与本帧的重推判定——同一失败态下原因变化（如先柜台拒绝、重试后超时）时 `state` 与进度都不变，仅靠其余状态字段判定会让新原因到不了前端。

**前端义务**：

1. **登录 pending 的清除主信号 = `last_error.timestamp` 变化且非 `null`**（表示一次登录尝试已结束），不等待兜底超时（建议 45s，连接 30s + 登录 10s + 余量）。失败态下 `progress_current` 停在失败阶段、不回退，`current` 回退不再是失败信号。
2. `progress_current` 回退降级为「**主动中止**」信号保留：登出（→ `min`）与前置断线（回到连接阶段）仍使 `current` 回退，`current` 回退时同样清登录 pending。`last_error` 缺失（如进程刚启动）时兜底超时是最后一道信号。
3. 卡片持久展示「上次失败：`message` + 时间」；`message` 为空时按 `code` 映射固定文案；**不得把 `code` 原文直接展示给用户**。`timestamp` 非法/缺失时不展示时间（不得渲染 1970）。
4. 登录成功后 `last_error` 清空，前端不得保留旧失败展示；断开/重连期间展示以本帧全量覆盖为准。
5. 失败态不提供原地重试（ADR 0051 决策 4，ADR 0059 继承）：恢复路径统一为「先登出、再登录」；登录资格仍只在 `progress_current <= progress_min`。
6. 进度展示以《帧契约：进度推送》为唯一入口（本帧的 `progress_*` 是历史兼容，见 2.1 注）；`desc` 仅展示、不得用于状态判定。

---

## 3. DZ_FRAME_TD_RTN_STATUS

**语义**：交易网关上报账户级细状态（全量）
**数据流**：形态 4（总则 §4.2）——交易网关（帧头 `instance_id` = 账户 ID）→ dzweb；无前端入口；镜像（key = `instance_id` = 账户 ID）→ WS（映射见契约 webui-ws，随实现票同步）
**Payload**：JSON（TdStatus），**始终全量**

**触发场景**：

1. 状态或任何状态字段变化时推送（状态机翻转、登录信息填充/清空、`date` 变化、进度变化、`last_error` 的 `timestamp` 变化）
2. 交易网关启动初始化完成后按账户各上报一次
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：前端直接整体覆盖本地状态镜像
- 一个帧只对应一个账户（`instance_id` 即账户 ID，payload 冗余携带 `account_id` 供消费方定位与自校验）
- 账户被移除/网关退出前的最后一条状态由实现保证可观察（推 `Idle` 或停止推送，前端以在线集合（《帧契约：账户登录状态》）判断账户是否存在）
- 本帧非策略可见：策略 SDK 直接丢弃（《帧契约：策略》拦截清单）

**镜像与前端义务**：
- dzweb 按 `instance_id`（= 账户 ID）更新细状态镜像并 WS 推送；不进交易记录镜像
- 前端按 `instance_id`（= 账户 ID）关联账户行；以全量覆盖更新，不自行推断字段清空时机；`date` 用于数据日期展示
- dzweb 在网关停止 / 崩溃 / 被移除时按注册表反查该网关账户集，清掉各账户的 `td_status` 镜像并广播（注册表降级时放弃清理，宁残留不猜）；账户从配置帧消失时同样清掉

---

## 4. 校验（仅写与总则不同的规则）

- 仅 RTN 推送：无 SET/REQ、无校验失败路径；payload 为发送方进程状态机生成的全量快照，接收方（dzweb）不做字段校验，按全量覆盖写镜像。
- `instance_id` = 账户 ID，由发送方保证（本进程配置内的账户）；接收方**不做拆分**。`account_id` 字段与 `instance_id` 不一致时以 `instance_id` 为准并记 WARN（同一事实两处给出，不一致是发送方缺陷）。
- 未知字段忽略（总则 §8）。

---

## 5. 变更流程

本契约随交易网关状态实现演进；修改必须执行总则 §11.3 变更 checklist。
