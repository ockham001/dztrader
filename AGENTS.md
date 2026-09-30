# dztrader

量化交易系统（CMake + Conan 2 + Ninja + GTest / C++20，Windows MSVC + Linux GCC 13）。
单人 + AI 协作。**项目简介、特性、4 命令工作流、命令详解、部署流程见 [README.md](README.md)** —— 本文只写 README 不会写、且写错代价很高的部分。

---

## 一、硬门禁

违反下面任何一条都属于返工，不是"风格建议"。

### 1. `apps/ctp/md/` 生产锁定

**该目录下所有文件的改动，必须先向我说明改动内容并取得确认，才能动手。**

这条规则的原始载体（一个锁定的说明文件）已在历史提交中删除，本条是全仓**唯一**存活的记录。不要以"顺手改一下"绕过。

### 2. `DZTRADER_HOME` 必须指向项目内

测试/调试期间**所有** `DZTRADER_HOME` 必须指向 `<项目根>/.dztrader_dev`：

- `ctest` 经 `CMakePresets.json` 的 `test-base` 强制注入，无需手动设置
- 手动运行任何 `dz*` 可执行文件（`dztraderd` / `dzweb` / `dzmd_*` / `dztd_*` / `stg_*`）前**必须显式设置** `DZTRADER_HOME=<项目根>/.dztrader_dev`
- **禁止依赖系统级环境变量**。漏设会在真实 home 目录里创建 `shm/`、`db/`、`configs/`、`flow/`、`cache/`（`.gitignore` 专门锚了这五条路径，正是因为它们一定会出现），且难以清理

唯一豁免：在 `SetUp` 内自管临时目录的测试自行覆盖该变量，不受此限。

### 3. 编译期门禁

- GCC 侧常开 `-Wall -Wextra -Wpedantic -Wconversion`（`CMakeLists.txt`）
- **`-Werror` 仅在 Release 生效** —— Debug 下告警可见但不致命，Release 自动升为硬门禁
- `-Wconversion` 是有意开启的收窄转换检查。`std::chrono duration → int32_t` 等已知噪音面按各文件既有惯例用 `static_cast` 显式收窄，**不要**为了消告警关掉这个选项
- 单文件编译检查必须用 `/Fo` 输出到临时目录，或直接用 `clangd --check`。**禁止在项目根裸调 `cl`**（会产生 `.obj` / `vc140.pdb` 污染根目录）

### 4. 只能通过 4 个封装脚本驱动构建

日常迭代只用 `setup` / `build` / `run-dev` / `clean` 四个脚本，**命令与参数以 [README.md 的"快速开始"与"命令详解"](README.md) 为准**。

绕开脚本直接 `cmake --build --preset ...` 会**静默丢失 config 粘性**：`.dztrader_dev/.config` 由 `setup` 写入，`build` / `run-dev` 自动跟随（优先级：`--config` 参数 > 粘性文件 > `release`）。直接调 cmake 等于每次都退回 `release`。

两条容易漏的附带事实：

- **`clean` 会删掉 `.dztrader_dev/`（含粘性 config 文件）**，下次需重新 `setup` 建立
- `run-dev.sh` **没有** `--stop` 子命令（`run-dev.ps1` 才有 `-Stop`）；Linux 手动停止用 README 给的 `pkill` 形式

**Debug 产物永不部署。** `run-dev` 在 Debug 下会打印 `=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===`；部署永远从 `build/<plat>/<arch>/Release/` 取。

### 5. 禁止直接 `npm run build`

改前端后必须走 `build` 脚本（底层 cmake target `dzweb_frontend`，自动 `npm ci` → `vite build` → 复制到 `${DZ_OUTPUT_DIR}/web`）。

直接 `npm run build` 的产物只落在 `frontend/dist/`，而 `dzweb` 加载的是 `build/.../web/` —— **UI 不会更新**，且没有报错。

用户验证 UI **永远访问 8080**（`dzweb.exe` 的 drogon HTTP 服务器，托管静态文件 + `/api` + `/ws` + `/health`）。5173 只是 vite dev server，仅在需要前端 HMR 时作为例外。

### 6. 配置文件不提交模板

配置文件一律由进程在缺失时**自动生成默认配置**（参照 `dztraderd.json` / `webui.json` 先例）。**仓库不保留 `*.example` 模板文件。**

### 7. 帧契约与 UI 协议

`docs/frame_contracts/` 是帧协议与 frontend↔dzweb 协议的**唯一语义真相源**。**先读 [general.md](docs/frame_contracts/general.md)（总则）**，再按需读各主题契约（目录地图见 [frame_contracts/README.md](docs/frame_contracts/README.md)，文件无编号）。

跨契约引用格式为《帧契约：\<主题\>》§N。**禁止以行号引用契约。**

---

## 二、架构不变量

这 7 条是**代码的前置条件**，无法从代码或 git 历史推导。改动前必须理解，违反会产出"能编译、测试也过、运行时静默出错"的东西。

