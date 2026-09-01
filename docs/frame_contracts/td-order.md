# 帧契约：交易委托请求

本文件覆盖 `DZ_FRAME_TD_ORDER_REQ`、`DZ_FRAME_TD_ORDER_CANCEL_REQ` 两个帧。总则见《帧契约：通用规则》。

与事件通道其他控制/配置帧不同，这两个帧是 **basic 帧**（仅 `DzFrameHeader`，无 `instance_id` 扩展头）：策略进程广播写入，交易网关按 payload 中的 `account_id` 归属路由。

---

## 语义 / 数据流 / 路由

| 帧 | 逻辑方向 | 路由方式 |
|---|---|---|
| `DZ_FRAME_TD_ORDER_REQ` | 策略进程 → 所有交易网关 | 广播 + payload `account_id` 归属过滤 |
| `DZ_FRAME_TD_ORDER_CANCEL_REQ` | 策略进程 → 所有交易网关 | 广播 + payload `account_id` 归属过滤 |

- **数据流**：形态 5（总则 §4.2）——策略进程广播写入，交易网关按 payload `account_id` 归属自取（无 `instance_id` 路由）。
- **发送方**：策略进程（`libs/strategy_api` 的 `dz_place_order` / `dz_cancel_order`），写入后唤醒事件通道订阅者，所有进程可见。
- **接收方**：每个交易网关进程（`dztd_*`）读所有这类帧，只处理 `account_id` 存在于本进程配置 `config_.accounts` 中的请求；其余忽略（DEBUG 日志），不产生任何响应。
- **不使用** `DzExtInstFrameHeader`：网关对这两个帧一律按 basic 布局解析。
- 发送方无需知道网关进程名：网关重命名（如 `dztd_ctp_2`）不影响策略下单；多网关实例共存时各实例按自身配置独立过滤。

## Payload

struct 引用（不抄写字段表，字段定义见 `libs/core/include/dztrader/core/core_struct.h`）：

- `TD_ORDER_REQ` → `DzOrderReq`（路由字段 `account_id`）
- `TD_ORDER_CANCEL_REQ` → `DzOrderCancelReq`（路由字段 `account_id`，定位字段 `order_id`）

## 校验（仅写与总则不同的规则）

接收方校验顺序：

1. `frame_size` 不足 `sizeof(DzFrameHeader) + sizeof(payload struct)` → 拒绝（WARN）
2. payload `account_id` 为空串 → 拒绝（WARN）
3. `account_id` 不在本进程配置 → 忽略（DEBUG），不报错、不回报
4. 账户无运行中 session → 拒绝（WARN）

## 时序与触发

- **响应**：无同步响应帧。下单/撤单结果由 `DZ_FRAME_ORDER_REPORT`（含 `REJECTED` 状态）异步推送，该帧契约见后续交易帧契约（当前未收录）。`ORDER_REPORT`/`TRADE_REPORT` 的 `strategy_id` 由 td 网关按下单 `DzOrderReq.strategy_id` 回填（td 侧 OrderRefMap 按 `DzOrderId` 关联），外部单/手工单（非任何策略所下）该字段为空串；策略 SDK 按 `strategy_id` 定向过滤（契约 strategy）。
- **当前拒绝行为**：账户不在配置 / 无 session 时静默丢弃（仅日志）；错误回报与拒单通知的完整语义由后续交易帧契约补齐。
- **触发场景**：仅策略主动下单/撤单；本契约不响应 `QUERY_FULL_SNAPSHOT`。

## 镜像

- 不进 dzweb 镜像（高频业务帧，见《帧契约：通用规则》§9）。
