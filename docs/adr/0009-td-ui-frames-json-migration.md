# ADR 0009: UI 协议帧（出入金/改密/风控拒绝）payload 由二进制结构体迁移为 JSON

## Status

Accepted

> 注: 本文件在 2026-09-11 的一次工具事故中被截断，此处按 `struct.h` 的删除结果、
> 平台头实现与两个 GoldenJson 测试重建要点；原始论证细节若与作者记忆不符，以作者为准。

## Context

`DZ_FRAME_TD_TRANSFER_*` / `DZ_FRAME_TD_PASSWORD_UPDATE_*` / `DZ_FRAME_TD_RISK_REJECT`
五帧原先用 `DZ_DECLARE_ALIGNED_STRUCT` 定长结构体（`libs/strategy_api/include/dztrader/struct.h`）
承载：定长 `char[N]` 字段、`reserved` 对齐补丁、`int8_t password_type` 单字符编码。

问题：这些帧是 UI 协议帧（dzweb ↔ td），低频、面向人机交互，定长布局带来
字段容量上限（如 `bank_account[32]`）、对齐补丁与单字符枚举，且新增字段即破坏二进制兼容；
它们不参与策略热路径，定长布局的性能收益无意义。

## Decision

五帧 payload 改为 **JSON 编码**，类型真相源迁移到平台头：

| 帧 | 新类型真相源 |
|---|---|
| `DZ_FRAME_TD_TRANSFER_REQ` / `_RSP` / `_RTN` | `libs/platform/include/dztrader/platform/td_account_ops.h` |
| `DZ_FRAME_TD_PASSWORD_UPDATE_REQ` / `_RSP` | 同上 |
| `DZ_FRAME_TD_RISK_REJECT` | `libs/platform/include/dztrader/platform/risk_reject.h` |

- 类型命名由 `TdXxx` 改为 `DzXxx`（`DzTransferReq`/`DzTransferRsp`/`DzPasswordUpdateReq`/
  `DzPasswordUpdateRsp`/`DzRiskReject`），避免与 CTP 结构体同名混淆。
- 五帧均为**无 `instance_id`** 的 `DzExtFrameHeader` ext 帧，写读用
  `platform::write_ext_json` + `nlohmann::json`，接收方按 payload `account_id` 过滤。
- `struct.h` 中五个 `DZ_DECLARE_ALIGNED_STRUCT` 定义**删除**（含 `reserved` 补丁）；
  策略 SDK 对这五帧仍按"非策略可见"直接丢弃（契约 strategy）。
- 序列化用 `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT`：缺失字段回退默认值、
  整体 `null` 解码为全默认结构（不抛错）——因此**接线时 handler 必须显式校验必填字段**，
  不得依赖解码错误（契约 td-account-ops / td-risk-reject "校验"节）。
- 字段 schema 的回归由两个 GoldenJson 测试锁定：
  `libs/platform/tests/td_account_ops_test.cpp`、`libs/platform/tests/risk_reject_test.cpp`。

## Alternatives

- **保留定长结构体，仅加大字段容量**：容量问题反复出现、仍受对齐补丁与单字符枚举拖累，
  且新增字段仍破坏二进制兼容。**否决**。
- **混合（REQ 用 struct、RSP/RTN 用 JSON）**：同一组帧两套编码，解码路径分裂难维护。**否决**。

## Consequences

- 五帧的 `struct.h` 定义删除属**破坏性变更**：旧版本进程与本版本不能混跑（本项目不保留兼容层）。
- 字段语义不受容量限制（银行账号/错误描述/拒绝原因改为 `std::string`）；
  `password_type` 由单字符改为字符串（`"U"`/`"A"`）。
- 安全约束不变：敏感字段仍在 SHM 内明文传输，禁止落日志与 WS/REST 回显（契约"安全约束"节）；
  RSP/RTN 不含任何密码字段。

## References

- 契约：`docs/frame_contracts/td-account-ops.md`、`docs/frame_contracts/td-risk-reject.md`
- 类型层：`libs/platform/include/dztrader/platform/{td_account_ops.h,risk_reject.h}`
- 回归测试：`libs/platform/tests/{td_account_ops_test.cpp,risk_reject_test.cpp}`
