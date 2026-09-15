# 帧契约：出入金与改密

本文件覆盖 `DZ_FRAME_TD_TRANSFER_REQ`、`DZ_FRAME_TD_TRANSFER_RSP`、`DZ_FRAME_TD_TRANSFER_RTN`、`DZ_FRAME_TD_PASSWORD_UPDATE_REQ`、`DZ_FRAME_TD_PASSWORD_UPDATE_RSP` 五个帧。总则见《帧契约：通用规则》。

五帧均使用 `DzExtFrameHeader` 扩展头（**无 `instance_id`**），事件通道广播；接收方按 payload `account_id` 过滤（账户 ID 全局唯一，总则 §5）。发送方身份/目标不在帧头承载。

类型层真相源：`libs/platform/include/dztrader/platform/td_account_ops.h`。

**实现状态（未接线）**：td 仅产出 `TRANSFER_RSP`/`TRANSFER_RTN`/`PASSWORD_UPDATE_RSP`；`TRANSFER_REQ`/`PASSWORD_UPDATE_REQ` 无生产者（td 未实现对应 CTP 请求调用）；且 td 当前仅接线银行→期货方向（`OnRsp`/`OnRtnFromBankToFutureByFuture`），`trade_code="202002"`（期货→银行）无响应处理，接线时需补对应 SPI 回调。dzweb 无消费、无镜像 domain、无 WS 消息、无 REST 端点、前端无入口。本契约先定义语义，实现滞后按总则 §11.3 checklist 跟踪。

---

## 数据结构

### TransferReq

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `account_id` | string | 是 | 目标交易账户 ID |
| `trade_code` | string | 是 | CTP 交易代码：`"202001"`=银行→期货，`"202002"`=期货→银行，`"204002"`=查询 |
| `bank_id` | string | 否 | 银行代码 |
| `bank_account` | string | 否 | 银行账号 |
| `bank_password` | string | 否 | 银行密码（敏感，见"安全约束"） |
| `future_password` | string | 否 | 期货资金密码（敏感，见"安全约束"） |
| `currency_id` | string | 否 | 币种，如 `"CNY"` |
| `trade_amount` | number | 是 | 金额 |
| `cust_type` | int | 否 | 0=个人，1=机构 |
| `request_id` | int | 否 | 请求追踪 ID |

```json
{
  "account_id": "account_001",
  "trade_code": "202001",
  "bank_id": "1",
  "bank_account": "6222020200112233",
  "bank_password": "******",
  "future_password": "******",
  "currency_id": "CNY",
  "trade_amount": 10000.0,
  "cust_type": 0,
  "request_id": 42
}
```

### TransferRsp

`TRANSFER_RSP`（`OnRsp` 即时响应）与 `TRANSFER_RTN`（`OnRtn` 银行权威结果）共用本 schema。

| 字段 | 类型 | 说明 |
|------|------|------|
| `account_id` | string | 目标交易账户 ID |
| `trade_code` | string | 原样回传请求的交易代码 |
| `error_id` | int | 0=成功；非 0 为 CTP 错误码 |
| `error_msg` | string | 错误描述（UTF-8；成功为空串） |
| `bank_balance` | number | CTP 出入金字段无银行余额，恒 0 |
| `trade_amount` | number | 金额 |
| `transfer_status` | string | CTP `TransferStatus` 原始单字符，值语义以 CTP 官方文档为准；空串=CTP 未返回（`'\0'`） |
| `time` | int | 距午夜秒数；-1=CTP `TradeTime` 缺失/非法，0=午夜 |

```json
{
  "account_id": "account_001",
  "trade_code": "202001",
  "error_id": 0,
  "error_msg": "",
  "bank_balance": 0.0,
  "trade_amount": 10000.0,
  "transfer_status": "0",
  "time": 34200
}
```

### PasswordUpdateReq

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `account_id` | string | 是 | 目标交易账户 ID |
| `password_type` | string | 是 | `"U"`=登录密码，`"A"`=资金密码 |
| `old_password` | string | 是 | 旧密码（敏感，见"安全约束"） |
| `new_password` | string | 是 | 新密码（敏感，见"安全约束"） |
| `currency_id` | string | 否 | 币种，资金密码场景使用 |

