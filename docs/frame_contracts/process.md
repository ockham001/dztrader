# 帧契约：进程

本文件覆盖 `DZ_FRAME_REQUEST_PROCESS_CONTROL`、`DZ_FRAME_RTN_PROCESS_STATUS`、`DZ_FRAME_SET_PROCESS_CONFIG`、`DZ_FRAME_RTN_PROCESS_CONFIG`、`DZ_FRAME_SHUTDOWN` 五个帧。总则见《帧契约：通用规则》。

- 前四个帧使用 `DzExtFrameHeader` 扩展头（无 `instance_id`），目标/来源均在 payload。
- 优雅退出帧 `SHUTDOWN` 为定向帧（`instance_id` = 目标进程名）。

类型层真相源：`libs/platform/include/dztrader/platform/process.h`（枚举、结构体、序列化、校验、合并函数）。

控制帧（REQUEST_PROCESS_CONTROL / RTN_PROCESS_STATUS）管理运行时操作与状态；配置帧（SET_PROCESS_CONFIG / RTN_PROCESS_CONFIG）管理进程级静态配置。Start 操作可携带配置 patch，实现"配置并启动"一步到位。

---

## 数据结构

### ProcessAction

| 值 | 说明 |
|------|------|
| `Start` | 启动进程 |
| `Stop` | 停止进程 |
| `Remove` | 移除进程（停止 + 清理配置） |

### ChildState

| 值 | 说明 |
|------|------|
| `Starting` | 启动中。master 启动进程后默认直接推 `Running`（不推 `Starting`）；仅启动流程存在可感知延迟（如等待子进程就绪）时可推。前端必须能显示该状态（快照/未来推送中可能出现） |
| `Running` | 运行中 |
| `Stopping` | 已发送关闭请求，等待退出。中间态：master 保证最终推送 `Stopped` 或 `Crashed`；等待超时则强制终止（见 §强制终止） |
| `Stopped` | 已正常退出（exit_code == 0） |
| `Crashed` | 已异常退出（exit_code != 0 或 spawn 失败） |

### ProcessEvent

操作结果事件，仅在 `RTN_PROCESS_STATUS` 中出现，用于关联某次 `REQUEST_PROCESS_CONTROL`。

| 值 | 说明 |
|------|------|
| `StartSucceeded` | 启动成功；已在 `Running`（幂等，重试安全） |
| `StartFailed` | 启动失败（exe 未找到 / 动态注册失败 / spawn 失败等；未注册但可扫描到同名网关 exe 时动态注册启动，不属于失败） |
| `StopSucceeded` | 停止请求已受理并已派发（重试安全）；幂等场景（本已 `Stopped`）直接成功，无后续帧 |
| `StopFailed` | 停止请求派发失败（进程不存在或内部异常），附 `message` |
| `RemoveSucceeded` | 移除流程已启动（配置已删除）；运行中进程附 `state=Stopping`，已停止进程附 `state=Stopped, pid=0` |
| `RemoveFailed` | 移除失败（目标不存在 / 配置删除持久化失败 / 停止派发失败），不幂等 |

> `state` 始终反映进程当前真实状态，与 `event` 无固定绑定。如 `StartFailed`（spawn 失败）时 `state=Crashed`、`pid=0`；`StartSucceeded`（幂等，已在 Running）时 `state=Running`。

### RestartPolicy

| 字段 | 类型 | 说明 |
|------|------|------|
| `enabled` | bool | 是否启用自动重启 |
| `max_attempts` | int | 最大连续重启次数，`enabled=false` 时忽略 |
| `backoff_sec` | int | 退避基数（秒） |

**协议级语义**（内部退避公式、计数细节、稳定窗口等调度实现不属契约）：

- 崩溃退出且 `enabled=true` 时，master 推 `Crashed` 后可能自动重启；**`Crashed` 非终态**，前端不得假设其永久
- 重启成功 → 推 `Running`（不经过 `Starting`，`event` 缺失）
- 重启 spawn 失败或达到 `max_attempts` → 停止重启，状态保持 `Crashed`
- 不重启场景：正常退出 / remove 流程 / master 关闭中
- 等待重启期间状态保持 `Crashed`，无额外推送

