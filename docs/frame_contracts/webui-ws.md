# WebSocket 协议：frontend ↔ dzweb

本文件规定 WebUI 前端与 dzweb 后端之间的 WebSocket 消息协议（type + payload 结构 + 校验 + 触发场景），与各帧契约同构。总则见《帧契约：通用规则》。

> 本文件覆盖的是**进程间契约投射到 WS 的最终形态**：子进程通过 SHM 上报 RTN 帧 → dzweb 领域服务更新镜像 + WS 推到前端。因此大量消息与对应帧契约的 payload 一致，本文件只描述 WS 信封与差异，payload 字段细节以对应帧契约为准。例外：交易数据事件（§2.5）**不落镜像**，其 payload 形状以契约 rest 为唯一真相源。
>
> 链路定位（总则 §4.2）：§2.2 领域消息 = 形态 1 响应侧 / 形态 4 的 dzweb 出口；§3 C2S 控制消息 = 形态 1/3 的 WS 前端入口（与 REST 等效）。

> **实现注记**：委托事件 `td_order_report` 与建连补发 `td_recent` 已随 #246 落地；成交 / 持仓 / 资金事件由 #248–#250 落地，`td_recent` 的事件集合随它们扩展（当前只含委托事件）。账户配置/细状态镜像的 WS 映射（契约 td-config / td-status）随 #251 接入（§7）。

---

## 1. 连接与鉴权

- 端点：`/ws?token=<jwt>`（也接受 `Authorization: Bearer <jwt>` 头）。
- 鉴权失败：服务端回 `{"type":"error","payload":{"message":"auth failed"}}` 并关闭连接。
- **凭证有效性随口令变化失效**（契约 rest §1「会话吊销」）：除验签 + `exp` 外，JWT 的 `ver` claim 必须等于该用户当前的 `token_version`；不等（改密 / 管理员重置 / 被禁用）或该用户 `must_change_password=1` 时一律按鉴权失败处理（回 `auth failed` 并断开）。已建立的连接不逐帧复核，由服务端在吊销时主动 `shutdown`（契约 rest §2.2 的重置/禁用路径）。
- 建连成功：服务端立即推送**全量镜像快照** `snapshot`（连接/重连的初始基线），随后定向推送一次交易数据补发 td_recent（§2.5），之后以增量消息更新。
- 心跳：客户端每 30s 发 `{"type":"ping"}`，服务端回 `{"type":"pong"}`（无载荷）。服务端不因心跳缺失而踢连接。
- 多客户端：同用户可多连接；领域消息对所有已连接客户端广播；`snapshot`/td_recent/`default_password_warning`/`*_ack`/`error`/`log_line`/`log_tail_unsubscribed`/`pong` 不广播（按连接/定向发送）；`data_changed` 对所有客户端广播。
- 服务端**不仲裁并发请求**：多个客户端发出的请求按到达顺序由 dzweb 串行写帧，由目标进程串行应用（总则 §7）。

### 信封格式

| 键 | 说明 |
|---|---|
| `type` | 消息类型（见 §2/§3） |
| `data` | 领域消息载荷（广播帧专用） |
| `payload` | 控制消息载荷（C2S 请求、`*_ack`/`error`/`log_line`/`log_tail_unsubscribed`/`data_changed`/`default_password_warning`、td_recent） |
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
| `progress` | 各进程（TD 为账户 ID） | `{ min, max, current, desc }` | progress |
| `md_config` | 各行情进程 | 脱敏行情配置 | md-config |
| `md_status` | 各行情进程 | 行情网关状态 | md-status |
| `sysmon_snapshot` | 固定 `dzsysmon` | 系统监控快照 | sysmon |
| `sysmon_config` | 固定 `dzsysmon` | 系统监控全量生效配置 | sysmon |