```json
{
  "account_id": "account_001",
  "password_type": "U",
  "old_password": "******",
  "new_password": "******",
  "currency_id": "CNY"
}
```

### PasswordUpdateRsp

| 字段 | 类型 | 说明 |
|------|------|------|
| `account_id` | string | 目标交易账户 ID |
| `password_type` | string | 原样回传：`"U"`=登录密码，`"A"`=资金密码 |
| `error_id` | int | 0=成功；非 0 为 CTP 错误码 |
| `error_msg` | string | 错误描述（UTF-8；成功为空串） |
| `time` | int | CTP 无时间字段，恒 0 |

```json
{
  "account_id": "account_001",
  "password_type": "U",
  "error_id": 0,
  "error_msg": "",
  "time": 0
}
```

---

## DZ_FRAME_TD_TRANSFER_REQ

**语义**：发起出入金请求
**数据流**：形态 1（总则 §4.2）——dzweb → td（payload `account_id` 路由）；前端入口与 dzweb 发送均**未接线**
**Payload**：JSON（TransferReq）
**时序与触发**：用户发起 → td 调用 CTP 请求；响应分两段：`TRANSFER_RSP` 即时返回受理/拒绝结果，`TRANSFER_RTN` 在银行异步结果到达后返回（可能缺失）

---

## DZ_FRAME_TD_TRANSFER_RSP / DZ_FRAME_TD_TRANSFER_RTN

**语义**：出入金即时响应（RSP）/ 银行权威结果（RTN）
**数据流**：形态 4（总则 §4.2）——td → dzweb；消费、镜像 domain、WS 消息均**未接线**
**Payload**：JSON（TransferRsp）
**时序与触发**：
- RSP：td 收到 CTP `OnRsp` 时写，每个被受理的 REQ 必回一条；仅表示"已受理"，不代表银行处理成功
- RTN：td 收到 CTP `OnRtn` 时写，携带银行权威结果；同一笔请求可能缺失
- 最终状态以 RTN 的 `error_id`/`transfer_status` 为准；前端 pending 清除与 RTN 修正语义在接线时按此实现

---

## DZ_FRAME_TD_PASSWORD_UPDATE_REQ

**语义**：修改登录密码（`password_type="U"`）或资金密码（`password_type="A"`）
**数据流**：形态 1（总则 §4.2）——dzweb → td（payload `account_id` 路由）；前端入口与 dzweb 发送均**未接线**
**Payload**：JSON（PasswordUpdateReq）
**时序与触发**：用户发起 → td 调用 CTP 请求（`ReqUserPasswordUpdate` / `ReqTradingAccountPasswordUpdate`）

---

## DZ_FRAME_TD_PASSWORD_UPDATE_RSP

**语义**：修改密码响应
**数据流**：形态 4（总则 §4.2）——td → dzweb；消费、镜像 domain、WS 消息均**未接线**
**Payload**：JSON（PasswordUpdateRsp）
**时序与触发**：td 收到 CTP `OnRsp` 时写；无异步 RTN

**镜像与前端义务**：未接线——dzweb 镜像 domain、WS 消息、REST 端点与前端 pending/RTN 修正语义均未建立（见"实现状态"）

---

## 校验

- `null` 出现在任何位置均非法；未知字段忽略（总则 §8）
- 解码行为提示：`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT` 对缺失字段回退默认值、对整体 payload 为 `null` 解码为全默认结构（不抛错）；接线时的 handler 必须按本节显式校验（`account_id` 非空、`password_type` 取值等），不得依赖解码错误
- TransferReq：`account_id`/`trade_code`/`trade_amount` 必填；`trade_code` 取值为 `"202001"`/`"202002"`/`"204002"`
- PasswordUpdateReq：`account_id`/`password_type`/`old_password`/`new_password` 必填；`password_type` ∈ {`"U"`, `"A"`}
- 本组帧非设置类可回滚请求，不适用总则 §8 四件套；失败经对应 RSP/RTN 的 `error_id`/`error_msg` 传达

## 安全约束

- `bank_password`/`future_password`/`old_password`/`new_password` 在事件通道 SHM 内明文传输（与本地 IPC 现状一致）；各进程禁止将 payload 原文落日志，禁止 WS/REST 回显
- RSP/RTN 不含任何密码字段
