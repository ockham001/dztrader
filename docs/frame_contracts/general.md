# 帧契约：通用规则

本文件是全部帧契约的总则。各契约只写与本文件不同的部分；本文件未提及的规则按本节执行。

只规定协议，不约束内部实现。

---

## 1. 目录

`docs/frame_contracts/` 是进程间帧协议的唯一语义真相源：

| 主题 | 覆盖范围 |
|------|----------|
| 通用规则（本文件） | 帧布局、传输与数据流（§4 链路形态）、请求-响应总则、快照查询 |
| 日志 | SET/FLUSH/RTN_LOG_CONFIG |
| SHM 通道配置 | 事件/行情通道配置 + 预加载 + 订阅者刷新 + 行情通道读者注册 |
| UI 通知 | NOTIFY_UI |
| 进程 | 进程控制/状态/配置 + 优雅退出请求 |
| 自动登录/登出排程 | SET/RTN_AUTO_LOGIN |
| 进度推送 | RTN_PROGRESS |
| 行情连接与订阅 | 连接请求、订阅/退订、订阅查询、行情生命周期通知 |
| 行情网关配置 | SET/RTN_MD_CONFIG |
| 行情网关状态 | RTN_MD_STATUS |
| 策略 | 策略用户输入/输出、逻辑持仓 |
| WebSocket 协议 | frontend ↔ dzweb 的 WS 信封与差异、前端行为义务 |
| REST API | frontend ↔ dzweb 的 REST 端点 |
| 交易委托请求 | TD_ORDER_REQ / TD_ORDER_CANCEL_REQ |

> 历史：本目录于 2026-07 由 `docs/flow_contracts/` 演进而来（后改名 frame_contracts）。2026-08 整理时新增本总则，原 00-05 顺延为 01-06，原 06-misc 拆解归位，原 07-10 编号不变。2026-08-16 契约文件名去除序号前缀，代码与文档引用改「契约 + 短名」格式（短名即文件名去扩展名）。

---

## 2. 帧布局（真相源：`libs/strategy_api/include/dztrader/struct.h`）

帧 = 固定头 + 可选扩展头 + payload。布局以头文件定义为准，本契约不重复字段。

- `DzFrameHeader`：所有帧共用固定头（`frame_size` 整帧大小、8 的倍数；`frame_type` 帧类型）。
- 变长帧在固定头后紧跟一个扩展头，二选一：
  - `DzExtFrameHeader`：仅 `data_size`，无实例标识（广播/无路由帧）。
  - `DzExtInstFrameHeader`：`instance_id`（`char[64]`）+ `data_size`，带实例路由（定向/来源标识）。策略帧的 `instance_id` 为裸策略名（§5 身份边界）。
- 定长 struct 帧无扩展头。

**写入 API 真相源**：`libs/platform/include/dztrader/platform/frame_codec.h`（JSON 帧）与 `libs/shm/include/dztrader/shm/writer.h`（底层 `write_frame`/`write_ext_frame`）。

---

## 3. 帧号（真相源：两个头文件，本契约不逐一列举）

- 系统帧（含 SHM 维护、日志配置、SHM 配置、优雅退出）：`libs/strategy_api/include/dztrader/data_type.h`
- UI/进程/行情控制/交易扩展帧：`libs/core/include/dztrader/core/core_data_type.h`

分段约定（分配规则）：

| 段 | 用途 |
|------|------|
| 0 | `INVALID_FILL` 边界填充，非业务帧 |
| 10-25 | 系统帧 |
| 101-121 | UI 通知、逻辑持仓、全量快照、进程、自动登录、进度 |
| 1000-1016 | 行情（数据推送 1000，控制/RTN 1001-1016） |
| 2000-2114 | 交易（推送 2000-2017，控制/配置/状态 2100-2114） |
| 3001+ | 策略（用户输入/输出等） |

新帧号必须落上述分段，登记位置与既有帧一致。

**帧头类型登记（现状）**：代码中无 `frame_has_instance_id()` 中央映射表；各接收方注册帧监听时自行声明该帧是否含 `instance_id`。声明与写端布局不一致时，解码失败按坏帧处理（WARN 丢帧，不影响其他帧）。新增帧时写端与全部接收方必须一致登记。

---

## 4. 传输与数据流

