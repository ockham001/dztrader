# ADR 0039: 策略 SDK 配置文件的原子写自成一体（不共用平台 json_section）

## Status

Accepted（2026-10-28，issue #156 实现期决策）

## Context

- BarStrategy 配置层（#156）要求配置写回是「临时文件 + `fsync` + 原子替换」，写盘失败时内存与旧文件都不变；该层是 header-only、随策略 SDK 公开 include 目录分发的代码。
- 平台侧已有一份 JSON 原子写：`libs/core/include/dztrader/core/json_section.h` 的 `save_json_section`（load-modify-save、tmp + rename、损坏备份、**不做 `fsync`**、经外部 `<nlohmann/json.hpp>` 取 JSON）。它服务 `dztraderd` / `dzsysmon` 等进程的 section 配置，不出现在 SDK 公开面。
- 按 AGENTS §三.1「同一能力不允许两套实现并存超过一个提交周期」，两套原子写必须在 ADR 里取舍；不能只把它写进组件文档。

## Decision

保留两份实现，**不合并、不互引**：

1. **SDK 配置层自带最小原子写**（`dztrader/bar_strategy/config_file.h`）：整文件替换、文件 `fsync`（Windows `_commit`）为硬要求、POSIX 父目录 `fsync` 尽力而为、不建父目录、JSON 只经 `dztrader/json.h` 取用（默认内置单头）。
2. **`libs/core` 的 `json_section.h` 保持平台内部使用**，不进入 SDK include 目录，也不被 SDK 头包含。

**为什么不用另一个方案**：

- **让 SDK 直接包含 core 头**：SDK 的公开承诺面只有 `libs/strategy_api/include`（ADR 0020），把 core 头发布进 SDK 会同时把「外部 nlohmann 路径」带进策略作者环境，与 ADR 0026 的「默认内置单头、`DZTRADER_JSON_EXTERNAL` 才用外部版本」直接冲突（同一 TU 混用两种 nlohmann 来源即 ODR 违规）；且 core 头依赖平台内部库的编译环境，不再是 header-only 自包含。
- **把 core 的 helper 提升为公共库**：它的语义是「读整文件 → 改一个 section → 写回」，写回是 best-effort 持久化（无 `fsync`）、损坏时备份而不是拒绝。配置层要的是「整文件原子替换 + 落盘失败必须报错」，两者签名与错误语义不同，强行合并会得到一个谁都不完全满足的通用接口——正是 ADR 0027 决策 16 反对的投机抽象。
- **放弃 `fsync` 改用 `json_section` 的行为**：`fsync` 是本票 AC 的显式要求（rename 后才发现数据没落盘就晚了），不能用「已有 helper 没做」作为降低要求的理由。

## Consequences

- 正面：SDK 保持自包含、header-only、默认零第三方依赖；配置层的 `fsync`/失败语义可以按 AC 收紧而不影响平台进程；两份实现都很短（各约 40 行），边界清晰（一份面向 section 补丁，一份面向整文件替换）。
- 负面：原子写逻辑存在两份（tmp 命名、rename 失败清理各写一次）；未来若平台也要求「写盘失败必须报错 + `fsync`」，应把它提升为共享实现并在新 ADR 中 supersede 本决策，而不是继续分叉。
- 测试责任：两份实现各自测试（配置层见 `libs/strategy_api/tests/bar_strategy_config_file_test.cpp`；平台侧见 `libs/core` / `apps/sysmon` 既有测试）。文件内容格式（2 空格缩进、不加结尾换行）在两侧都是实现细节，不是跨进程契约。
