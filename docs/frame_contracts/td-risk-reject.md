# 帧契约：风控拒绝通知

本文件覆盖 `DZ_FRAME_TD_RISK_REJECT` 一个帧。单向通知（td 风控门 → 事件通道广播，dzweb 处理），无 RTN 配对。总则见《帧契约：通用规则》。

帧使用 `DzExtFrameHeader` 扩展头（**无 `instance_id`**），接收方按 payload `account_id` 过滤（账户 ID 全局唯一，总则 §5）。

类型层真相源：`libs/platform/include/dztrader/platform/risk_reject.h`。

**实现状态（未接线）**：td 已产出本帧（写侧落地）；dzweb 无消费、无镜像 domain、无 WS 消息、前端无展示。

---

## DZ_FRAME_TD_RISK_REJECT

**语义**：委托请求被本地风控门拒绝时通知 UI（撤单路径当前未接风控——`RiskGate::check_cancel` 无生产调用，本帧仅由委托触发；接线撤单风控时需同步扩展本契约）
**数据流**：形态 4（总则 §4.2）——td → dzweb；消费、镜像 domain、WS 消息均**未接线**
**Payload**：JSON

| 字段 | 类型 | 说明 |
|------|------|------|
| `account_id` | string | 被拒请求所属账户 |
| `rule_name` | string | 触发的规则名（如 `"max_order_volume"`） |
| `reason` | string | 拒绝原因（展示文本，UTF-8） |
| `timestamp_ns` | int | Unix 纳秒时间戳；前端 JS 有效整数上限 2^53，dzweb 镜像/WS 转发需转 ms 或字符串 |

```json
{
  "account_id": "account_001",
  "rule_name": "max_order_volume",
  "reason": "order volume 500 exceeds limit 100",
  "timestamp_ns": 1757500000000000000
}
```

**时序**：无 RTN。风控门拒绝后 td 走本地拒单路径：产生拒单终态的 `ORDER_REPORT`（契约 td-data-sync）并额外写本帧；**不向 CTP 发送**委托请求（设计 §8.2）

**镜像与前端义务**：未接线——dzweb 镜像 domain、WS 消息与前端展示均未建立（见"实现状态"）

**约束**：
- 本帧不含 `order_id`/`instrument_id`，UI 无法关联具体被拒委托（当前语义；扩展需改 schema 与契约）
- 非策略可见：策略 SDK 直接丢弃（《帧契约：策略》拦截清单）
- 发送侧 fire-and-forget：写入失败仅记日志，无重试、无反馈
