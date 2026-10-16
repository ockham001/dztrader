# 贡献指南

dztrader 是一个由一位作者维护的开源量化交易系统。Issue 与 PR 都欢迎，但下面几条是硬要求 —— 违反了会让改动被要求返工。

## 0. 先读约定

改动前请读仓库根的 **[AGENTS.md](AGENTS.md)**。它是项目约定（含编码 agent 约定）的**权威来源**：

| 你要做的事 | 看哪一节 |
|---|---|
| 碰 `apps/ctp/md/`、跑进程、改前端 | 第一节「硬门禁」 |
| 动进程模型、共享内存、性能路径 | 第二节「架构不变量」 |
| 引入第二套实现、删代码、改文档 | 第三节「方案收敛」+ ADR |
| 写 C++ 代码 | 第四节「代码约定」 |
| 准备提交 | 第五节「工作方式」 |

本文只写"从哪儿开始"，不重复内容。文档可能滞后于代码，**以代码与契约为准**。

## 1. 环境与构建

- 依赖与版本要求见 [README.md](README.md) 前置表。Linux 侧必须存在 `/usr/bin/gcc-13`（原因见 `profiles/linux-gcc` 注释）；Windows 用 MSVC 2022，`scripts/` 下的封装脚本会处理环境激活
- 日常迭代只用 `setup` / `build` / `run-dev` / `clean` 四个封装脚本；命令、参数与 config 粘性机制见 [README.md 的"快速开始"与"命令详解"](README.md)
- 改前端必须走 `build` 脚本 —— 直接 `npm run build` 的产物不会被 `dzweb` 加载（原因见 AGENTS.md 硬门禁第 5 条）

## 2. 提交前的门禁

**权威清单是 [docs/development-checklist.md](docs/development-checklist.md) 的 C 段**（push 前必跑）。CI 的覆盖范围以 [.github/workflows/ci.yml](.github/workflows/ci.yml) 为准；CI 之外的检查以清单为准。

## 3. 代码约定（要点）

命名、日志、跨平台规则的摘要与权威均在 [AGENTS.md 第四节](AGENTS.md)。补充一条最容易忘的：

- **所有共享内存结构体必须用 `DZ_DECLARE_ALIGNED_STRUCT` 声明**（保证 8 字节对齐且无 padding）

## 4. 测试放哪

- 单元测试用 Google Test，与被测模块同目录：`libs/<名称>/tests/` 或 `apps/<组件>/tests/`；每个 `*_test.cpp` 独立编译，由 `gtest_discover_tests` 自动注册
- 集成冒烟测试在 `apps/master/tests/`
- 新功能应带测试。**本项目不设覆盖率指标，也没有接入覆盖率工具** —— 靠测试的针对性而非数字

## 5. 提交信息

遵循[约定式提交](https://www.conventionalcommits.org/)，描述用**中文**；类型清单与示例见 [AGENTS.md 第五节](AGENTS.md)。**一个提交只做一件事**，正文写清"为什么" —— 提交信息本身就是决策记录。

## 6. 架构决策与协议

- 重大架构变更写 ADR，落 `docs/adr/`（`NNNN-<slug>.md`）；触发条件、编号规则、"Accepted 后不可修改"见 [AGENTS.md 第三节](AGENTS.md)
- 改帧协议**先改 [docs/frame_contracts/](docs/frame_contracts/README.md)**（先读 general 总则），再按其 §11.3 checklist 逐项同步；契约编号规则见 ADR 0003

## 7. 如何贡献

- **报问题**：提 Issue。请给出复现步骤、`DZTRADER_HOME` 下生成的日志，以及相关帧契约主题（如《帧契约：md-subscription》）
- **提 PR**：从 `main` 切分支 → 按第 1 节构建 → 按第 2 节跑完门禁 → PR 描述里写清改了什么、为什么
- **预期**：单人维护，评审可能需要几天。涉及 `apps/ctp/md/`、帧契约或构建系统的改动，建议**先开 Issue 讨论**再动手
- 不确定某个约定是否仍然成立时，问一声 —— 以代码和契约为准