**校验**（SET_PROCESS_CONFIG / Start 携带的 config 均适用）：`max_attempts`/`backoff_sec` 为 int 且 ≥ 0；`enabled=true` 且 `max_attempts=0` 时等价于不重启。

### ProcessConfig

| 字段 | 类型 | RTN 必填 | 增量语义 |
|------|------|------|------|
| `args` | array\<string\> | 是 | 出现则整体覆盖 |
| `env` | object | 是 | 递归合并；某个 key 的 value 为 `null` 表示删除该 key |
| `restart` | object | 是 | 出现则整体覆盖（非递归合并）：`enabled`/`max_attempts`/`backoff_sec` 三个子字段必须全部出现，缺失任一 = 校验失败 |
| `display_name` | string | 否 | 出现则覆盖，空串=清空显示名；缺失时前端回退到进程名 |

- 全量角色（`RTN_PROCESS_CONFIG`）：`args`/`env`/`restart` 必须出现；`display_name` 可缺失。
- 增量角色（`SET_PROCESS_CONFIG` / `REQUEST_PROCESS_CONTROL` 携带的 config）：RFC 7386 语义，字段缺失=不修改。

### 参数可改性

| 分类 | 参数 | 说明 |
|------|------|------|
| 可修改 | `args` / `env` / `restart` / `display_name` | Start 携带：先应用配置，当次启动生效；其余时刻修改：生效时机见 SET_PROCESS_CONFIG |
| 本契约不可修改 | SHM 页大小（`page_size_mb`） | 仅人工编辑配置文件，进程创建后不可变（见《帧契约：SHM 通道配置》） |
| 不可修改 | 进程名 | 进程的身份标识，非参数 |

> 可执行文件路径与启动目录由系统派生，不属配置项。

---

## DZ_FRAME_REQUEST_PROCESS_CONTROL

**语义**：请求 master 对目标进程执行启动/停止/移除
**数据流**：形态 1（总则 §4.2）——dzweb → master（帧头无 `instance_id`，目标在 payload `target`）；前端入口 `POST /api/market-sources/{id}/start|stop`、`DELETE /api/market-sources/{id}`（契约 rest）；响应帧 `RTN_PROCESS_STATUS`（带 `event`）→ 镜像 `process_status` 域 → WS 消息 `process_status`。Start 携带 `config` 成功时先推 `RTN_PROCESS_CONFIG` 再推 `RTN_PROCESS_STATUS`（帧顺序见约束）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | ProcessAction | 是 | 控制动作 |
| `target` | string | 是 | 目标进程名，如 `"dzmd_ctp"` |
| `config` | ProcessConfig（增量角色） | 否 | 配置 patch，仅 `action=Start` 时有效；master 先应用配置再启动。为 `null` 时校验失败 |

```json
{"action": "Start", "target": "dzmd_ctp", "config": {"display_name": "CTP行情源"}}
{"action": "Start", "target": "dzmd_ctp"}
{"action": "Stop", "target": "dzmd_ctp"}
{"action": "Remove", "target": "dzmd_ctp"}
```

**时序**：前端提交触发 → 必回一条带 `event` 的 `RTN_PROCESS_STATUS`（总则 §7 兜底适用）

**约束**：

- `target` 必须为已注册进程：Stop / Remove 对未注册 `target` 分别回 `StopFailed` / `RemoveFailed`，附 `message` 与 NOTIFY_UI 错误弹窗（四件套，总则 §8）
- **Start 的动态注册**（"添加行情源"场景）：`target` 未注册时，master 实时扫描 App Root 下的同名网关可执行文件（`dzmd_*`/`dztd_*`）；找到则先动态注册（写配置条目、持久化、推 `RTN_PROCESS_CONFIG` 全量新值），再走正常启动流程；exe 不存在或非网关进程（策略有独立注册流程）则回 `StartFailed`。扫描/存储机制是实现细节，协议语义仅为：未注册 `target` 的 Start 可能成功，且成功时先推 `RTN_PROCESS_CONFIG` 再推 `RTN_PROCESS_STATUS`
- `config` 仅 `action=Start` 时有效，其余动作忽略
- `config` 携带时，master 先应用配置（等价于 SET_PROCESS_CONFIG），再启动进程；配置应用失败按 SET_PROCESS_CONFIG 失败处理，不启动进程
- **帧顺序固定**：`config` 应用成功 → 先推 `RTN_PROCESS_CONFIG`（全量新值），再推 `RTN_PROCESS_STATUS`（`StartSucceeded`）
- 配置应用成功但 spawn 失败：配置**保留**（不回滚），推 `RTN_PROCESS_CONFIG`（新值）+ `RTN_PROCESS_STATUS{StartFailed}` + NOTIFY_UI 错误弹窗
- 配置应用失败：推 `RTN_PROCESS_CONFIG`（回滚旧值）+ `RTN_PROCESS_STATUS{StartFailed}` + NOTIFY_UI 错误弹窗，不启动进程
- **幂等**：目标状态已达视为成功（Start 已 Running → `StartSucceeded`；Stop 已 Stopped → `StopSucceeded`），前端超时重发安全；Remove 目标不存在 → `RemoveFailed`（不幂等，用于区分"已移除"与"名字错误"）
- master 关闭期间不保证处理/响应请求，前端以 pending 超时兜底

