# 帧契约：TD 刷新请求

本文件覆盖 `DZ_FRAME_TD_REFRESH_REQ` 一个帧。总则见《帧契约：通用规则》。

类型层真相源：**本契约**（JSON schema 与校验语义）；帧号在 `libs/core/include/dztrader/core/core_data_type.h` 登记（实现票落盘）。前端入口见契约 rest §2.9.11。


## DZ_FRAME_TD_REFRESH_REQ

**语义**：请求目标交易网关对指定账户做一次 CTP 资金查询（`ReqQryTradingAccount`）。只置脏，**不保证立即发送**（受账户查询通道的在途 / 最小间隔约束，见《帧契约：TD 数据同步》§4.1），也不保证查询成功；无专用 RTN 帧。

**数据流**：形态 3（总则 §4.2）——dzweb → td（事件通道广播；`DzExtFrameHeader` 扩展头，**无 `instance_id`**；接收方按 payload `account_id` 过滤）。生效信号 = 该账户新的 `TRADING_ACCOUNT` 帧（《帧契约：TD 数据同步》§1 / §4.1）经 dzweb 广播的 WS `td_trading_account`（契约 webui-ws §2.5）。

**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `account_id` | string | 是 | 目标交易账户 ID（柜台真实投资者代码，全局唯一，总则 §5）；空串 / 未配置账户 → 接收方忽略并记 WARN |
| `request_id` | int | 否 | 调用方追踪 ID（日志关联用，原样记录、不回传）；由 dzweb 生成，当前固定 0 = 不追踪 |

```json
{"account_id": "00001", "request_id": 42}
```

**时序与触发**：用户在资金面板点击该行「刷新」→ REST（契约 rest §2.9.11）→ dzweb 写本帧 → td 置脏（受查询通道合并 / 限速）→ 查询完成后写 `TRADING_ACCOUNT` 帧 → dzweb 广播 WS `td_trading_account` → 前端按同账户 `seq` 前进清除该行 pending。

**约束**：

- **只对 Ready 账户受理**；非 Ready / 未配置账户忽略（REST 预检已挡一遍，td 侧为其兜底），不写 RTN、不改账户状态。
- **一次请求最多对应一次额外查询**；连续多次请求在查询通道的最小间隔窗口内合并（不排队累加），不放大查询次数。
- 查询受同一账户查询通道约束：同一时刻至多 1 笔在途、任意两笔 `ReqQry*` 发送间隔 ≥ `qry_min_interval_ms`（默认值见契约 td-config §2.4），因此**不保证**请求后 N ms 内发出。
- 失败不产生专用 RTN：后台记 WARN + 限频 ERROR 通知（每账户 5 分钟，`popup=false`）；用户可见的失败由前端 pending 超时提示承接（契约 webui-ws §5）。
- 本帧不写交易库、不改变账户状态、不影响持仓 / 委托 / 成交链路；策略侧不可见（策略 SDK 按拦截清单丢弃）。

## 校验

- payload 必须为 JSON object；`account_id` 缺失 / 空串 / 非 string、`request_id` 非整数 → 整帧忽略并记 WARN（不写 RTN、不改状态）。
- 未知字段忽略（总则 §8）。
- 接收方只处理自己配置内的 `account_id`；不在配置内 → 忽略并记 WARN（REST 预检按注册表路由，这里是兜底）。

## 镜像

本帧**不进镜像**：dzweb 不保留交易记录镜像；查询结果经既有 `TRADING_ACCOUNT` 帧 → WS `td_trading_account`（契约 webui-ws §2.5）表达，前端按同账户 `seq` 合并。

## 前端义务

- 点击后该行 pending；收到同账户 `td_trading_account` 且 `seq` 前进后清除；3s 超时清除并提示「刷新超时，稍后自动更新」。
- 离线（非 Ready）行不得发起刷新（REST 会 409）；重复点击 / 多客户端并发在 td 侧按最小间隔合并，不放大查询次数。
