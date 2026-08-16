# WebSocket 协议：frontend ↔ dzweb

本文件规定 WebUI 前端与 dzweb 后端之间的 WebSocket 消息协议（type + payload 结构 + 校验 + 触发场景），与各帧契约同构。总则见《帧契约：通用规则》。

> 本文件覆盖的是**进程间契约投射到 WS 的最终形态**：子进程通过 SHM 上报 RTN 帧 → dzweb 领域服务更新镜像 + WS 推到前端。因此大量消息与对应帧契约的 payload 一致，本文件只描述 WS 信封与差异，payload 字段细节以对应帧契约为准。
>
> 链路定位（总则 §4.2）：§2.2 领域消息 = 形态 1 响应侧 / 形态 4 的 dzweb 出口；§3 C2S 控制消息 = 形态 1/3 的 WS 前端入口（与 REST 等效）。

---

## 1. 连接与鉴权

- 端点：`/ws?token=<jwt>`（也接受 `Authorization: Bearer <jwt>` 头）。
- 鉴权失败：服务端回 `{"type":"error","payload":{"message":"auth failed"}}` 并关闭连接。
- 建连成功：服务端立即推送**全量镜像快照** `snapshot`（连接/重连的初始基线），之后以增量消息更新。
- 心跳：客户端每 30s 发 `{"type":"ping"}`，服务端回 `{"type":"pong"}`（无载荷）。服务端不因心跳缺失而踢连接。
- 多客户端：同用户可多连接；领域消息对所有已连接客户端广播；`snapshot`/`default_password_warning`/`*_ack`/`error`/`log_line`/`log_tail_unsubscribed`/`pong` 不广播（按连接/定向发送）；`data_changed` 对所有客户端广播。
- 服务端**不仲裁并发请求**：多个客户端发出的请求按到达顺序由 dzweb 串行写帧，由目标进程串行应用（总则 §7）。

### 信封格式

| 键 | 说明 |
|---|---|
| `type` | 消息类型（见 §2/§3） |
| `data` | 领域消息载荷（广播帧专用） |
| `payload` | 控制消息载荷（C2S 请求、`*_ack`/`error`/`log_line`/`log_tail_unsubscribed`/`data_changed`/`default_password_warning`） |
| `instance_id` | 可选；领域消息的实例归属（`log_config`/`md_shm_config`/`auto_login`/`progress`）；为空时不携带该字段（`event_shm_config` 等挂 `dztraderd` 的消息为空，见 §2.2 表） |
| `seq` | 可选；C2S 消息携带时由 `*_ack`/`error` 回填 |

**`data` 与 `payload` 互斥**：领域消息用 `data`，控制消息用 `payload`，勿混用。

---

## 2. 服务端 → 客户端（S2C）

### 2.1 `snapshot`

**语义**：连接/重连时服务端推送的全量镜像快照，作为初始基线。
**触发**：每次鉴权成功建连后。
**载荷（`data`）**：`MirrorStore` 全量，结构 `{ [instance_id]: { [domain]: payload } }`。

domain 清单与挂载实例：

| domain | 挂载实例 | payload 结构（= 对应契约的 RTN payload） | 对应契约 |
|---|---|---|---|
| `log_config` | 各进程（含 `dztraderd` 自身） | `{ level, flush_on }` | log |
| `event_shm_config` | 固定 `dztraderd` | SHM 事件通道配置 | shm |
| `md_shm_config` | 各行情进程 | SHM 行情通道配置 | shm |
| `process_config` | 固定 `dztraderd` | `{ [进程名]: config }` 全量映射 | process |
| `process_status` | 各进程 | `{ name, state, pid, ... }` | process |
| `auto_login` | 各行情进程 | `{ enabled, schedules }` | auto-login |
| `progress` | 各进程（TD 为 `{进程名}:{账户ID}`） | `{ min, max, current, desc }` | progress |
| `md_config` | 各行情进程 | 脱敏行情配置 | md-config |
| `md_status` | 各行情进程 | 行情网关状态 | md-status |

> 注：
> - `md_rtn_subscriptions`（契约 md-subscription）为查询响应，**不进镜像**，故不在快照中。
> - **快照不承诺完整性**：snapshot 始终是"建连时刻 dzweb 已知镜像"（总则 §7）；尚未上报的进程域由后续增量消息补齐。
> - **快照与增量形状不同**：快照按 `instance_id`/`domain` 嵌套、domain 值为纯 payload；增量消息（§2.2-2.8）多数把 `source`/`name` 内嵌在 payload 中且 `instance_id` 为空。前端为快照与增量各自维护 apply 逻辑（见 §5）。

### 2.2 领域消息（广播帧，`data` 键）