---

## DZ_FRAME_RTN_PROCESS_STATUS

**语义**：master 推送进程状态（单条完整状态）
**数据流**：形态 4（总则 §4.2）——master → dzweb（帧头无 `instance_id`，进程名在 payload `name`）；无前端入口；镜像 `process_status` 域 → WS 消息 `process_status`（未注册进程不进镜像但仍广播）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 进程名 |
| `state` | ChildState | 是 | 当前状态 |
| `pid` | int | 是 | 进程 PID，未运行时为 0 |
| `display_name` | string | 否 | 用户可读显示名，master 按当前配置填充；缺省为空串 |
| `message` | string | 否 | 状态说明 / 失败原因 / 退出信息；缺省为空串 |
| `event` | ProcessEvent | 否 | 操作结果事件；缺失 = 自发状态变化 |

```json
{"name": "dzmd_ctp", "state": "Running", "pid": 12345, "display_name": "CTP行情", "event": "StartSucceeded"}
{"name": "dzmd_ctp", "state": "Crashed", "pid": 0, "message": "spawn failed: no such file", "event": "StartFailed"}
{"name": "dzmd_ctp", "state": "Stopping", "pid": 12345, "event": "StopSucceeded"}
{"name": "dzmd_ctp", "state": "Stopped", "pid": 0, "message": "exit_code=0"}
{"name": "dzmd_ctp", "state": "Running", "pid": 12345, "display_name": "CTP行情"}
```

**触发场景**：

1. `REQUEST_PROCESS_CONTROL` 的响应（带 `event`）
2. 进程状态变化（`event` 缺失）
3. `QUERY_FULL_SNAPSHOT` 响应：master 对每个注册进程各推一条（`event` 缺失），无顺序要求；未注册进程无推送

**约束**：

- 单条完整状态：接收方收到后直接覆盖该进程的状态，无需增量合并
- `event` 缺失表示自发状态变化（崩溃退出、自动重启、全量快照等），前端仅更新状态，不清 pending
- `display_name` 唯一真相源为配置：前端以 `RTN_PROCESS_CONFIG` 为准维护显示名；本帧的 `display_name` 仅用于展示，不回写配置镜像
- Stop/Remove 的 Succeeded 表示请求已受理；进程实际退出后另推一条 `event` 缺失的 RTN（`state=Stopped`/`Crashed`）；已处于 `Stopped` 的幂等场景无后续帧
- Remove 流程：master 先删除配置并推 `RTN_PROCESS_CONFIG`（该进程条目消失），随后进程退出时推 `RTN_PROCESS_STATUS`（`event` 缺失；已停止进程则无此帧）。**条目消失是移除完成的唯一权威信号**（配置条目 = 进程存在性的唯一真相源）
- **强制终止**（`Stopping` 等待超时）：master 强制 kill 后按退出码判定状态推送（强制 kill 的 exit_code != 0 → `Crashed`，`message` 说明原因）；超时时刻已发警告通知，退出时不再重复崩溃弹窗

**镜像**：
- dzweb 以 `payload.name` 定位实例，更新镜像 `process_status` 域，WS 推送
- 未被 `process_config` 注册的进程：镜像不写入，但 WS 广播仍无条件发出（前端可感知未知进程状态）

