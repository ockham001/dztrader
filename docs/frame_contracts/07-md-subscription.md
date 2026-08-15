# 帧契约：行情连接与订阅

本文件覆盖 `DZ_FRAME_REQUEST_MD_CONNECT`、`DZ_FRAME_REQUEST_MD_DISCONNECT`、`DZ_FRAME_REQUEST_MD_SUBSCRIBE`、`DZ_FRAME_QUERY_MD_SUBSCRIPTIONS`、`DZ_FRAME_RTN_MD_SUBSCRIPTIONS`、`DZ_FRAME_NOTIFY_MD_STARTED`、`DZ_FRAME_NOTIFY_MD_CONNECTED`、`DZ_FRAME_NOTIFY_MD_DISCONNECTED`、`DZ_FRAME_NOTIFY_MD_STOPPED` 九个帧。总则见《帧契约：通用规则》。

全部帧使用 `DzExtInstFrameHeader` 扩展头，`instance_id` = 行情进程名（定向帧为目标进程，广播帧为来源进程）。

类型层真相源：
- 订阅状态管理与查询：`libs/platform/include/dztrader/platform/subscription_manager.h`（`SubState`/`SubscriptionDetail`/`RtnMdSubscriptionsRsp`/`SUBSCRIPTION_QUERY_MAX`）
- 订阅请求：`libs/core/include/dztrader/core/core_struct.h`（`SubscribeReq`/`SubscribeAction`）

---

## 数据结构

### SubState

订阅状态三态。JSON 值与枚举名一致（首字母大写）。

| 值 | 说明 |
|---|---|
| `NotRequested` | 未发送订阅请求 |
| `Pending` | 已发送，待确认 |
| `Subscribed` | 确认订阅成功 |

**失败语义**：确认失败不引入失败态——失败且无订阅者时条目删除；失败且仍有订阅者时保持 `Pending`，由补订链路（`sub_check_interval_ms`/`sub_max_retry`，见《帧契约：行情网关配置》）重试。查询侧 `unsuccessful` 模式即覆盖 Pending + NotRequested。

### SubscriptionDetail

`RTN_MD_SUBSCRIPTIONS` 响应元素。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `instrument` | string | 是 | 合约代码 |
| `sub_state` | SubState | 是 | 订阅状态 |
| `subscribers` | array\<string\> | 是 | 订阅者 `instance_id` 列表 |

### 截断常量

`SUBSCRIPTION_QUERY_MAX = 32`：查询返回条数上限，编译期固定常量，不可配置，前后端共用。超过时截断并置 `truncated=true`，`total_matched` 反映实际匹配总数。

---

## DZ_FRAME_REQUEST_MD_CONNECT / DZ_FRAME_REQUEST_MD_DISCONNECT

**语义**：请求指定行情进程建立 / 断开连接（登录 / 登出）
**数据流**：形态 3（总则 §4.2）——UI → dzweb → 目标行情进程（帧头 `instance_id` = 行情进程名）；前端入口 `POST /api/market-sources/{id}/login|logout`（契约 11）或 WS `md_connect`/`md_disconnect`（契约 10，两路等效）；无 RTN，结果经该进程 `RTN_PROGRESS` 与 `NOTIFY_MD_*` 健康度广播体现（ack 仅表示已写通道）
**Payload**：空

**时序**：前端发起 → dzweb 对前端回 ack（仅表示已写入事件通道，**不表示连接结果**；写通道失败时 `ok=false`，见契约 10）→ **无 RTN**

**约束**：
- 定向帧，仅 `instance_id` 匹配的行情进程处理
- 连接结果通过该行情进程的 `RTN_PROGRESS`（状态数值映射，契约 06）与健康度广播（下方 NOTIFY_MD_*）体现；前端不得以 ack 判定连接成败

---

## DZ_FRAME_REQUEST_MD_SUBSCRIBE