> 注：
> - `md_rtn_subscriptions`（契约 md-subscription）为查询响应，**不进镜像**，故不在快照中。
> - **交易记录（委托/成交/持仓/资金）不在快照中**：dzweb 不保留记录镜像（ADR 0043），记录基线一律由 REST 拉取（契约 rest §2.9）；WS 只推增量交易事件（§2.2）与有界建连补发（§2.5）。
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
| `sysmon_snapshot` | `RTN_SYSMON_SNAPSHOT` | 系统监控快照（契约 sysmon §2） | `dzsysmon`（固定；帧头无实例标识） | sysmon |
| `sysmon_config` | `RTN_SYSMON_CONFIG` | 全量生效配置（契约 sysmon §3） | `dzsysmon` | sysmon |
| td_order_report | `ORDER_REPORT` | 契约 rest §2.9.3 的委托行（含 avg_price / price_decimals / avg_price_decimals / seq / sort_key） | 空（account_id 在 payload 内） | td-data-sync |
| td_trade_report | `TRADE_REPORT` | 契约 rest §2.9.3 的成交行 | 空（account_id 在 payload 内） | td-data-sync |
| td_position_info | `POSITION_INFO` | 契约 rest §2.9.4 的持仓行 | 空（account_id 在 payload 内） | td-data-sync |
| td_trading_account | `TRADING_ACCOUNT` | 契约 rest §2.9.4 的资金行 | 空（account_id 在 payload 内） | td-data-sync |

- 未被 `process_config` 注册的进程：`process_status` 镜像不写入，但广播仍无条件发出（前端可感知未知进程状态）。
- `md_shm_config` 前端已有消费（展示与编辑，pending 由本消息清除）；`event_shm_config` 前端已消费（系统设置页展示，编辑 pending 由本消息清除，入口契约 rest §2.7 `PUT /api/settings/event-shm-config`）。
- 四类交易事件（td_*）是**帧转发而非镜像域**：不写 `MirrorStore`、不进 `snapshot`；payload 与 REST 行同 schema（唯一真相源：契约 rest §2.9.3 与契约 rest §2.9.4）。转发条件与补发容量见《帧契约：TD 数据同步》§8；WS 时序与合并语义见 §2.5、§5。

### 2.3 控制消息（`payload` 键）

| type | 触发 | 载荷 |
|---|---|---|
| `log_line` | 日志 tail 推送（客户端 `subscribe_log` 后逐行推送，单次上限 200 行） | `{ file, line: { n, ts, level, logger, func, file, line, pid, tid, msg, raw, parsed } }` |
| `log_tail_unsubscribed` | 服务端主动退订日志 tail（连续失败 3 次 / 订阅自身日志被拒） | `{ file, reason }`；`reason ∈ self_logs_cannot_be_tailed \| send failures` |
| `data_changed` | 后端数据变更（DB 写或交易数据基线事件，如在线账户集合变化/日切），通知前端按 `scope` 重新 REST 刷新 | `{ scope }`；scope 取值见下 |
| td_recent | 鉴权成功建连后（snapshot 之后）定向发送一次（§2.5） | `{ events: [ { type, data } ] }`；`type` 为四类交易事件名之一，`data` 为对应行对象；无事件时 `events` 为空数组（仍发送） |
| `default_password_warning` | admin 用户连接且仍为默认密码且未确认时，定向推送 | `{ message }`；**`message` 是后端给出的完整中文告警文案（唯一真相源 `apps/webui/config.h` 的 `kDefaultPasswordWarningMessage`），前端直接展示、不得自行拼接或硬编码** |
| `error` | 连接协议错误（鉴权失败 / 非法 JSON / 未知 type / 控制消息参数缺失 / 管理操作权限或进程状态预检未通过） | `{ message, seq? }` |
| `pong` | 响应客户端 `ping` | 无载荷 |

- `data_changed` 的 scope 语义（前端只按 scope 重新拉取，不得用本地缓存回填）：`users` / `security_config` / `blacklist` / `whitelist` / `login_history` / `market_sources` / `td`。`td` = 交易数据基线失效——日切、在线账户集合变化、账户注册表集合增减，前端清空委托/成交/持仓/资金/统计并重拉（§5）。新增 scope 必须在本清单登记。

### 2.4 控制消息 ACK（多态 `*_ack`）

响应客户端控制消息，`payload` 携带结果与原始 `seq`。**ack 统一语义：表示请求已被 dzweb 受理（写入事件通道成功），不代表业务结果**；业务结果由对应领域消息/镜像推送表达（下表"业务结果"列）。

| type | 触发 | 载荷 | 业务结果 |
|---|---|---|---|
| `md_connect_ack` | `md_connect` | `{ source, ok }`；`ok=false` 时前端立即提示，不设 pending | 行情进程的 `progress`（登录态） |
| `md_disconnect_ack` | `md_disconnect` | `{ source, ok }`（与 `md_connect_ack` 对称） | 同上 |
| `subscribe_log_ack` | `subscribe_log` | `{ file }` | `log_line` 逐行推送 |
| `unsubscribe_log_ack` | `unsubscribe_log` | `{}` | — |
| `query_md_subscriptions_ack` | `query_md_subscriptions` | `{ source, ok }`；`ok=false` 时前端立即提示，不设 pending | `md_rtn_subscriptions`（`error=null` 为成功数据，非 null 为查询失败） |