---

## DZ_FRAME_SET_PROCESS_CONFIG

**语义**：请求 master 修改目标进程的配置（RFC 7386 增量更新）
**数据流**：形态 1（总则 §4.2）——dzweb → master（帧头无 `instance_id`，目标在 payload `target`）；前端入口：当前未定义独立端点（Start 携带 `config` 的路径间接覆盖，契约 rest）；响应帧 `RTN_PROCESS_CONFIG` → 镜像 `process_config` 域 → WS 消息 `process_config`
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `target` | string | 是 | 目标进程名 |
| `config` | ProcessConfig（增量角色） | 是 | 配置 patch，必须为 object（`null` 校验失败） |

```json
{"target": "dzmd_ctp", "config": {"restart": {"enabled": true, "max_attempts": 3, "backoff_sec": 10}, "display_name": "CTP行情源"}}
{"target": "dzmd_ctp", "config": {"env": {"OLD_VAR": null}}}
{"target": "dzmd_ctp", "config": {}}
```

**时序**：前端提交触发 → 必回 `RTN_PROCESS_CONFIG`（总则 §7 兜底适用）

**约束**：

- 合并语义见 ProcessConfig 增量角色；`null` 规则见总则 §8（仅 `env` 内部 key 的 value 为 null 合法 = 删除）
- `target` 不存在时校验失败
- 校验失败/持久化失败四件套：总则 §8（回滚旧值 + 日志 + NOTIFY_UI 错误弹窗 + 回 `RTN_PROCESS_CONFIG` 旧值）
- 空对象 `config: {}` 视为无操作，仍回 RTN（当前值）
- **生效时机**：配置修改对运行中进程不生效（不重启进程），进程下次启动（含自动重启）时按新配置启动；`restart` 策略变更对后续崩溃处理立即生效

---

## DZ_FRAME_RTN_PROCESS_CONFIG

**语义**：master 推送所有进程配置（全进程集合）
**数据流**：形态 4（总则 §4.2）——master → dzweb（帧头无 `instance_id`）；无前端入口；镜像 `process_config` 域（挂固定 `dztraderd`，key = 进程名）→ WS 消息 `process_config`
**Payload**：JSON，**始终全量**；object，key = 进程名，value = ProcessConfig（全量角色）

```json
{
  "dzmd_ctp": {
    "args": ["--name", "dzmd_ctp"],
    "env": {},
    "restart": {"enabled": true, "max_attempts": 5, "backoff_sec": 5},
    "display_name": "CTP行情源"
  }
}
```

**触发场景**：

1. `SET_PROCESS_CONFIG` 应用后（成功=新值，失败=回滚旧值）
2. `REQUEST_PROCESS_CONTROL`（action=Start）携带的 `config` 应用成功后
3. master 启动初始化完成后上报
4. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：

- 全量镜像：接收方直接整体覆盖本地配置镜像（覆盖天然含删除）
- **条目消失 = 进程已移除**：前端同步删除该进程的状态卡片；对已删除进程后续到达的 `RTN_PROCESS_STATUS` 忽略（不重建镜像）

**镜像**：
- dzweb 全量覆盖固定实例 `dztraderd` 的 `process_config` 域（key = 进程名），WS 推送

---

## DZ_FRAME_SHUTDOWN

**语义**：请求进程优雅退出
**数据流**：形态 5（总则 §4.2）——master → 定向（帧头 `instance_id` = 目标进程名）；无前端入口（master 自身发起，UI 的 Stop/Remove 经形态 1 间接触发）；无 RTN，结果经 `RTN_PROCESS_STATUS` 体现
**Payload**：空

**触发场景**：
- master 停止/移除某个子进程（UI 发起 Stop / Remove）
- master 整体关闭：按逆序逐批定向发送（编排顺序与批次语义见《组件架构 dztraderd》「停止与整体关闭」），不采用广播式全员关闭帧

**约束**：
- 仅 `instance_id` 匹配的进程处理
- 无 RTN、无确认；master 以进程退出状态为准（见 RTN_PROCESS_STATUS）
- 接收方必须在收到后发起退出流程；master 在 `Stopping` 等待超时后强制终止（见 RTN_PROCESS_STATUS 约束）