### 4.1 逻辑角色与介质

| 角色 | 说明 |
|------|------|
| 前端 | WebUI / Qt UI，经 REST（契约 rest）与 WS（契约 webui-ws）与 dzweb 通信 |
| dzweb | WebUI 后端进程。逻辑组件：REST/WS 入口（controller）、领域服务（镜像更新 + WS 广播）、事件监听（事件通道读侧） |
| master | 进程管理进程（`dztraderd`），事件通道配置宿主 |
| 网关进程 | `dzmd_*`（行情）/ `dztd_*`（交易） |
| 策略进程 | `stg.*` |
| 事件通道 | SHM 内存文件映射的**广播介质**（通道名真相源：`core_data_type.h` 常量 `CHANNEL_NAME_EVENT`；多进程写）：无路由器，任何进程写入即唤醒全部等待进程（NamedSemaphore），接收方按 `frame_type` 与 `instance_id` 自行过滤。契约中的"方向"描述逻辑收发方，物理上帧对所有等待进程可见 |
| 行情数据通道 | 每行情源一个，单行情进程写，承载 tick/深度等高频数据帧（本轮契约不覆盖，见 §10 遗留） |

通用规则：

- 写入方义务：写帧后必须唤醒等待进程；接收方义务：只处理自己注册的帧类型与匹配的 `instance_id`。
- 本目录全部契约覆盖的帧（除行情数据帧）均走事件通道。
- 目标实例不存在/未启动 ⇒ 帧无人认领被丢弃 ⇒ 无 RTN ⇒ 前端 pending 超时兜底（§7 兜底规则的物理成因）。

### 4.2 标准链路形态

各契约"数据流"字段引用下列形态编号并只写差异；未提及的环节按形态定义执行。

**形态 1：标准 SET→RTN 环路**（前端发起的设置/查询类请求）

```
请求侧：前端 →(REST 契约 rest / WS 契约 webui-ws)→ dzweb 入口
        →(写 SET/REQ 帧 + 信号量唤醒)→ 事件通道
        →(全部等待进程唤醒；仅 frame_type 与路由键匹配者处理：instance_id 或 payload 目标字段)→ 目标进程 handler
响应侧：目标进程 handler →(写 RTN 帧 + 唤醒)→ 事件通道
        →(dzweb 事件监听)→ 领域服务 →(镜像 domain 更新)→ WS 广播 → 前端（清 pending）
```

- REST/WS 响应仅表示"已写事件通道"，生效信号 = 对应 WS 领域消息（契约 webui-ws §4）。
- 同一请求存在 REST 与 WS 两个等效入口时行为一致。

**形态 2：dzweb 自身短路**（目标 = dzweb）

```
前端 →(REST/WS)→ dzweb 入口 →(直调本进程处理，不经事件通道)→ 帧语义决定是否回推
```

- SET 类帧回推镜像 + WS（与形态 1 响应侧等价）；纯动作帧（如 FLUSH_LOG）无回推。

**形态 3：单向下行，无 RTN**（前端发起，fire-and-forget）

```
前端 →(REST/WS)→ dzweb 入口 →(写帧 + 唤醒)→ 事件通道 →(instance_id 匹配者)→ 目标进程
```

结果经其他帧（如 `RTN_PROGRESS`）体现或无反馈。`QUERY_FULL_SNAPSHOT`（dzweb 启动自发广播，§7）为本形态的无前端入口变体。

**形态 4：纯上行推送**（进程自发，无前端入口）

```
进程 →(写 RTN 帧 + 唤醒)→ 事件通道 →(dzweb 事件监听)→ 领域服务 →(镜像 domain 更新)→ WS 广播 → 前端
```

查询类响应（如 `RTN_MD_SUBSCRIPTIONS`，契约 md-subscription）为本形态的"不进镜像"变体：dzweb 注入 `source` 后透传广播。

**形态 5：后台进程间帧**（不经 dzweb、不进镜像、无前端参与）

```
进程 A →(写帧 + 唤醒)→ 事件通道 →(按 frame_type + instance_id / payload 归属自取)→ 进程 B
```

**形态 6：NOTIFY_UI 上行**（经 dzweb，不进镜像）

```
进程 →(写帧 + 唤醒)→ 事件通道 →(dzweb 事件监听)→ 通知缓存 + WS 广播 `notify_ui` → 前端
```

