# ADR 0022: 策略 SDK 公开面收敛（C 波）——移除 `dz_md_source_name`、`dz_notify_self` 改名 `dz_wakeup`

## Status

Accepted（2026-10-19）

## Context

- 2026-10-19 grill：对 `api.h` 全部 51 个 `DZ_API` 函数做消费者证据审计（仓内生产调用、doxygen、测试、契约提及、内部能力依赖）。除引擎直接调用的 7 个函数外，仓内调用全部来自示例策略或测试；`apps/`、`libs/` 其他模块从不 include `api.h`。SDK 的交付对象是仓外策略作者（[ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)），故判据取「能力/契约角色/仓外标准能力」，仓内消费者数只作佐证。
- `dz_md_source_name`：随单行情源改造引入（commit e1d6fbf）。原始用途是让策略模板按 `instance_id` 过滤 `PRELOAD_MD_SHM` 广播帧；预加载内部化后（commit afbd905）该画面消失，模板侧不再需要源名。仓内唯一消费者是 `stg_demo` 的 ready/info 行；它是全部 `DZ_API` 中唯一无 doxygen 注释、且无任何测试的函数。源名已由 master 注入环境变量 `DZTRADER_MD_SOURCE` 并记日志（`strategy env injected | ... DZTRADER_MD_SOURCE=...`）。
- `dz_notify_self`：语义是 post 自身信号量唤醒 `dz_wait`；"self" 无信息量（单进程 SDK 唯一目标就是自己），且 "notify" 与通知中心 `dz_notify_ui`/`NOTIFY_UI` 撞词。改名成本仅 7 处引用。
- 同轮确认保留：`dz_query_account_status`（事件通道无历史重放，它是策略启动时取得账户状态的唯一路径）；UI 两条通道（`NOTIFY_UI` 与 `UI_INPUT`+`OUTPUT_UI`）的帧名与函数名本次保持不动。

## Decision

1. 删除公开函数 `dz_md_source_name`（声明与导出符号一并删除）；`DzContext::md_source_name` 内部字段保留——订阅写帧的 `ext_inst_id` 与 `NOTIFY_MD_STARTED` 过滤仍依赖它。源名继续由 master 注入 `DZTRADER_MD_SOURCE`，`stg_demo` 直接读环境变量展示。
2. `dz_notify_self` 改名 `dz_wakeup`：语义不变（唤醒自身信号量），与 `dz_wait` 成对，消除与通知语义的撞词。
3. `dz_wait`/`dz_next_event`/`dz_next_md` 保持原名；`api.h` 补一段标准主循环文档，不改名。
4. 本次明确保留：`dz_query_account_status` 不动；UI 通道命名全部保持，对称化/控制台化待 issue #6 接线时再评估。

## Alternatives

- 保留 `dz_md_source_name` 并补 doxygen/测试：公开面留一个无能力语义、信息与环境变量重复的函数；否决。
- 保留 `dz_notify_self`：撞词是真实歧义源，而改名成本低；否决。
- 重命名其余三个事件循环原语（如 `dz_read_event`/`dz_read_md`）：232+ 处引用，语义不变得更清楚；否决。
- UI 通道对称化（`OUTPUT_UI`→`UI_OUTPUT`）或控制台领域化（`CONSOLE_*`）：改动 9–46 处且与 issue #6 接线绑定；暂缓，不在本 ADR 决策内。
- 重命名 `dz_query_account_status`（如 `dz_request_account_status`）：现有名与帧 `TD_QUERY_ACCOUNT_STATUS` 对应、语义准确（异步查询）；本次不动。

## Consequences

- ABI：`dzstrategy_sdk` 少一个导出符号、一个符号改名；旧策略二进制需随新 SDK 重编译（0.0.1 无兼容层，同 [ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)/[ADR 0021](0021-strategy-sdk-no-logging-api.md)）。
- 策略侧获取源名的唯一途径变为 `DZTRADER_MD_SOURCE`（master 注入并记日志）；`stg_demo` 的 ready/info 展示改为读该环境变量，E2E 断言语义不变。
- 其余公开面候选（`dz_version_*` 收敛、`dz_resultset_get_bool` 等）不在本 ADR 范围，清单与证据留档于 GitHub issue「策略 SDK 公开面收敛（C 波）：候选清单与证据」。

## References

- [ADR 0020: 策略 SDK 交付形态与公开面](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)
- [ADR 0021: 策略 SDK 不提供日志能力（撤销公开 `dz_diag`）](0021-strategy-sdk-no-logging-api.md)
- commit `e1d6fbf`（引入 `dz_md_source_name`）、`afbd905`（预加载内部化）