| type | 来源帧 | 载荷（`data`） | `instance_id` | 对应契约 |
|---|---|---|---|---|
| `process_status` | `RTN_PROCESS_STATUS` | `{ name, state, pid, message, display_name, event }`；`event` 恒出现，`null` = 自发状态变化；`state` PascalCase。**形状注**：快照（§2.1）中该域值按帧层 RTN 序列化（`event` 缺失、空 `message`/`display_name` 省略），与增量消息的 `event:null` 形状不同；前端对缺失与 null 均按自发状态处理 | 空（`name` 在 payload 内） | process |
| `process_config` | `RTN_PROCESS_CONFIG` | `{ [进程名]: config }` 全量映射，整体覆盖（条目消失 = 进程已移除） | 空 | process |
| `md_rtn_config` | `RTN_MD_CONFIG` | `{ source: 进程名, config: 脱敏配置 }` | 空（`source` 在 payload 内） | md-config |
| `md_rtn_status` | `RTN_MD_STATUS` | `{ source: 进程名, status: 网关状态 }` | 空（`source` 在 payload 内） | md-status |
| `md_rtn_subscriptions` | `RTN_MD_SUBSCRIPTIONS` | 订阅查询结果，服务端注入 `source` 字段后整体透传 | 空 | md-subscription |
| `notify_ui` | `NOTIFY_UI` | 原 payload 整体透传 `{ source, level, message, timestamp, popup }` | 空 | notify-ui |
| `log_config` | `RTN_LOG_CONFIG` | `{ level, flush_on }`，始终全量 | 目标/来源进程名 | log |
| `event_shm_config` | `RTN_EVENT_SHM_CONFIG` | SHM 事件通道配置 | 空（镜像挂 `dztraderd`） | shm |
| `md_shm_config` | `RTN_MD_SHM_CONFIG` | SHM 行情通道配置 | 行情进程名 | shm |
| `auto_login` | `RTN_AUTO_LOGIN` | `{ enabled, schedules }`；dzweb 读取时先校验，非法则记日志并忽略（不更新镜像、不广播） | 行情进程名 | auto-login |
| `progress` | `RTN_PROGRESS` | `{ min, max, current, desc }` | 进程名 / `{进程}:{账户}` | progress |

- 未被 `process_config` 注册的进程：`process_status` 镜像不写入，但广播仍无条件发出（前端可感知未知进程状态）。
- `md_shm_config` 前端已有消费（展示与编辑，pending 由本消息清除）；`event_shm_config` 前端无 UI 消费，忽略无害。

### 2.3 控制消息（`payload` 键）

| type | 触发 | 载荷 |
|---|---|---|
| `log_line` | 日志 tail 推送（客户端 `subscribe_log` 后逐行推送，单次上限 200 行） | `{ file, line: { n, ts, level, logger, func, file, line, pid, tid, msg, raw, parsed } }` |
| `log_tail_unsubscribed` | 服务端主动退订日志 tail（连续失败 3 次 / 订阅自身日志被拒） | `{ file, reason }`；`reason ∈ self_logs_cannot_be_tailed \| send failures` |
| `data_changed` | 后端 DB 数据变更（用户上下线等），通知前端按 `scope` 重新 REST 刷新 | `{ scope }` |
| `default_password_warning` | admin 用户连接且仍为默认密码且未确认时，定向推送 | `{ message }` |
| `error` | 连接协议错误（鉴权失败 / 非法 JSON / 未知 type / 控制消息参数缺失 / 管理操作权限或进程状态预检未通过） | `{ message, seq? }` |
| `pong` | 响应客户端 `ping` | 无载荷 |

### 2.4 控制消息 ACK（多态 `*_ack`）

响应客户端控制消息，`payload` 携带结果与原始 `seq`。**ack 统一语义：表示请求已被 dzweb 受理（写入事件通道成功），不代表业务结果**；业务结果由对应领域消息/镜像推送表达（下表"业务结果"列）。

| type | 触发 | 载荷 | 业务结果 |
|---|---|---|---|
| `md_connect_ack` | `md_connect` | `{ source, ok }`；`ok=false` 时前端立即提示，不设 pending | 行情进程的 `progress`（登录态）+ 健康度 |
| `md_disconnect_ack` | `md_disconnect` | `{ source, ok }`（与 `md_connect_ack` 对称） | 同上 |
| `subscribe_log_ack` | `subscribe_log` | `{ file }` | `log_line` 逐行推送 |
| `unsubscribe_log_ack` | `unsubscribe_log` | `{}` | — |
| `query_md_subscriptions_ack` | `query_md_subscriptions` | `{ source, ok }`；`ok=false` 时前端立即提示，不设 pending | `md_rtn_subscriptions`（`error=null` 为成功数据，非 null 为查询失败） |

---

## 3. 客户端 → 服务端（C2S）

C2S 消息格式：`{ "type": ..., "seq": 0, "payload": { ... } }`（`payload` 键包裹全部参数）。`seq` 可选，由 `*_ack`/`error` 回填。