### 4.3 契约内引用格式

各契约以 `**数据流**：形态 N（总则 §4.2）` 引用，其后只写差异：逻辑方向与 `instance_id` 归属、前端入口端点（无则注明）、响应帧 → 镜像 domain → WS 消息名映射、短路/特例。

---

## 5. `instance_id` 规范

- 逻辑实例分两级：**进程级** = 进程名（如 `dzmd_ctp`、`dztd_ctp`）；**账户级** = 账户 ID（交易多账户场景）。
- 进程名由命名规则约束（`dzmd_*`/`dztd_*`/`stg.*` 等）。
- 账户 ID **全局唯一**（跨全部接口类型，如 CTP / XTP 不冲突），作为账户级实例的唯一标识，不携带进程名；账户归属进程由业务配置 / 路由推断，不由 `instance_id` 承载。
- 策略实例 ID = `stg.<name>`（无 pid 后缀，重启复用同名；master 与策略 SDK 必须构造同名）。
- 订阅者（读者）身份 = 进程 instance_id（策略 `stg.<name>`，其余进程为进程名），与该进程的事件通道信号量名一致（见契约 shm）。
- **身份边界**：`stg.<name>` 前缀**仅用于内存中的订阅者/信号量/reader/md 订阅者身份**（区分策略与系统组件，同一事件通道命名空间防冲突）。策略帧的帧头 `instance_id`、payload 内嵌 `strategy_id` 字段、展示层与 SDK 接口一律用**裸策略名**（策略名在全部策略中唯一，如 `stg_demo`；本目录《帧契约：策略》《帧契约：UI 通知》按此执行）。
- 同一逻辑实例在全部契约中必须使用同一 `instance_id`（日志、SHM 配置、进度、镜像 key 对齐）。
- 帧头无 `instance_id` 的帧不得依赖"帧来源"路由；来源/目标必须显式在 payload 中携带（如 `target`/`name`/`source` 字段）。

---

## 6. Payload 编码规则

两种编码，判定标准固定：

| 编码 | 判定 | 真相源 |
|------|------|--------|
| JSON | 前后端通信帧（UI 可读、需校验/合并语义） | **schema 在本目录对应契约**；代码校验函数引用契约 |
| struct | 后台进程间通信、高频、策略 API 消费 | 对应头文件（`struct.h`/`core_struct.h`/`platform/*.h`）；契约只写帧名+引用，不重复字段 |

规则：
- struct 帧的字段定义只在头文件中出现一次；契约禁止抄写字段表。
- JSON 帧的 schema 只在契约中出现一次；代码不得另立一套字段语义。
- JSON 数值在目标类型范围内即可（不区分 int32/uint32/int64/uint64）。
- JSON 字段名与 platform 头文件类型保持一一对应（`platform/*.h` 是 JSON 帧的类型层真相源，见各契约引用）。

---

## 7. 请求-响应总则

- **SET/REQ 类帧必回对应 RTN**（前端 pending 以 RTN 为准）；RTN 一律全量（当前生效值）。
- 例外（无 RTN 的请求/通知帧）在各自契约声明；无 RTN 帧必须说明接收方如何获知结果（如进程退出状态、进度推送）。
- **极端情况兜底**：目标实例不存在/内部异常时 RTN 可能缺失；所有"必回 RTN"的 SET/REQ 均隐含"前端以 pending 超时兜底"，不逐契约重复。
- **快照查询** `DZ_FRAME_QUERY_FULL_SNAPSHOT`：dzweb 启动时广播（无 `instance_id`，空 payload），各进程按自身契约上报对应 RTN（触发场景"QUERY_FULL_SNAPSHOT 响应"），无统一响应帧。
  - **快照不承诺完整性**：`snapshot`（WS）始终是"建连时刻 dzweb 已知镜像"；尚未上报/尚未启动的进程域由后续 RTN 增量补齐（前端幂等覆盖）。进程启动初始化完成后主动上报（各契约触发场景 2）是完整性收敛的机制。
  - 前端页面刷新不重新广播本帧，直接查询 dzweb 镜像。
- **并发语义**：同一实例的 SET 由接收方串行应用；前一 SET 未回 RTN 前收到后一 SET，允许排队或拒绝，具体见各契约（未声明则允许排队，RTN 顺序 = 应用顺序）。

