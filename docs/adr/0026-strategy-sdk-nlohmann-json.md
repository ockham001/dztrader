# ADR 0026: 策略 SDK 内置 nlohmann/json（修订 ADR 0020 公开面不变量）

## Status

Proposed（2026-10-20，待评审；评审通过后改 Accepted）

Supersedes [ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md) 的两条表述：

- Decision 2「公开面三不变量」中的「C++ 层仅依赖标准库」→ 改为「C++ 层唯一允许的编译期第三方依赖 = nlohmann/json」；
- Decision 6「公共头不得引入第三方解析库（含 nlohmann）」→ 失效。

## Context

- 新的 BarStrategy 框架（[ADR 0027](0027-bar-strategy-framework.md)）需要 JSON 配置的读取与原子写回：品种列表、账户列表、周期、`max_lookback`、全局/品种参数。配置由框架管理器（A）读写，策略作者不直接解析。
- [ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md) Decision 2 规定策略 SDK 公开头文件「C 层为纯 C（C11）可编译；C++ 层仅依赖标准库（C++20）」；Decision 6 进一步规定 UI_INPUT 命令协议「不得把 nlohmann 引入公共头」。
- 用户决策（2026-10-20）：SDK 自带 nlohmann/json，并允许策略作者通过宏接入自己版本/来源的 nlohmann（参照 spdlog 内嵌 fmt 的模式）。理由：自研 JSON 解析/序列化成本高、易错；nlohmann/json 事实上的准标准库地位。
- 仓库现状：Conan 已固定 `nlohmann_json/3.12.0`（`conanfile.py`），各库以 CMake target 方式链接；公开头目前只 include `<dztrader/*>` 与标准库。
- 不因本决定改变的三条：C 层纯 C（C11）可编译；动态库导出符号仅 `dz_*`；运行时依赖闭包零第三方。

## Decision

1. **SDK 内置 nlohmann/json 单头**：以单头形式随 SDK 头文件目录分发，版本与仓库 Conan 固定版本一致（当前 3.12.0）。最终路径在实施时确定，验收标准是「SDK include 目录 + 动态库即可编译外部策略」。
2. **外部接入宏**：定义 `DZTRADER_JSON_EXTERNAL` 时，SDK 头以 `#include <nlohmann/json.hpp>` 使用外部提供版本；未定义时使用内置单头。宏名、include 方式与版本兼容要求写入策略开发者指南。
3. **修订 ADR 0020 公开面不变量**：C 层零第三方保持；C++ 层允许唯一第三方 `nlohmann/json`（header-only、编译期依赖）；动态库导出符号仅 `dz_*` 与运行时依赖闭包零第三方均保持。
4. **UI_INPUT 命令协议**：协议语义不依赖解析库；SDK 可提供基于 nlohmann 的可选辅助类型，协议契约仍以 `docs/frame_contracts/strategy.md` 为唯一真相源。
5. **分发与许可**：SDK 安装包包含内置单头；nlohmann 的 MIT 许可声明随 #7 的分发包一并提供。
6. **版本纪律**：内置单头与 Conan 版本在**同一次提交**升级；CI 增加「仅凭 SDK include 路径可编译」检查，覆盖内置与 `DZTRADER_JSON_EXTERNAL` 两种宏模式。

## Alternatives

- **自研 JSON 解析/序列化**：维护成本高，转义、嵌套、数值边界易错，且与仓库现有 nlohmann 用法重复；否决。
- **配置改用简单行格式**：无法自然表达数组与嵌套（品种、账户、参数覆盖），后续回测/优化扩展成本更高；用户明确选择 JSON；否决。
- **仅 Conan 依赖、不内置**：外部策略作者不接触 Conan，违背 ADR 0020 的自包含交付目标；否决。
- **保持零第三方，把 JSON 解析放可选头或由策略自备**：公开面出现两套配置解析能力，违背「同一能力不允许两套实现」；否决。
- **内置完整多文件库**：单头已足够且分发简单；否决。

## Consequences

- 公开头编译时间随内置单头增加（仅在包含处）；C 层不受影响。
- ADR 0020 的两条表述以本 ADR 为准；ADR 0020 本身不修改。
- 需要同步更新：`docs/development-checklist.md` 策略 SDK 段（第三方头检查 + 双宏模式编译）、#7 分发规则（headers/许可/版本/ABI 说明）、策略开发者指南（配置格式与外接 nlohmann 指南）。
- 外部策略作者定义 `DZTRADER_JSON_EXTERNAL` 时，需保证所用 nlohmann 版本满足 SDK 使用的 API 子集；版本兼容说明进开发者指南。
- 运行时依赖闭包与导出符号不变量不因本决定改变。

## References

- 讨论日期：2026-10-20（BarStrategy 设计会话）
- [ADR 0020: 策略 SDK 交付形态与公开面](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)
- [ADR 0027: BarStrategy 框架](0027-bar-strategy-framework.md)
- 源码与配置：`conanfile.py`（`nlohmann_json/3.12.0`）、`libs/strategy_api/CMakeLists.txt`、`docs/architecture.md`（策略接口约束）
