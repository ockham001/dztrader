# 帧契约：交易账户连接控制

本文件覆盖 `DZ_FRAME_TD_CONNECT`、`DZ_FRAME_TD_DISCONNECT` 两个帧。总则见《帧契约：通用规则》。

两帧均使用 `DzExtInstFrameHeader` 扩展头，**进程级帧**：`instance_id` = 目标交易网关进程名（如 `"dztd_ctp"`）；账户身份在 payload `account_id`（账户 ID = 柜台真实投资者代码，全局唯一，总则 §5），帧头不承载账户。事件通道广播；目标交易网关按 `instance_id` 取帧后按 payload 账户处理。

> 为什么登录 / 登出指令是**进程级**而不是账户级：指令必须能路由到一个具体进程（谁去连柜台），而账户的归属网关只有注册表知道；用账户 ID 作 `instance_id` 会失去投递目标。**状态 / 进度**（谁现在是什么状态）才是账户级（契约 td-status / progress）。

**命名（ADR 0051）**：用户/前端动作名为「登录 / 登出」（REST 路径与按钮用词）；本组帧是传输层命令名，与动作一一对应：登录 → `DZ_FRAME_TD_CONNECT`，登出 → `DZ_FRAME_TD_DISCONNECT`。「连接 / 断开」只描述帧内部的传输阶段，不作动作名。

与行情侧对照（契约 md-subscription）：行情网关是「一进程一会话」，定向帧 payload 为空、`instance_id` 即目标进程；交易网关是「一进程多账户」，定向帧 payload 必须携带 `account_id`，`instance_id` 仍是网关进程名。两边的动作名、入口命名与进度端点规则一致（ADR 0051）。

类型层真相源：**本契约的 JSON schema**（无 platform 头文件；解析与处理在 `apps/ctp/td/td_api.cpp`）。

---

## 数据结构

### AccountSessionReq

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `account_id` | string | 是 | 目标交易账户 ID（柜台真实投资者代码），非空；全局唯一（ADR 0045 / ADR 0056） |

```json
{"account_id": "00001"}
```

---

## DZ_FRAME_TD_CONNECT

**语义**：建立指定账户的网关会话（登录）：为目标账户创建会话并重走完整登录链（连接 → 认证 → 登录 → 结算确认 → 合约加载 → 初始查询 → `Ready`）。

**数据流**：形态 3（总则 §4.2）——UI → dzweb → 目标交易网关；前端入口 `POST /api/td/accounts/{account_id}/login`（契约 rest §2.9.10）与 WS `td_connect`（契约 webui-ws §3，两路等效）；无 RTN，结果经该账户的 `RTN_PROGRESS`（契约 progress，`instance_id` = 账户 ID）与 `RTN_TD_STATUS`（契约 td-status）体现（ack 仅表示已写通道）。目标网关在任意状态字段变化时主动推送这两帧（契约 td-status §3 触发场景 1），不是只在快照/配置变更时推送。

**Payload**：JSON（AccountSessionReq）

**时序与触发**：

- 前端发起 → dzweb 预检（admin、账户已登记、目标网关 Running、写通道可用）并写帧 → 回 ack（`ok=false` = 未写通道）→ 目标网关串行处理 → 进度/状态推送。
- 目标账户已有会话：幂等跳过（不重建会话、不产生新登录链、不推送）。
- 登录失败（柜台拒绝 / 交易日无效 / 基准装载失败 / 登录超时）：状态回滚到中间态并停住，本进程**不自动重试**（#406）；失败原因经 `NOTIFY_UI` 错误级弹窗上浮（#407 / #408）。恢复路径 = 先登出、再登录（ADR 0051）。
- 登录链后段失败（结算确认被拒 / 结算确认 60 秒无响应 / 合约查询被拒或超时 / 合约加载失败）：同样回滚到 `LoggedIn` 并停住、不自动重试，失败原因同样经 `NOTIFY_UI` 错误级弹窗上浮（#488）。**注意进度由此从 8/10（或 7/10）退回 6/10「已登录」**——这与「登录成功、正在初始化」是同一个状态，消费方不得据此推断登录成功，须以 `RTN_PROGRESS.current == progress.max` 为就绪判据。
- 登录成功判据：该账户 `RTN_PROGRESS.current == progress.max`（契约 progress「网关会话进度骨架」）。

**约束**：