### 2.5 交易数据事件与建连补发（td）

四类交易事件由交易网关的推送帧**转发**而来（《帧契约：TD 数据同步》），不是镜像域：

- **payload 即行对象**：消息载荷（`data`）= 契约 rest §2.9.3 与契约 rest §2.9.4 定义的同一行 schema（含 seq、price_decimals、sort_key 等派生字段）；`instance_id` 为空（account_id 在 payload 内）。
- **转发与容量**：转发条件、在线账户过滤、Offline 清环与每账户补发容量见《帧契约：TD 数据同步》§8；dzweb 不维护记录镜像，前端记录基线由 REST 拉取（契约 rest §2.9）。
- **合并**：前端按业务键去重、同键 seq 大者胜（§5）；委托/成交按 sort_key 降序插入，持仓/资金按业务键覆盖。

**td_recent（建连补发）**：

- 鉴权成功建连后、`snapshot` 之后定向发送一次；载荷 `{"events": [{"type": <四类交易事件名>, "data": <行对象>}, ...]}`。
- 内容 = 各在线账户补发环内容（容量见《帧契约：TD 数据同步》§8），按入环先后从旧到新；不同账户之间顺序不承诺。
- 无事件时仍发送 `{"events": []}`（前端据此确定补发阶段结束）。
- 补发事件与随后到达的实时事件使用同一合并规则；重复/乱序无害（同键 seq 大者胜）。

**恢复**：dzweb 重启后补发环为空——记录基线以 REST 为准，补发只是收敛窗口内的加速。断线重连时服务端重推 `snapshot` + td_recent，前端按 §6 重拉 REST 基线。

**日切 / 在线集合变化**：dzweb 广播 `data_changed{scope:"td"}`（§2.3），前端清空并重拉基线（§5）。

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
- **交易记录**：REST 是唯一基线查询通路（契约 rest §2.9 直查交易库），WS 是增量推送（§2.5）；交易记录不进 `snapshot`、不落镜像。
- 设置类请求统一走 REST：进程启停（`POST /api/market-sources/{id}/start|stop` → `REQUEST_PROCESS_CONTROL`）、行情连接（`POST /api/market-sources/{id}/login|logout` → `REQUEST_MD_CONNECT/DISCONNECT`）、行情配置（brokers CRUD → `SET_MD_CONFIG`）、自动登录（`PUT .../auto-login` → `SET_AUTO_LOGIN`）、日志配置（`POST /api/logs/level|flush` → `SET_LOG_CONFIG`/`FLUSH_LOG`）。WS 的 `md_connect`/`md_disconnect`/`query_md_subscriptions` 为等效便捷通道，两路行为一致。
- `md_connect`/`md_disconnect` 与 REST `POST /api/market-sources/{id}/login|logout` 核心守卫一致：admin 角色 + 目标进程镜像 `Running` 预检。镜像未就绪（dzweb 启动初期快照未完成）时预检可能保守拒绝（回 `error`，前端不设 pending）；预检放行后目标进程失效的场景由 pending 超时/progress 推送兜底。
- 前端在 REST 请求成功后**不得假设已生效**：以 WS 领域消息（RTN 推送）为生效信号。

---

## 5. 前端行为义务

以下为跨进程可观察行为义务（视觉细节不属契约）：

