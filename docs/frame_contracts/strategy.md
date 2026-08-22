# 帧契约：策略

本文件覆盖 `DZ_FRAME_STG_USER_INPUT`、`DZ_FRAME_STG_USER_OUTPUT`、`DZ_FRAME_SET_LOGICAL_POSITION` 三个帧。总则见《帧契约：通用规则》。

- `STG_USER_INPUT` / `STG_USER_OUTPUT`：策略与 UI 之间的交互文本/数据帧，使用 `DzExtInstFrameHeader` 扩展头，`instance_id` = **裸策略名**（如 `stg_demo`，策略间唯一）。
- `SET_LOGICAL_POSITION`：**basic 帧**（仅 `DzFrameHeader`，无扩展头），策略身份在 payload `strategy_id` 字段，同样为裸策略名。

**身份边界**（总则 §5）：本契约全部帧的身份字段（帧头 `instance_id`、payload `strategy_id`）一律用**裸策略名**——策略帧只属于策略域，不会与其他进程混淆，策略名在全部策略中唯一。`stg.<name>` 前缀**仅用于内存中的订阅者/信号量/reader/md 订阅者身份**（与系统组件区分），不出现于策略帧、展示层与 SDK 接口。

类型层真相源：`libs/strategy_api/include/dztrader/struct.h`、`libs/core/include/dztrader/core/core_struct.h`（`DzLogicalPosition`）。

---

## DZ_FRAME_STG_USER_INPUT

**语义**：向指定策略进程投递用户输入（UI → 策略）
**数据流**：形态 3（总则 §4.2）——前端 → dzweb → 目标策略进程（帧头 `instance_id` = 目标裸策略名，如 `stg_demo`）；无 RTN，单向投递（fire-and-forget）
**Payload**：变长 UTF-8 文本或 JSON（策略自行组织格式），无固定 schema

**时序**：无同步响应、无 RTN；本契约不规定策略的响应义务，前端不设 pending

**约束**：
- 定向帧，仅 `instance_id` 匹配的策略进程处理（策略 SDK 按自身裸策略名匹配）
- 策略侧接收 API 由 SDK 提供（当前未实现，本帧为契约先行的占位语义）
- 发送方（dzweb）未知策略是否存活时，帧可能无人认领被丢弃（总则 §4.1）

**镜像**：不进 dzweb 镜像

---

## DZ_FRAME_STG_USER_OUTPUT

**语义**：策略向 UI 自主输出数据（策略 → UI，类似 stdout），对应 `dz_output_ui`
**数据流**：形态 6 变体（总则 §4.2）——策略进程 → dzweb 转发 → 前端（帧头 `instance_id` = 来源裸策略名）；无前端入口；无 RTN
**Payload**：变长 UTF-8 文本或 JSON（策略自行组织格式），无固定 schema

**约束**：
- `instance_id` = 来源裸策略名（与 dzweb 进程镜像 key 一致，零转换关联）
- 与 `NOTIFY_UI`（契约 notify-ui）的区别：`NOTIFY_UI` 是通知消息（有级别/弹窗/时间戳，进通知缓存）；`STG_USER_OUTPUT` 是策略自主上行输出（无级别无弹窗，不进缓存）；是否响应 UI 输入由策略自行决定，本契约不定义配对关系
- 接收方（dzweb）当前未消费本帧（`dz_output_ui` 写帧但 dzweb 无对应处理，见 README 范围与遗留）；契约定义语义，实现滞后由 general §11.3 checklist 跟踪

**镜像**：不进 dzweb 镜像

---

## DZ_FRAME_SET_LOGICAL_POSITION

**语义**：策略上报自身逻辑持仓（策略认为应持有的仓位），用于 UI 监控策略持仓与账户实际持仓的偏差，对应 `dz_set_logical_position`
**数据流**：形态 4（总则 §4.2）——策略进程 → dzweb（basic 帧，策略身份在 payload `strategy_id`）；无前端入口；无 RTN
**Payload**：struct `DzLogicalPosition`（真相源：`libs/core/include/dztrader/core/core_struct.h`，非 JSON；字段表不重复）

**约束**：
- basic 帧：仅 `DzFrameHeader` + struct payload，无扩展头；来源策略由 payload `strategy_id` 携带（总则 §5：帧头无 `instance_id` 的帧不得依赖帧头路由）
- `strategy_id` = 裸策略名，与 `DzOrderReq.strategy_id`/`DzOrderReport.strategy_id`（TD 透传裸名）一致，供 dzweb 关联订单 ↔ 逻辑持仓
- `account_id` / `instrument_id` 为空串表示通配（清空该维度全部逻辑持仓，`net_volume` 忽略固定为 0），语义与 SDK 侧 `dz_set_logical_position` 文档一致
- 接收方（dzweb）当前未消费本帧，契约定义语义先行

**镜像**：dzweb 未来按 `strategy_id` 维护逻辑持仓镜像（当前未实现）

---

## 变更流程

本契约随策略帧实现演进；修改必须执行 general §11.3 变更 checklist。