| type | payload | 校验规则（dzweb） | 处理步骤 |
|---|---|---|---|
| `ping` | — | — | 回 `pong` |
| `md_connect` | `{ source }` | `source` 非空、事件通道可用、当前连接为 admin、目标进程在镜像中为 `Running`；否则回 `error`（message 说明原因），不写帧 | 写 `REQUEST_MD_CONNECT`（`instance_id=source`）→ 回 `md_connect_ack{source, ok:true}` |
| `md_disconnect` | `{ source }` | 同 `md_connect`（含 admin 与镜像 `Running` 校验） | 写 `REQUEST_MD_DISCONNECT` → 回 `md_disconnect_ack{source, ok:true}` |
| `subscribe_log` | `{ file }` | `file` 非空；否则回 `error` | 拒绝订阅自身日志（回 `log_tail_unsubscribed{reason:self_logs_cannot_be_tailed}`，不建订阅）；建立本连接日志订阅 → 回 `subscribe_log_ack{file}` |
| `unsubscribe_log` | — | — | 清除本连接订阅 → 回 `unsubscribe_log_ack{}` |
| `query_md_subscriptions` | `{ source, query }` 或 `{ source, instruments }` | `source` 非空；`query` 为字符串或 `instruments` 为数组（二选一，缺一或类型错回 `error`）；dzweb 不校验 `query` 取值与互斥（由目标 md 进程校验，经 `RTN.error` 表达） | 构造 SHM payload（不含 `source`，`source` 作为 `instance_id`）写 `QUERY_MD_SUBSCRIPTIONS` → 回 `query_md_subscriptions_ack{source, ok}` |

未知 `type` / 非法 JSON：回 `error`（message 说明原因，`seq` 回填）。

---

## 4. REST 与 WS 的分工

- WS 是**状态推送通道**（镜像增量 + 快照）；REST 是**请求入口与大数据查询**（见契约 rest）。
- 设置类请求统一走 REST：进程启停（`POST /api/market-sources/{id}/start|stop` → `REQUEST_PROCESS_CONTROL`）、行情连接（`POST /api/market-sources/{id}/login|logout` → `REQUEST_MD_CONNECT/DISCONNECT`）、行情配置（brokers CRUD → `SET_MD_CONFIG`）、自动登录（`PUT .../auto-login` → `SET_AUTO_LOGIN`）、日志配置（`POST /api/logs/level|flush` → `SET_LOG_CONFIG`/`FLUSH_LOG`）。WS 的 `md_connect`/`md_disconnect`/`query_md_subscriptions` 为等效便捷通道，两路行为一致。
- `md_connect`/`md_disconnect` 与 REST `POST /api/market-sources/{id}/login|logout` 核心守卫一致：admin 角色 + 目标进程镜像 `Running` 预检。镜像未就绪（dzweb 启动初期快照未完成）时预检可能保守拒绝（回 `error`，前端不设 pending）；预检放行后目标进程失效的场景由 pending 超时/progress 推送兜底。
- 前端在 REST 请求成功后**不得假设已生效**：以 WS 领域消息（RTN 推送）为生效信号。

---

## 5. 前端行为义务

以下为跨进程可观察行为义务（视觉细节不属契约）：

1. **pending 生命周期**：设置/控制请求发出后设 pending → 收到对应领域消息（RTN 推送）后清除；`*_ack` 的 `ok=false` 立即清除并提示；**所有请求必须有超时兜底**（建议 5s，超时视为目标进程无响应并提示），总则 §7。
2. **pending 清除信号的唯一来源**：由各契约声明（如进程操作用 `process_status` 的 `event` 字段，日志配置用 `log_config` 推送）；`notify_ui` 是纯通知，**永不**清除 pending（契约 notify-ui）。
3. **错误展示来源**：业务失败经 `NOTIFY_UI`（`popup=true` 必须打断展示）或领域消息的错误字段（如 `md_rtn_subscriptions.error`）表达；`error`（WS 协议错误）只表示请求未被受理。
4. **刷新来源**：镜像数据以 WS 推送为准（不本地缓存镜像）；`data_changed` 只提示按 `scope` 重新 REST 拉取对应列表。
5. **幂等覆盖**：所有领域消息按"后到覆盖先到"处理（含快照分发与增量 apply），重复消息无害；对已移除进程的后续 `process_status` 忽略（契约 process）。
6. **订阅查询结果处理**：以 `md_rtn_subscriptions` 的 `error` 字段判定成功/失败（`null`=成功），`truncated=true` 时按契约 md-subscription 的截断语义展示。

---

## 6. 重连语义

- 客户端退避重连：3s×2ⁿ，最多 5 次；用尽进入 `failed`，30s 后归零重试（7×24 自愈）。重连参数为实现细节，非协议。
- 重连成功（`onopen`）后：服务端重推 `snapshot`，前端按快照领域分发到各 store（幂等覆盖，并清 pending），并 REST 拉取行情源列表（DB 为真相源，契约 rest）。
- 镜像不被前端本地缓存：断连期间错过的增量消息由重连后的 `snapshot` 全量补齐。

---

## 7. 已知差异与遗留

- 高频行情（tick/K 线）**不进** WS 镜像模型：镜像只服务低频状态/配置帧，高频走独立 msgpack 二进制通道（预留，未实现）。
- 历史消息 `rtn_process_control` 已删除：进程操作 pending 由 `process_status` 帧的 `event` 字段清理（契约 process）。
- 快照 `process_status` 域按帧层 RTN 序列化（`event` 缺失），与增量消息的 `event:null` 形状不同；前端对缺失与 null 均按自发状态处理（§2.2 形状注）。