1. **一个策略一个进程**，提供 c / c++ / python 接口
2. **一个行情源一个进程**
3. **一个交易接口一个进程**，同接口多账户同进程
4. **主进程**负责启停子进程、管理共享内存
5. **前后端分离**，后台可无 UI 运行，前端支持 Qt 和 WebUI
6. **共享内存 = 内存文件映射，不是环形缓冲区。** 文件持续增长，定期删除旧文件，**单文件序号只增不减**。默认心智模型（共享内存 IPC → 环形队列）在这里是错的，不要"顺手优化"
7. **核心路径极致性能（30μs 目标），7×24 运行**

> 第 6 条曾因内存预热被误改（见 `docs/adr/0006-shm-no-memory-warming.md`）。

---

## 三、方案收敛（硬规则）

1. **同一能力不允许两套实现并存超过一个提交周期。** 举棋不定时：写 ADR 选一个，另一个**立即删除** —— git 历史就是后悔药，不留尸体
2. **接口/协议只有一个真相源**（帧契约目录、类型头文件）。新实现**先改契约再改代码**（`general.md` §11.3 变更 checklist）
3. **删除旧方案前：全仓 grep 确认无引用。** 删除本身值得记录时，作为对应 ADR 的 Consequences
4. **发现文档与代码漂移：以代码/契约为准，当场修文档，随同一次提交**，不留"以后再说"

**仓库是唯一记忆。** 计划与决策一律落 `docs/`，不做工具私有 plans。工具目录只有 `.opencode/`；其他工具的历史产物归档到 `docs/plans/`（前缀 `legacy-tool-`）。

### ADR（架构决策记录）

**何时必须写**（任一条件满足）：

- 方向性改变：如"订阅请求由 master 转发改为策略直发行情进程"这类协议/拓扑变化
- 两套方案取舍：犹豫不决、或已并行实现过的能力
- 不可逆/高成本变更：删除模块、契约编号变更、帧布局变化、构建系统迁移

**写法**：`docs/adr/NNNN-<slug>.md`，编号**只增不回收**。模板 = Status / Context / Decision / Consequences，外加需要时的 Supersedes 段（参考 0002、0003 的实际结构）。Decision 必须写明**为什么不用另一个方案**；20 行内讲清，超过说明决策不聚焦。

**约束**：一经 **Accepted 不修改** —— 新的推翻写新 ADR，旧 ADR 加 Supersedes 引用。契约重编号的 old→new 映射见 ADR 0003。历史文档（`docs/specs/`、`docs/plans/`）不改。

---

## 四、代码约定

摘要。命名以**现有代码为准**（代码库自身一致遵循）。日志格式的检查工具是 `scripts/check_log_format.py`，**需手动在提交前跑**（未接入 CI，CI 不会替你兜住）。构建与跨平台的外围细节见 [docs/agent/build-appendix.md](docs/agent/build-appendix.md)。

### 命名

- 文件名 snake_case；头 `.h`，源 `.cpp`
- 头文件保护 `DZTRADER_<路径>_<文件名>_H_`
- C 接口（`dz` 前缀，`DZ_API` 导出）：函数 `dz_`+snake_case；typedef/struct `Dz`+PascalCase；宏/常量 `DZ_`+UPPER_SNAKE_CASE
- C++（`dztrader` 命名空间内）：类/结构体/枚举类型名/模板参数 PascalCase；函数与变量 snake_case；**class 成员 snake_case + `_`**；常量 UPPER_SNAKE_CASE；unscoped 枚举值 = 类型前缀+PascalCase，`enum class` 枚举值 PascalCase
- Internal 命名空间：翻译单元内部用匿名命名空间；需跨 TU 共享时用 `<文件名>_internal`（≤3 级）—— **不要自造 `helpers` / `detail` 之类**
- **头文件引用**：项目内部、以及策略接口的**源文件** → `#include <dztrader/common.h>`；策略接口的**头文件** → `#include "common.h"`

### 日志与异常

- 注释风格：**doxygen**
- **所有**错误描述 / 异常消息 / 日志文本：**小写开头、无句号、全英文**
- 格式：`prose | k=v k=v`。key 全小写下划线分隔；值含空格/等号/引号用双引号（`error="disk full"`）；ID/数字/枚举不加引号（`order_id=12345`）
- **禁止单引号包裹占位符**：错 `"user '{}' fail"`，对 `"user fail | user={}"`
- **禁止多行 `===` banner**，必须合并为单行 `prose | k=v`。**禁止中文**（含函数名、变量名）
- 非性能路径用异常，日志作为最终输出。`Exception ──catch──→ LastError::set()`（C 接口边界，**无日志**）/ `──→ spdlog`（平台代码，无 LastError）
- 提交前跑 `python scripts/check_log_format.py apps/ libs/`；违规用 `// NOLINT(log-format)` 豁免

### 跨平台

仅 64 位，Windows x86_64 / MSVC 与 Linux x86_64 / GCC。策略接口（C API）不在此限制内。