**语义**：请求目标行情进程对某订阅者执行订阅/退订操作
**数据流**：形态 5（总则 §4.2）——订阅方进程（策略/数据存储等）→ 目标 md 进程（帧头 `instance_id` = 行情进程名，订阅者身份在 payload `instance_id`）；无前端入口（策略侧 API 已封装）；无 RTN，结果经订阅状态查询间接体现
**Payload**：JSON（`SubscribeReq`，类型真相源 `core_struct.h`）

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `instance_id` | string | 是 | 订阅者身份（策略实例 ID） |
| `action` | SubscribeAction | 是 | `0`=Subscribe / `1`=Unsubscribe / `2`=UnsubscribeAll（数字枚举） |
| `replace` | bool | 是 | 仅 `Subscribe` 时有效：`true` 先退订该订阅者的全部旧合约再订阅新列表（全量替换） |
| `instruments` | array\<string\> | 是 | 合约列表（`UnsubscribeAll` 时忽略） |

**约束**：
- 定向帧，仅 `instance_id` 匹配的行情进程处理
- 无 RTN；执行结果通过订阅状态查询（下方 QUERY/RTN）间接体现
- 策略侧 API 已封装（`dz_subscribe` / `dz_unsubscribe`），调用方无需直接构造帧

---

## DZ_FRAME_QUERY_MD_SUBSCRIPTIONS

**语义**：查询目标行情进程的订阅详情
**数据流**：形态 1（总则 §4.2）——dzweb → 目标 md 进程（帧头 `instance_id` = 行情进程名）；前端入口 WS `query_md_subscriptions`（契约 10；REST 未提供）；响应帧 `RTN_MD_SUBSCRIPTIONS` → 不进镜像 → WS 消息 `md_rtn_subscriptions`
**Payload**：JSON，两种模式互斥

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `query` | string | 二选一 | 查询模式，目前仅支持 `"unsuccessful"`（未成功：Pending + NotRequested） |
| `instruments` | array\<string\> | 二选一 | 按合约列表查 |

```json
{"query": "unsuccessful"}
{"instruments": ["IF2506", "IC2506", "rb2510"]}
```

**时序**：前端发起 → 必回 `RTN_MD_SUBSCRIPTIONS`（总则 §7 兜底适用）

**约束**：
- `query` 与 `instruments` 互斥，同时出现校验失败（经 RTN `error` 表达，见下）
- `query` 目前仅支持 `"unsuccessful"`，其他值校验失败
- `instruments` 中非字符串元素跳过（不报错、不计入 `total_matched`）；不存在的合约也计入 `total_matched`，返回 `NotRequested` + 空 `subscribers`

---

## DZ_FRAME_RTN_MD_SUBSCRIPTIONS

**语义**：行情进程推送订阅详情查询结果
**数据流**：形态 4"不进镜像"变体（总则 §4.2）——md 进程（帧头 `instance_id` = 行情进程名）→ dzweb 注入 `source` 后透传 → 前端；无独立前端入口（仅由查询触发）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `subscriptions` | array\<SubscriptionDetail\> | 是 | 详情列表（可能截断） |
| `returned_count` | int | 是 | 实际返回条数（`subscriptions.size()`） |
| `total_matched` | int | 是 | 匹配总数（截断前） |
| `truncated` | bool | 是 | 是否因超 `SUBSCRIPTION_QUERY_MAX` 截断 |
| `error` | string \| null | 是 | 错误路径填充错误码，成功路径为 `null` |

成功路径示例：

```json
{
  "subscriptions": [
    {"instrument": "IF2506", "sub_state": "Pending", "subscribers": ["stg.alpha"]},
    {"instrument": "IC2506", "sub_state": "NotRequested", "subscribers": ["stg.beta"]}
  ],
  "returned_count": 2,
  "total_matched": 5,
  "truncated": true,
  "error": null
}
```

**触发场景**：`QUERY_MD_SUBSCRIPTIONS` 的响应

**约束**：
- 成功路径排序：`unsuccessful` 模式下 Pending 优先、NotRequested 次之；`instruments` 模式按请求顺序
- 错误路径：仅填充 `error`，其余字段为默认值（空列表/0/false）；错误码取值：`bad_json` / `missing_query_or_instruments` / `ambiguous_query` / `unknown_query`
- **不进镜像**：查询响应，dzweb 注入 `source` 后透传广播（见契约 10）；前端数据以本 RTN 为准