- 定向帧，仅 `instance_id` 匹配的交易网关处理。
- 前置条件拒绝（账户不存在 / 已禁用 / 无可用前置地址 / 当前状态不接受连接）：不建会话、**不产生 RTN**；用户请求路径经 `NOTIFY_UI` 错误级弹窗说明原因，自动调度 / `--recover` 补登路径只记日志（避免非用户动作弹窗，对齐行情侧）。
- 校验失败（`account_id` 缺失或空串、JSON 解析失败）：丢弃并只记 WARN（dzweb 入口已守卫，正常不可达；不推 RTN、不弹通知）。
- 无 RTN：不得以 ack 判定登录成败；前端 pending 清除规则见「前端义务」。

---

## DZ_FRAME_TD_DISCONNECT

**语义**：结束指定账户的网关会话（登出）：释放柜台 API、销毁会话、推送 Offline。任意状态可发（连接中 / 登录中 / 断线重连中 / 失败态 / 就绪），即使用户在登录过程中发起，也立即回到未登录——这是卡死状态的强制中断动作。

**数据流**：同 `DZ_FRAME_TD_CONNECT`（形态 3）；前端入口 `POST /api/td/accounts/{account_id}/logout`（契约 rest §2.9.10）与 WS `td_disconnect`（契约 webui-ws §3）。

**Payload**：JSON（AccountSessionReq）

**时序与触发**：

- 目标账户无会话：no-op（不报错、不推送）。
- 有会话：释放 CTP API、推送 `DZ_FRAME_ACCOUNT_STATUS` Offline（契约 account-status）、推送 `RTN_TD_STATUS`（`state=Idle`）与 `RTN_PROGRESS`（`current=min`）、销毁会话。
- 结果判据：该账户 `RTN_PROGRESS` 回到 `current == progress.min`。

**约束**：

- 定向帧，仅 `instance_id` 匹配的交易网关处理。
- 校验失败（`account_id` 缺失或空串）：同 `TD_CONNECT`（丢弃、只记 WARN）。
- 不发送柜台登出请求：当前实现直接释放 API（与行情侧一致，《帧契约：行情连接与订阅》同语义）；「登出」不承诺优雅收尾。

---

## 校验（仅写与总则不同的规则）

- 两帧均为 JSON payload；`null` 出现在任何位置均非法（总则 §8）。
- `account_id` 必填、非空字符串；未知字段忽略（总则 §8）。
- 无 RTN，不适用总则 §8 四件套；失败按上文「前置条件拒绝 / 校验失败」处理。

---

## 镜像

- 不新增镜像域：`progress`（key = 账户 ID，契约 progress）与 `td_status`（同 key，契约 td-status）已覆盖会话状态；dzweb 不缓存会话控制请求的结果。
- 账户集合与启用状态以账户注册表 / `td_config` 镜像为准（契约 td-config §5），本组帧不改变配置。

---

## 前端义务

> 按钮可见性/位置/disabled 的视觉规则是前端设计决策，见 ADR 0051；本节只写跨进程可观察的协议义务（契约侧只约束消息触发与判据，不约束控件形态）。

1. **会话态判据**：以 `RTN_PROGRESS` 端点为唯一判据——`current <= min` = 未登录、`current >= max` = 就绪、其余 = 建立中（连接中/登录中/重连中/失败回滚）。`progress` 未到达时可按 `RTN_TD_STATUS.state` 兜底：`Idle` = 未登录、`Ready` = 就绪、其余 = 建立中。规则实现见 ADR 0051。
2. **pending 清除**：登录 pending 的清除信号 = `RTN_PROGRESS` 到达 `current == max`、`current` 相对上次推送回退（进度骨架：失败/中止会回退）、或兜底超时（建议 45s，连接 30s + 登录 10s + 余量）；登出 pending = `current == min` 或兜底超时（建议 10s）。端点到 `min`/`max` 时清掉对应在途请求（登录中点登出 → 到 `min` 时登录与登出 pending 都清）。`NOTIFY_UI` 是纯通知，**永不清除 pending**（契约 notify-ui）。
3. **失败展示**：`NOTIFY_UI` 错误级弹窗即时提示；`RTN_TD_STATUS` 的 `last_error` 持久化在后续票（#427）落地前不存在，前端不得依赖该字段做判定。
4. **权限与禁止发起的条件**：仅 admin 可发起；目标网关未 Running 时两动作都不可发起（无法写帧）。账户已禁用时**登录**不可发起（网关会拒），**登出不受启用状态影响**——禁用不强制断开已有会话，仍需可登出（契约 td-config 启用/禁用语义）。dzweb 守卫权威，前端禁用只是体验优化（守卫失败经 WS `error` / REST 错误体表达，不写帧）。
5. **不以 ack / HTTP 200 判定结果**：两者只表示已写事件通道；成败只看 `progress` / `td_status`。

---

## 变更流程

本契约随交易网关会话控制实现演进；修改必须执行总则 §11.3 变更 checklist。
