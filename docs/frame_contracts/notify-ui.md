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
| `popup` | bool | 是否弹窗打断用户 |

**时序**：无 RTN

**约束**：
- 纯通知：**不携带任何 pending 清除语义**——前端仅显示消息，不清除任何进行中的请求状态
- `source` 在 payload，不在帧头；dzweb 收到后直接转发 payload，不注入/改写 source
- 发送侧 fire-and-forget：写入失败仅记日志，无重试、无反馈

**dzweb 处理**：
- 收到后 WS 广播（消息名 `notify_ui`，见契约 webui-ws）
- 存入最近通知环形缓存（供前端查询历史通知，条数可配）；**不进镜像**（不在 snapshot 中）
- 解析失败仅记日志丢弃（防御性，不广播）

**前端义务**：
- `level=error` 且 `popup=true` 时必须打断用户展示；其余 toast 展示
- 显示内容以 payload 为准，不得依赖缓存顺序重建状态