**前端义务**：
- 查询后进入 pending，以对应实例的 `RTN_MD_SUBSCRIPTIONS` 推送清除（`error=null` 为成功数据，`error` 非 null 为查询失败并展示错误）；ack 仅用于写通道失败的快速反馈

---

## 行情服务生命周期通知

四个帧（`DZ_FRAME_NOTIFY_MD_STARTED` / `DZ_FRAME_NOTIFY_MD_CONNECTED` / `DZ_FRAME_NOTIFY_MD_DISCONNECTED` / `DZ_FRAME_NOTIFY_MD_STOPPED`）为行情服务的后台生命周期广播，均为**形态 5（总则 §4.2）**：流向策略/数据存储等订阅进程，供其维护行情通道的开启/关闭；无前端入口、无 RTN、不进镜像。均为空 payload，`instance_id` = 行情进程名。

**健康度定义**：行情健康度为二元（Up/Down），**由登录态决定**——进入 `LoggedIn` → Up，离开 `LoggedIn`（断线、登出、登录失败等任何原因）→ Down。健康度与订阅状态无关（订阅失败由补订机制处理，不翻转健康度）。仅在健康度翻转时广播，避免重复。

**两路一致**：UI 侧状态（`RTN_PROGRESS` 的数值映射）与后台侧健康度由同一状态机驱动，禁止两路不一致（进入可交易态时必须既推 `RTN_PROGRESS` 又广播 `NOTIFY_MD_CONNECTED`，离开时同理）。

> 与 UI 状态的关系：本组帧仅服务后台订阅进程，前端 UI 不消费。前端判断行情进程的运行/停止状态以《帧契约：进程》的 `RTN_PROCESS_STATUS` / `RTN_PROCESS_CONFIG` 为准；判断登录/健康细粒度状态以 `RTN_PROGRESS`（契约 06）为准。

### DZ_FRAME_NOTIFY_MD_STARTED

**语义**：某行情服务已启动就绪，可对其行情通道发起订阅
**数据流**：形态 5——md → 广播（`instance_id` = 行情进程名）
**触发场景**：行情进程启动初始化完成后广播一次

**约束**：仅后台进程消费（可重订阅/打开对应行情通道），dzweb / 前端不处理

### DZ_FRAME_NOTIFY_MD_CONNECTED

**语义**：某行情服务已登录（可交易），健康度由 Down 翻转为 Up
**数据流**：形态 5——md → 广播（`instance_id` = 行情进程名）
**触发场景**：进入 `LoggedIn`（登录成功）时广播

**约束**：仅后台进程消费；仅在健康度翻转时发送

### DZ_FRAME_NOTIFY_MD_DISCONNECTED

**语义**：某行情服务已断开/不可用（不可交易），健康度由 Up 翻转为 Down
**数据流**：形态 5——md → 广播（`instance_id` = 行情进程名）
**触发场景**：离开 `LoggedIn`（断线、登出、被服务器登出等任何原因）时广播

**约束**：仅后台进程消费；仅在健康度翻转时发送

### DZ_FRAME_NOTIFY_MD_STOPPED

**语义**：某行情服务已停止，应关闭对应行情通道
**数据流**：形态 5——master → 广播（`instance_id` = 被停止的行情进程名；进程退出无法自行发 `DISCONNECTED`，由 master 代发并隐含其语义）
**触发场景**：行情进程退出时由 master 广播（崩溃时代发 + 正常停止时通知）

**约束**：
- `instance_id` 标识被停止的行情源，接收方据此关闭对应行情通道
- **隐含 `NOTIFY_MD_DISCONNECTED` 语义**：进程退出时无法自行发送 `DISCONNECTED`，接收方收到 `STOPPED` 后应同时执行 `DISCONNECTED` 的清理逻辑（标记不可交易）
- 仅后台进程消费，dzweb / 前端不处理，不据此判断行情进程状态
