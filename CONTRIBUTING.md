# 贡献指南

dztrader 是一个**单人维护**的开源量化交易系统（346 个提交均来自一位作者）。Issue 与 PR 都欢迎，但请把下面几条当作硬要求 —— 它们不是风格偏好，违反了会让改动被要求返工。

---

## 1. 先读约定

改动前请读仓库根的 **[AGENTS.md](AGENTS.md)**。它是编码 agent 与新贡献者的**权威项目约定**，包含：

| 你要做的事 | 看哪一节 |
|---|---|
| 碰 `apps/ctp/md/`、跑进程、改前端 | 第一节「硬门禁」 |
| 动进程模型、共享内存、性能路径 | 第二节「架构不变量」 |
| 引入第二套实现、删代码、改文档 | 第三节「方案收敛」+ ADR |
| 写 C++ 代码 | 第四节「代码约定」 |
| 准备提交 | 第五节「工作方式」 |

本文只写"从哪儿开始"，不重复内容。

## 2. 环境要求

见 [README.md 的依赖表](README.md)。两点特别容易踩：

- **Linux 必须有 `/usr/bin/gcc-13`。** `profiles/linux-gcc` 用 `tools.build:compiler_executables` 把编译器路径钉死，以保证 Conan 源码包的 `package_id` 与 CI 的 gcc-13 预编译包一致。系统默认 gcc 是什么版本不重要，但那个路径必须在
- **Windows 需要 MSVC 2022**（含 C++ 桌面开发工作负载）。Ninja 不会自动激活 MSVC 环境，`scripts/` 下的封装脚本会处理

首次克隆 / 改依赖 / 新建 worktree 后先跑 `setup`（见下）。

## 3. 构建与测试：只用 4 个命令

日常迭代只用 `setup` / `build` / `run-dev` / `clean` 四个封装脚本，**命令、参数、config 粘性机制见 [README.md 的"快速开始"与"命令详解"](README.md)**。

为什么必须走脚本：它们维护 `.dztrader_dev/.config` 粘性配置，绕过脚本直接调 `cmake --build --preset` 会**静默退回 Release**。

需要知道的几个事实：

- `clean` 会删掉 `.dztrader_dev/`（含粘性配置），之后需重新 `setup`
- CI 用的也是这几个脚本（Windows job 跑 `setup.ps1` / `build.ps1` / `test.ps1`）
- 改前端后走 `build` 脚本，**不要**直接 `npm run build` —— 产物落在 `frontend/dist/`，而 `dzweb` 加载的是 `build/.../web/`，UI 不会更新且不报错

## 4. 提交前的门禁

**权威清单是 [docs/development-checklist.md](docs/development-checklist.md) 的 C 段**（push 前必跑）。CI 在 Windows（MSVC Release）与 Linux（GCC Release）两个 job 上强制其中这些：

| 门禁 | 覆盖范围 |
|---|---|
| `ctest` 全绿 | 后端 C++，含集成冒烟 |
| `npm run type-check` | 前端 Vue/TS 类型 |
| `npm run lint` + `npm run lint:css` | 前端 eslint + CSS token 规则 |
| 前端契约类型 Freshness | `generated.ts` 与 schema 不得漂移 |
| `npm test` | 前端 vitest |

另外两条 **CI 不覆盖、需你本地跑**：

- `python scripts/check_log_format.py apps/ libs/` —— 日志格式检查（`// NOLINT(log-format)` 可豁免）
- `clang-format -i <文件>` —— 项目提供 `.clang-format`，但 CI 不校验格式

`.clang-tidy` 配置也在仓库里，同样**未接入构建与 CI**，按需手动跑。

## 5. 几条最容易踩的硬门禁

摘自 [AGENTS.md](AGENTS.md)，重复一遍因为它们最容易在无意识中破坏：

