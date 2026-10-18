# ADR 0021: 策略 SDK 不提供日志能力（撤销公开 `dz_diag`）

## Status

Accepted（2026-10-19）

## Context

- 现状：SDK 公开头 `api.h` 导出 `DZ_API void dz_diag(const char* message)`，实现固定往 stderr 写一行并加 `[dzsdk]` 前缀。master 捕获全部子进程 stdout/stderr 转发进日志（stdout→info、stderr→warn，logger `<master>.fwd`，[child_process.cpp](../../apps/master/child_process.cpp)）。
- 该函数最初（commit 0154f07）是为消除 `strategy_context.h` 与 `strategy_engine.h` 两处逐字节重复的内部诊断实现；之所以导出成 `DZ_API`，是因为 `strategy_engine.h` 是 header-only 模板，实例化发生在用户 TU，必须从动态库解析符号。
- 用户诉求（2026-10-19）：策略作者自己的日志不统一整合（跨语言、需求各异，成本高），**本阶段不在 SDK 提供任何日志能力**；但 SDK 内部失败仍需兜底可见。
- 事实：策略进程 stdio（用户 `cout/cerr/clog/printf`）已被 master 全量捕获转发；转发日志行已带 `child_pid`/`child_entry`，按策略定位不依赖额外标记。

## Decision

1. **SDK 不提供日志能力**：删除公开面 `dz_diag`（声明与导出符号一并删除）；策略作者自行决定日志实现，写 stdio 即由 master 兜底转发。
2. **SDK 内部诊断直写 stderr**：引擎模板与 SDK 实现在异常/降级路径直接 `std::cerr` 写英文单行诊断（沿用既有日志文本规范），不再保留统一诊断函数、内部头或来源标记。选 `std::cerr` 是因为：去掉标记后 helper 无剩余职责；所有调用点使用同一标准机制，不存在两套自定义实现；不新增随包分发的“内部”头。
3. **不做来源识别**：去掉 `[dzsdk]` 前缀，master 不解析、日志格式不变；“哪个策略的什么信息”由既有 `child_pid=`/`child_entry=` 与消息文本满足。
4. **用户输出不整合**：SDK 不承诺级别/格式/编码/轮转；master 现有 stdio 转发即兜底。

## Alternatives

- **保留 `dz_diag` 公开（现状）**：公开面出现一个只服务内部诊断的打印函数，外部策略作者会把它当日志 API。**否决**。
- **统一日志 API（level/格式/UI）**：用户明确本阶段不做（跨语言、需求差异、成本高），将来另行立项。**否决（本阶段）**。
- **保留 `[dzsdk]` 标记 + master 解析成 `child_origin` 字段**：需要一条跨进程约定（契约 + master 解析 + SDK/master 版本耦合），而“来源”维度没有消费者——用户日志不整合，按策略定位已由 `child_entry` 满足。**否决**。
- **专用 fd/通道传输 SDK 诊断**：master 可天然区分且不可伪造，但 Windows 额外句柄继承、手工运行无 master 时无 fallback，成本远大于收益。**否决**。
- **保留内部 inline helper（不导出）**：单一改动点，但需新增随包分发的内部头，或让 C API 实现依赖 C++ 引擎头；且无标记后它只剩“写一行 stderr”。**否决**（`std::cerr` 已是同一机制）。
- **保留导出符号、仅把声明移出 `api.h`**：ABI 不变，但公开面留一个说不清身份的导出符号。**否决**。

## Consequences

- **ABI**：`dzstrategy_sdk` 少一个导出符号 `dz_diag`；旧策略二进制需随新 SDK 重新编译（策略与 SDK 同版本发布，见 ADR 0020；0.0.1 阶段不做兼容层）。SONAME/导出表收敛仍由 #7 负责。
- 策略作者没有结构化日志 API：需要日志时自行实现（写 stdout/stderr 由 master 转发兜底，或走 `OUTPUT_UI`/`NOTIFY_UI` 进 UI 通道）。
- 日志里 SDK 内部诊断与策略自身 stderr 不可机器区分；定位策略靠 `child_entry`，内容靠文本。将来若需结构化过滤，可解析既有的 `child_entry=`，不涉及 SDK 或协议。
- 遗留（不在本 ADR 范围）：用户日志统一整合；子进程输出编码（Windows 系统代码页 vs UTF-8）；单行长度上限（master 读端当前无界）；stdout/stderr 跨流顺序；`.fwd` logger 级别不随运行期 `SET_LOG_CONFIG` 变化。

## References

- [ADR 0020: 策略 SDK 交付形态与公开面](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)
- commit 0154f07（原 `dz_diag` 导出动机）
- [子进程 stdio 转发实现](../../apps/master/child_process.cpp)