非上述组合**必须 `#error`，禁止静默回退**（`((void)0)` 之类）。完整 `#if` 模板见 `docs/agent/build-appendix.md`。

---

## 五、工作方式

### 改动纪律（按此优先级）

1. **外科手术式改动。** 只碰必须碰的；不要"顺手改进"相邻代码、注释或格式；不要重构没坏的东西；匹配现有风格。**发现无关死代码只提示，不删除**；只清理**你自己**的改动造成的孤儿（多余的 import / 变量 / 函数）。判据：每一行改动都能直接追溯到用户这次的请求
2. **先说假设再动手。** 不假设、不掩饰困惑。多种解释时**全部列出**而不是默默选一个；有更简单的方案就直说；不清楚就停下来问
3. **发现文档与代码漂移：当场修文档**（见第三节收敛规则 4）

### 日常工作流（单人 + AI 协作）

1. **开工前必读**：本文 → [docs/README.md](docs/README.md)（文档地图与冲突裁决规则）→ 相关帧契约（先读 `general.md`）
2. **小步提交**：一个提交只做一件事，每步可验证（构建 + 相关测试）。提交信息**中文**，前缀 `fix:` / `feat:` / `refactor:` / `docs:` / `test:` / `chore:`
3. **push 前自检**：见 [docs/development-checklist.md](docs/development-checklist.md) 的 C 段（**那是权威且更全的清单** —— 含 `type-check` / `lint` / `lint:css` / 契约 Freshness 门禁）。push 后等 CI 绿再开下一波；**CI 红先修红，不叠加新改动**
4. **大改动先写文档**：spec 设计 + plan 实施计划（checkbox 制），我确认后再实现
5. **收尾**：工作区保持干净（运行产物已 gitignore；临时诊断脚本要么通用化提交、要么删除）。每波改动结束 `git status` 干净 + `git log` 可读 + CI 绿

### 组合变更的特别提醒

- **跨进程协议改动**：先改 `docs/frame_contracts/` 契约，再按 `general.md` §11.3 checklist 逐项同步（platform 头文件、帧号登记、dzweb 领域服务、前端、测试）。契约定稿后**实现以契约为准**
- **涉及 `apps/ctp/md`**：见第一节硬门禁 1，改前必须先问我
- **帧/契约编号一经发布只增不改**（ADR 0003）

### Worktree 与 Git 禁令

- worktree 放 `.worktrees/`，`git worktree add .worktrees/<branch> -b <branch> main`
- worktree 内**需重新跑 `setup`**（conan 缓存独立）
- 多终端可在不同 worktree 并行；**禁止同一 worktree 同时操作**
- 合并前确认共同祖先：`git merge-base main HEAD`，无则 cherry-pick
- **禁止**：`--orphan`、`push --force` 到 main、未确认的 `reset --hard` / `clean -fd`、未经我同意推送远程

---

## 六、CodeGraph

本仓已建索引（仓库根存在 `.codegraph/`）。**理解或定位代码时先用 CodeGraph，而不是 grep / find / 逐个读文件**：

- MCP 工具 `codegraph_explore`：一次调用返回相关符号的逐字源码 + 调用路径（含 grep 跟不动的动态派发链）。传入文件名即可当作读该文件
- Shell 兜底（永远可用）：`codegraph explore "<符号名或问题>"`
- 仓库根**没有** `.codegraph/` 时完全跳过 CodeGraph。**是否建索引由我决定，不要自行 `codegraph init`**

---

## 七、按需查阅

| 文档 | 内容 |
|---|---|
| [README.md](README.md) | 项目简介、特性、**4 命令工作流与命令详解**、调试流程、部署 |
| [docs/README.md](docs/README.md) | 文档地图：活跃真相源 / 架构参考分层与冲突裁决规则 |
| [docs/frame_contracts/](docs/frame_contracts/README.md) | 帧协议唯一语义真相源（先读 `general.md` 总则） |
| [docs/development-checklist.md](docs/development-checklist.md) | 新功能门禁清单（前端 / 后台 / 提交前三段） |
| [docs/architecture.md](docs/architecture.md) | 依赖方向、静态库策略、进程模型（蓝图级，细节以代码与契约为准） |
| [docs/components/](docs/components/) | 组件级设计（线程模型、通道、会话） |
| [docs/ui-trading.md](docs/ui-trading.md) | UI 系统与交易接口规划（蓝图级） |
| [docs/adr/](docs/adr/) | 架构决策记录，按编号追溯 |
| [docs/agent/build-appendix.md](docs/agent/build-appendix.md) | 构建外围细节：工具链、MSVC 激活、clangd、预设、输出布局、单测约定、install、conan、前端机制、部署产物分析、跨平台 `#error` 模板 |

`docs/specs/`、`docs/plans/`、`docs/superpowers/` 是**本地归档**（不在版本控制内），与现状冲突时以帧契约与代码为准，**不修改**。
