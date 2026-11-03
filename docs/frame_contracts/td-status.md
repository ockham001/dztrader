# 帧契约：交易网关状态

本文件覆盖 `DZ_FRAME_TD_RTN_STATUS` 一个帧。总则见《帧契约：通用规则》。

帧使用 `DzExtInstFrameHeader` 扩展头，`instance_id` = **网关名:账户ID**（如 `"dztd_ctp:account_001"`，网关名为来源 td 进程名）。仅 RTN 推送，无 SET、无持久化、无文件 IO；交易网关在状态或状态字段变化时主动推送。

与《帧契约：账户登录状态》的分工：`ACCOUNT_STATUS` 面向策略/master 的三态聚合（Offline/LoggingIn/Ready）；本帧面向 UI 的细状态（11 态 + 登录信息 + 进度），按账户上报。

**实现注记**：契约先行——当前 td 上报的 `DZ_FRAME_TD_RTN_STATUS` payload 为扁平结构（`trading_day` 与 `api_version`/`sys_version` 在顶层、无 `ext`），且 dzweb 未消费；通用化 schema 由后续实现票落地。

---

## 1. 接口类型识别

帧 payload 格式由**接口类型**决定。接口类型从 `instance_id` 的网关名部分（`:` 之前）派生，**payload 中不携带类型字段**，派生算法与《帧契约：交易网关配置》§1 完全一致：

1. 取 `instance_id` 中第一个 `:` 之前的部分为网关进程名
2. 去掉 `dztd_` 前缀得到 tail；tail 为空时不是合法的交易进程名（退化情况，不处理）
3. tail 中第一个 `_` 之前的部分为 `interface_type`；不含 `_` 时 = tail 本身

| instance_id | 网关名 | interface_type |
|---|---|---|
| `dztd_ctp:account_001` | `dztd_ctp` | `ctp` |
| `dztd_ctp_2:account_001` | `dztd_ctp_2` | `ctp` |
| `dztd_xtp:acct_a` | `dztd_xtp` | `xtp` |

- **后台进程**：每个交易网关进程仅识别自身接口类型，按自身 schema 构造 payload。
- **dzweb**：按 `instance_id` 定位实例（镜像 key = `instance_id`，与总则 §9 一致），按派生出的接口类型选择前端卡片 schema。

---

## 2. CTP 类型（interface_type = `ctp`）

### 2.1 数据结构

#### TdStatus

交易网关账户级细状态（全量结构，RTN payload 用）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `account_id` | string | 账户标识（与 `instance_id` 冒号后半段一致；per-account 状态上报用） |
| `state` | string | 网关内部状态，取值见 2.2；与《帧契约：账户登录状态》三态聚合的映射由后者定义 |
| `date` | string | 会话交易日 `"YYYYMMDD"`（交易日，不是自然日）；未登录/进入 `Idle` 时为空串 |
| `login_time` | string | 登录时间 `"YYYY-MM-DD HH:MM:SS"`（服务器本地时间，由登录响应时间戳转换）；未登录/进入 `Idle` 时为空串 |
| `progress_desc` | string | 进度描述（展示文本，UTF-8）；无描述时为空串 |
| `progress_min` | int | 进度最小值（交易网关为 0） |
| `progress_max` | int | 进度最大值（交易网关为 10） |
| `progress_current` | int | 当前进度值 |
| `ext` | object | 柜台特有字段（CTP schema 见 2.3） |

> `progress_*` 与 `RTN_PROGRESS`（契约 progress）同源同值；本帧携带是历史兼容，前端进度展示以契约 progress 为唯一入口。

JSON 示例：

```json
{
  "account_id": "account_001",
  "state": "Ready",
  "date": "20260808",
  "login_time": "2026-08-08 08:45:32",
  "progress_desc": "ready",
  "progress_min": 0,
  "progress_max": 10,
  "progress_current": 10,
  "ext": {"api_version": "v6.7.2", "sys_version": "v6.7.2_20240105"}
}
```

### 2.2 状态枚举（`state`）

`state` 取值为交易网关内部状态机（11 态，值以发送方进程状态机定义为唯一真相源；CTP：`apps/ctp/td/td_state.h` 的 `TdState`，JSON 为枚举名字符串）：

| 值 | 说明 |
|---|---|
| `Idle` | 未启动/已登出，初始状态 |
| `Connecting` | API 已初始化，等待前置连接 |
| `Connected` | 前置已连接，准备认证 |
| `Authenticating` | 认证请求已发，等待响应 |
| `Authenticated` | 认证通过，准备登录 |
| `LoggingIn` | 登录请求已发，等待响应 |
| `LoggedIn` | 登录成功，准备确认结算单 |
| `Confirming` | 结算单确认已发，等待响应 |
| `LoadingInstruments` | 合约加载进行中 |
| `Ready` | 初始化完成，可下单可查询 |
| `Disconnected` | 网络断开，接口库自动重连中 |

三态聚合映射（Offline/LoggingIn/Ready）以《帧契约：账户登录状态》为准。

### 2.3 CTP `ext`

| 字段 | 类型 | 说明 |
|---|---|---|
| `api_version` | string | 接口库版本字符串，构造时设置一次 |
| `sys_version` | string | 登录响应中的系统版本；未登录/进入 `Idle` 时为空串 |

### 2.4 清空与保留语义

- **进入 `Idle`**（登出/手动停止）：`date`、`login_time` 清空，`api_version`/`sys_version` 按 2.3 清空（`api_version` 为进程生命周期常量可保留），`progress_*` 归零，推送。
- **登录失败/登录响应解析失败**：`login_time` 清空并推送（防止前端误显示过期登录信息）。
- **断线（`Disconnected`，接口库自动重连中）期间**：上述字段**不清空**——`date` 的语义是当前会话交易日（由登录响应确定，是本交易日全部业务数据的时间锚），断线不改变交易日；`sys_version`/`login_time` 同样保留，重连成功后由新登录响应覆盖。
- **日切**：会话交易日变化（含日切后重新登录）时以新 `date` 全量推送；前端以字段值变化为准，不自行推断清空时机。

---

## 3. DZ_FRAME_TD_RTN_STATUS

**语义**：交易网关上报账户级细状态（全量）
**数据流**：形态 4（总则 §4.2）——交易网关（帧头 `instance_id` = 网关名:账户ID）→ dzweb；无前端入口；镜像（key = `instance_id`）→ WS（映射见契约 webui-ws，随实现票同步）
**Payload**：JSON（TdStatus），**始终全量**

**触发场景**：

1. 状态或任何状态字段变化时推送（状态机 11 态翻转、登录信息填充/清空、`date` 变化、进度变化）
2. 交易网关启动初始化完成后按账户各上报一次
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：前端直接整体覆盖本地状态镜像
- 一个帧只对应一个账户（`instance_id` 含账户 ID，payload 冗余携带 `account_id` 供消费方定位）
- 账户被移除/网关退出前的最后一条状态由实现保证可观察（推 `Idle` 或停止推送，前端以在线集合（《帧契约：账户登录状态》）判断账户是否存在）
- 本帧非策略可见：策略 SDK 直接丢弃（《帧契约：策略》拦截清单）

**镜像与前端义务**：
- dzweb 按 `instance_id` 更新细状态镜像并 WS 推送；不进交易记录镜像
- 前端按 `instance_id`（= 网关名:账户ID）关联账户卡片；以全量覆盖更新，不自行推断字段清空时机；`date` 用于数据日期展示

---

## 4. 变更流程

本契约随交易网关状态实现演进；修改必须执行总则 §11.3 变更 checklist。