1. **pending 生命周期**：设置/控制请求发出后设 pending → 收到对应领域消息（RTN 推送）后清除；`*_ack` 的 `ok=false` 立即清除并提示；**所有请求必须有超时兜底**（建议 5s，超时视为目标进程无响应并提示），总则 §7。
2. **pending 清除信号的唯一来源**：由各契约声明（如进程操作用 `process_status` 的 `event` 字段，日志配置用 `log_config` 推送，行情登录失败用 `md_rtn_status` 的 `last_error` 变化——契约 md-status）；`notify_ui` 是纯通知，**永不**清除 pending（契约 notify-ui）。
3. **错误展示来源**：业务失败经 `NOTIFY_UI`（`popup=true` 必须打断展示）或领域消息的错误字段（如 `md_rtn_subscriptions.error`）表达；`error`（WS 协议错误）只表示请求未被受理。
4. **刷新来源**：镜像数据以 WS 推送为准（不本地缓存镜像）；`data_changed` 只提示按 `scope` 重新 REST 拉取对应列表。交易数据例外：记录基线以 REST 为准（第 7 条）。
5. **幂等覆盖**：所有领域消息按"后到覆盖先到"处理（含快照分发与增量 apply），重复消息无害；对已移除进程的后续 `process_status` 忽略（契约 process）。
6. **订阅查询结果处理**：以 `md_rtn_subscriptions` 的 `error` 字段判定成功/失败（`null`=成功），`truncated=true` 时按契约 md-subscription 的截断语义展示。
7. **交易数据基线**：交易记录（委托/成交/持仓/资金）与统计的基线一律由 REST 拉取（契约 rest §2.9）。`snapshot` 不含交易记录、td_recent 有界——二者都不能替代基线；重连/日切/`data_changed{scope:"td"}` 后必须重拉。
8. **交易事件合并**：按业务键去重 + 同键 `seq` 大者胜（与到达顺序无关）；重复/乱序无害；同一账户检测到 `seq` 回退视为数据重置——清空该账户交易数据并重拉基线。业务键见契约 rest §2.9.6。
9. **排序与分页**：委托/成交按 sort_key 降序维护（增量行按 sort_key 插入，不是简单前插）；滚动到底按 next_cursor 拉取下一页（契约 rest §2.9.2）；筛选、日切或作用域变化后丢弃游标从首页重拉。
10. **日切/账户集合变化**：收到 `data_changed{scope:"td"}`（§2.3）后清空委托/成交/持仓/资金/统计并重拉基线；日切后不得混入上一交易日数据。统计无 WS 推送，按需轮询 REST（契约 rest §2.9.5）。
11. **账户筛选**：账户多选默认全选；选项来源——委托/成交/统计用 online_accounts，持仓/资金用返回的账户全集（含离线；资金行 has_data:false 也进筛选项）；筛选变化必须重置游标从首页开始。
12. **精度**：价格显示用行内 price_decimals / avg_price_decimals 做 `toFixed`；不得自行推断小数位，也不得依赖服务端字符串舍入（契约 rest §2.9.6）。
13. **降级**：列表返回 `available:false` 时展示统一降级提示（不白屏、不用 0 冒充数据），按退避重试；WS 事件照常合并，恢复后重拉基线。
14. **非契约范围**：有界窗口容量、虚拟滚动、批量刷新节流、新数据气泡等 UI 性能/交互设计见 ADR 0043 决策 7，由前端实现票落地，不属于本契约语义。

---

## 6. 重连语义

- 客户端退避重连：3s×2ⁿ，最多 5 次；用尽进入 `failed`，30s 后归零重试（7×24 自愈）。重连参数为实现细节，非协议。
- 重连成功（`onopen`）后：服务端重推 `snapshot` 与一次 td_recent；前端按快照领域分发到各 store（幂等覆盖，并清 pending），并 REST 重拉行情源列表（DB 为真相源，契约 rest）与交易数据基线（委托/成交/持仓/资金/统计，契约 rest §2.9）。
- 镜像不被前端本地缓存：断连期间错过的增量消息由重连后的 `snapshot` 全量补齐。交易记录不在镜像中：断连期间错过的交易事件由 td_recent 补发 + REST 基线重拉收敛（补发有界，基线是权威）。

---

## 7. 已知差异与遗留

- 高频行情（tick/K 线）**不进** WS 镜像模型：镜像只服务低频状态/配置帧，高频走独立 msgpack 二进制通道（预留，未实现）。
- 历史消息 `rtn_process_control` 已删除：进程操作 pending 由 `process_status` 帧的 `event` 字段清理（契约 process）。
- 快照 `process_status` 域按帧层 RTN 序列化（`event` 缺失），与增量消息的 `event:null` 形状不同；前端对缺失与 null 均按自发状态处理（§2.2 形状注）。
- 交易记录（委托/成交/持仓/资金）**不镜像、不进 snapshot**（ADR 0043）：WS 只推增量交易事件（§2.5）与有界建连补发，记录基线一律由 REST 直查交易库（契约 rest §2.9）。
- 账户配置与细状态（契约 td-config / td-status）的镜像域与 WS 消息随账户配置页实现（#251）接入本契约；当前 `snapshot` 不含这两个域，账户配置页读取随该票定义。