---

## 8. 校验失败与错误处理总则

校验失败/持久化失败/状态保护拒绝的**四件套**（各契约简称"四件套"）：

1. 回滚旧值（内存镜像不变）
2. 日志（必须）
3. `NOTIFY_UI` 错误级别弹窗（必须，`popup=true`）
4. 回 RTN（旧值/回滚后值）

其余总则：
- **`null` 语义**：默认任何位置出现 `null` 均为校验失败；仅当契约显式允许时例外（如 `env` 内部 key 的 value 为 null 表示删除、查询响应 `error` 字段成功路径为 null）。
- **未知字段**：JSON patch/payload 中的未知字段一律忽略（前向兼容）；此规则适用于全部 JSON 帧，不逐契约重复。
- **空对象 patch `{}`**：无操作，校验通过，仍回 RTN（当前值）。
- 失败原因的传达方式（`message` 字段 / `error` 字段 / 仅 NOTIFY_UI）由各契约声明，且每帧必须二选一：要么在 RTN 中携带错误，要么声明"无 RTN 兜底 + NOTIFY_UI"。

---

## 9. 镜像总则（dzweb）

- dzweb 为每个实例维护镜像（key = `instance_id`，无 `instance_id` 帧挂固定 `dztraderd`）；收到 RTN 更新对应 domain 并 WS 推送。
- RTN 全量覆盖：前端直接整体覆盖本地镜像，覆盖天然含删除。
- 镜像 domain 名与 WS 消息的映射见契约 webui-ws；镜像不进"高频数据"。
- 仅查询响应（如订阅详情）不进镜像，dzweb 透传广播。

---

## 10. 本轮范围与遗留

- 本目录当前覆盖事件通道的低频控制/配置/通知帧。
- **已覆盖**：策略帧（`STG_USER_INPUT`/`STG_USER_OUTPUT`/`SET_LOGICAL_POSITION`，见《帧契约：策略》）。
- **未覆盖**（后续独立契约，本目录暂不收录）：交易帧（除《帧契约：交易委托请求》已覆盖的 `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` 外，其余 2000-2114，TD 已实现大半）、行情/交易数据帧（`RTN_MD_TICK`、TD 推送 2000-2004，struct payload）。
- `DZ_FRAME_SYS_SCHED`：帧类型与 payload（`DzSysSched`）保留但**当前无任何进程消费**（md 已于 2026-07 移除处理）；未来 master 集中调度若启用，需另行定契约。新读者不得假设其语义。

---

## 11. 契约编写与变更规则

### 11.1 每份契约的固定模板

1. **覆盖帧**：帧名清单（帧号不写，见 §3）
2. **语义/数据流/路由**：引用 §4.2 链路形态编号 + 差异（逻辑方向、`instance_id` 语义、前端入口端点、响应→镜像→WS 映射；后台帧注明广播或定向）
3. **Payload**：JSON schema（字段表 + 示例）或 struct 引用
4. **校验**：仅写与总则不同的规则
5. **时序与触发**：响应帧、触发场景（含"QUERY_FULL_SNAPSHOT 响应"时只写差异）
6. **镜像**：dzweb 镜像 domain 与 key 规则
7. **前端义务**（如适用）：仅跨进程可观察行为（pending 清除由哪个消息触发、超时兜底、错误展示来源），不写视觉细节

### 11.2 锚点

- 规则用 `§` 编号锚定；**禁止以行号引用契约**（行号随编辑漂移）。
- 跨契约引用格式：《帧契约：\<主题\>》§N。

### 11.3 变更 checklist

修改契约必须同步检查：

1. `libs/platform/include/dztrader/platform/*.h` 类型/校验函数（JSON 帧）
2. 帧号/帧头登记（`data_type.h`/`core_data_type.h`/接收方注册处）
3. dzweb 领域服务（镜像 domain + WS 广播）
4. 前端 store/组件
5. 相关单测与 frame_types 测试
6. 契约 webui-ws 与 rest 的 WS/REST 映射表

契约定稿后，实现与契约的冲突（含既有实现偏离）以契约为准；修实现需走对应模块流程（`apps/ctp/md` 另有锁定规则）。
