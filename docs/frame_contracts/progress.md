# 帧契约：进度推送

本文件覆盖 `DZ_FRAME_RTN_PROGRESS` 一个帧。总则见《帧契约：通用规则》。

帧使用 `DzExtInstFrameHeader` 扩展头，`instance_id` 标识来源逻辑实例（进程名；TD 账户级为账户 ID，账户 ID 全局唯一，见总则 §5）。

类型层真相源：`libs/platform/include/dztrader/platform/progress.h`（`ProgressReporter`）。

仅 RTN 推送，无 SET、无持久化、无文件 IO。进程在状态转移 / 进度里程碑时主动推送。

---

## 数据结构

### ProgressStatus

单条完整推送。

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `min` | int | 是 | 进度最小值（通常 0） |
| `max` | int | 是 | 进度最大值；`max <= min`（含 `max==0`）表示不确定进度 |
| `current` | int | 是 | 当前进度 |
| `desc` | string | 否 | 简短文本说明（如 `"订阅合约中"`）；空串/省略 = 无文本 |

**进度语义**：

- `max > min`：确定进度
- `max <= min`（含 `max == 0`）：不确定进度；`current` 值忽略
- `current` 应在 `[min, max]` 范围内；超出时前端钳制到 `[min, max]`（防御性，不报错）；钳制仅适用于确定进度

**消费者约定**：

- `desc` 仅为展示文案，**消费者不得依赖 `desc` 文本做状态判定**（文案调整不得破坏协议语义）
- 需要判定业务状态时使用 `min`/`max`/`current` 数值映射；映射表的真相源由发送方进程维护（实现注释/进程文档），映射变更必须同步所有消费者

JSON 示例：

```json
{"min": 0, "max": 4, "current": 2, "desc": "订阅合约中"}
{"min": 0, "max": 0, "current": 0}
{"min": 0, "max": 10, "current": 10, "desc": "就绪"}
```

---

## DZ_FRAME_RTN_PROGRESS

**语义**：推送当前进度状态
**数据流**：形态 4（总则 §4.2）——进程（帧头 `instance_id` = 来源，TD 账户级为账户 ID）→ dzweb；无前端入口；镜像 `progress` 域 → WS 消息 `progress`
**Payload**：JSON（ProgressStatus）

**触发场景**：

1. 进度变化时推送（状态转移、步骤推进、完成、失败等里程碑）
2. `QUERY_FULL_SNAPSHOT` 响应：有活跃进度时推送；无活跃进度时可不推送（总则 §7）

**约束**：

- 单条完整状态：接收方直接覆盖该实例的进度，无需增量合并；后到的覆盖先到的，不累积
- 进程无进度追踪需求时可不实现本帧（不影响其他帧）

**镜像**：
- dzweb 更新该实例镜像的 `progress` 域，并 WS 推送

**前端义务**：
- 以最新推送为准展示；镜像/快照中无 `progress` 域的实例视为无活跃进度
- 超出范围的 `current` 钳制展示，不报错、不反馈