- **`apps/ctp/md/` 是生产锁定区。** 改动前必须先在 Issue 或 PR 里说明改动内容并取得维护者确认，不要直接提 PR
- **`DZTRADER_HOME` 在测试/调试期间必须指向项目内 `.dztrader_dev`。** 漏设会在真实 home 目录创建 `shm/`、`db/`、`configs/`、`flow/`、`cache/`，且难以清理
- **共享内存是内存文件映射，不是环形缓冲区。** 文件持续增长、单文件序号只增不减 —— 不要"顺手优化"成环形队列
- **帧协议与 frontend↔dzweb 协议的唯一真相源是 `docs/frame_contracts/`。** 改协议**先改契约**（先读 `general.md` 总则），再按其 §11.3 逐项同步 platform 头文件、帧号登记、dzweb、前端、测试。契约重编号只增不改（见 ADR 0003）
- **一个策略 / 一个行情源 / 一个交易接口 = 一个进程。** 进程模型是架构不变量，不要合并网关或跨策略共享 ioc

## 6. 代码风格

- **C/C++ 命名以现有代码为准**（代码库自身一致遵循）；规则摘要见 [AGENTS.md 第四节](AGENTS.md)。不要引入与现有风格并存的新命名体系
- **所有共享内存结构体必须用 `DZ_DECLARE_ALIGNED_STRUCT` 声明**（保证 8 字节对齐且无 padding）
- **注释用 doxygen**
- **日志/错误文本：小写开头、无句号、全英文**，格式 `prose | k=v k=v`，禁止多行 `===` banner —— 规则与豁免见 [AGENTS.md 第四节](AGENTS.md)
- **跨平台**：仅 64 位（Windows x86_64 / MSVC 与 Linux x86_64 / GCC）。不支持的组合必须 `#error`，**禁止静默回退**

## 7. 测试放哪

- 单元测试用 Google Test，与被测模块同目录：`libs/<名称>/tests/` 或 `apps/<组件>/tests/`
- 每个 `*_test.cpp` 独立编译，由 `gtest_discover_tests` 自动注册
- 集成冒烟测试在 `apps/master/tests/`
- 新功能应带测试。**本项目不设覆盖率指标，也没有接入覆盖率工具** —— 靠测试的针对性而非数字

## 8. 提交信息

遵循 [约定式提交](https://www.conventionalcommits.org/)，描述用**中文**：

```
<类型>: <中文描述>

<可选正文：为什么这样改，而不只是改了什么>
```

类型：`feat` / `fix` / `refactor` / `docs` / `test` / `chore` / `perf` / `ci`

示例：
- `feat(td): 新增持仓聚合查询接口`
- `fix(db): 修 WAL 收敛诊断在只读会话下的无限重试`
- `refactor: 删除 legacy SQL 级接口，P0 收敛完成`

**一个提交只做一件事**，每步可验证。正文请写清"为什么" —— 这个仓库的提交信息本身就是决策记录。

## 9. 架构决策

重大架构变更要写 ADR，落在 `docs/adr/`，格式为 `NNNN-<slug>.md`。

**何时必须写**、编号规则、以及"Accepted 后不可修改"的约束，见 [AGENTS.md 第三节](AGENTS.md)。简要：方向性改变、两套方案取舍、不可逆或高成本变更（删模块、契约重编号、帧布局变更、构建系统迁移）任一满足即需写；编号只增不回收；Decision 必须说明**为什么不用另一个方案**。

## 10. 如何贡献

- **报问题**：提 Issue。请给出复现步骤、`DZTRADER_HOME` 下生成的日志，以及相关帧契约编号
- **提 PR**：从 `main` 切分支 → 按第 3 节构建 → 按第 4 节跑完门禁 → PR 描述里写清改了什么、为什么
- **预期**：这是单人维护的项目，评审可能需要几天。涉及 `apps/ctp/md/`、帧契约或构建系统的改动，建议**先开 Issue 讨论**再动手
- 不确定某个约定是否仍然成立时，问一声 —— 文档可能滞后于代码，以代码和契约为准
