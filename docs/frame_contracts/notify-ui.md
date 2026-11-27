# 帧契约：UI 通知

覆盖 `DZ_FRAME_NOTIFY_UI` 一个帧。单向通知（所有进程可见，dzweb 处理），无 RTN 配对。总则见《帧契约：通用规则》。

帧使用 `DzExtFrameHeader` 扩展头（无 `instance_id`），发送方身份在 payload 的 `source` 字段。

发送侧真相源：`libs/platform/include/dztrader/platform/notify_ui.h`（`NotifyUi`：`error` 默认 `popup=true`，`warn`/`info` 默认 `popup=false`，均可显式覆盖）。

---

## DZ_FRAME_NOTIFY_UI

**语义**：向 UI 推送通知消息
**数据流**：形态 6（总则 §4.2）——各进程 → dzweb → 前端；无前端入口；不进镜像，dzweb 转发 payload 并 WS 广播 `notify_ui`
**Payload**：JSON

| 字段 | 类型 | 说明 |
|------|------|------|
| `source` | string | 通知来源（进程名或裸策略名，总则 §5 身份边界） |
| `level` | string | 级别：`"info"`/`"warning"`/`"error"`（与《帧契约：日志》的 log level 规范全称一致） |
| `message` | string | 通知正文 |
| `timestamp` | time_t | Unix 秒级时间戳 |
| `popup` | bool | 是否**常驻**呈现（不自动消失，需用户手动关闭）；**不是**「打断用户」（见「前端义务」）|
| `count` | int | 可选，默认 `1`。同一事件被去重折叠的次数；`1` 表示未折叠。**由 dzweb 填**（发送侧不填），见「dzweb 处理」 |

**时序**：无 RTN

**约束**：
- 纯通知：**不携带任何 pending 清除语义**——前端仅显示消息，不清除任何进行中的请求状态
- `source` 在 payload，不在帧头；dzweb 收到后直接转发 payload，不注入/改写 source（`count` 除外，它是 dzweb 的唯一写入字段）
- 发送侧 fire-and-forget：写入失败仅记日志，无重试、无反馈
- **同一事件只有一个级别**：禁止同一事件既发 `warning` 又发 `error`。持续故障靠 `count` 折叠表达，不靠降级级别（ADR 0060 决策 2）
- 产生侧是**显式白名单**，不从日志级别自动桥接（ADR 0060 决策 1）

**dzweb 处理**：
- 收到后 WS 广播（消息名 `notify_ui`，见契约 webui-ws）
- 存入最近通知环形缓存（供前端查询历史通知，条数可配）；**不进镜像**（不在 snapshot 中）
- 解析失败仅记日志丢弃（防御性，不广播）
- **合并去重**（唯一漏斗，ADR 0060 决策 4）：指纹 = `source + level + 归一化文本`，归一化 = 抠掉订单号 / 时间戳 / 价格等动态值；同一指纹在**窗口**内合并为一条并 `count++`、**不重复广播**，只就地更新该条的 `count` 与 `timestamp` 后广播一次；窗口内重复**更新最后发生时间**；同一指纹经**冷却**后才允许作为新条目重新出现。窗口与冷却时长可配（`webui.json`）
- 「已转发」轨迹日志用 DEBUG：转发不是独立事件，不得以 `warning` 形态再报一次（ADR 0060 决策 5）

**前端义务**：
- `popup=true` 时必须**常驻**呈现（不自动消失，直到用户确认或关闭）；**不得以全屏遮罩、居中模态等阻断操作的形态呈现**（ADR 0060 决策 3）
- 其余按级别短暂呈现（即时提示 / toast）
- 显示内容以 payload 为准，不得依赖缓存顺序重建状态
- `count > 1` 时应就地更新已有条目的计数，不得新增一条
