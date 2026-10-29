# dztrader

量化交易系统（CMake + Conan 2 + Ninja + GTest / C++20，Windows MSVC + Linux GCC 13）。
单人 + AI 协作。**项目简介、特性、4 命令工作流、命令详解、部署流程见 [README.md](README.md)** —— 本文只写 README 不会写、且写错代价很高的部分。

**定位/理解代码先 `codegraph_explore`（MCP）或 `codegraph explore "<符号>"`；不要先 grep/sed 逐个读文件。**

---

## 一、硬门禁

违反下面任何一条都属于返工，不是"风格建议"。

### 1. `apps/ctp/md/` 生产锁定

**该目录下所有文件的改动，必须先向我说明改动内容并取得确认，才能动手。**

这条规则的原始载体（一个锁定的说明文件）已在历史提交中删除；本条是全仓**权威**记录（`CONTRIBUTING.md` 的同类表述指向本节）。不要以"顺手改一下"绕过。已由 `.opencode/opencode.json` 权限机器化：编辑 `apps/ctp/md/` 会先要求确认。

### 2. `DZTRADER_HOME` 必须指向项目内

测试/调试期间**所有** `DZTRADER_HOME` 必须指向项目内目录，禁止落到真实 home：

- **测试**：`<项目根>/.cache/test_home`，由 `CMakePresets.json` 的 `test-base` 强制注入，无需手动设置；`test.sh` / `test.ps1` 每次运行前清空重建（避免上一轮残留 shm/DB 让无关测试失败）
- **手动运行任何 `dz*` 可执行文件**（`dztraderd` / `dzweb` / `dzmd_*` / `dztd_*` / `stg_*`）前**必须显式设置** `DZTRADER_HOME=<项目根>/.dztrader_dev`
- **禁止依赖系统级环境变量**。漏设会在真实 home 目录里创建 `shm/`、`db/`、`configs/`、`flow/`、`cache/`（`.gitignore` 专门锚了这五条路径，正是因为它们一定会出现），且难以清理

唯一豁免：在 `SetUp` 内自管临时目录的测试自行覆盖该变量，不受此限。

### 3. 编译期门禁

- GCC 侧常开 `-Wall -Wextra -Wpedantic -Wconversion`（`CMakeLists.txt`）
- **`-Werror` 仅在 Release 生效** —— Debug 下告警可见但不致命，Release 自动升为硬门禁
- `-Wconversion` 是有意开启的收窄转换检查。`std::chrono duration → int32_t` 等已知噪音面按各文件既有惯例用 `static_cast` 显式收窄，**不要**为了消告警关掉这个选项
- 单文件编译检查必须用 `/Fo` 输出到临时目录，或直接用 `clangd --check`。**禁止在项目根裸调 `cl`**（会产生 `.obj` / `vc140.pdb` 污染根目录）
- **MSVC-only 失败 Linux 编译器拦不到**（Windows 头顺序、`SPDLOG_DEBUG` 编译掉致 C4101 等）：`scripts/check_msvc_hazards.py` 已接入 preflight；改 Windows 分支前先读 [build-appendix](docs/agents/build-appendix.md) 的「Windows 编译陷阱」

### 4. 只能通过封装脚本驱动构建

日常迭代只用 `setup` / `build` / `run-dev` / `clean` 四个脚本（另有 `install-artifacts.sh` 供发版用），**命令与参数以 [README.md 的"快速开始"与"命令详解"](README.md) 为准**。

**只有一个日常构建档：Release。** 无参数即 Release；`Debug` / `asan` 是显式指定的本地档，不进门禁、禁止部署（见 [ADR 0037](docs/adr/0037-release-only-build-and-install-as-sole-artifact.md)）。**不存在「config 粘性」概念**——不要去找 `.dztrader_dev/.config`，它已被删除。

绕开脚本直接 `cmake --build --preset ...` 仍被权限 deny：脚本负责构建锁、ccache 环境与 install，直调会绕过锁（两个 ninja 写同一棵树产出不可信结果）。

三条容易漏的附带事实：

- **`clean` 会删掉 `build/` 与 `install/` 以及 `.dztrader_dev/`**，下次需重新 `setup`
- `run-dev.sh` 与 `run-dev.ps1` **都有** stop 能力（`--stop` / `-Stop`）
- 同一 worktree 的 build/test 共用一把锁；看到 `waiting for build lock` 是正常的串行等待

**`build/` 是中间产物，禁止直接运行。** 运行、部署、打包一律从 `install/<plat>/<arch>/Release/` 取（唯一产物源）。Debug 产物永不部署，`run-dev` 在 Debug 下会打印 `=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===`。

### 5. 禁止直接 `npm run build`

改前端后必须走 `build` 脚本（底层 cmake target `dzweb_frontend`：`package-lock.json` 变化时才安装依赖（门控脚本 `scripts/npm-ci-if-needed.cmake`），随后 `vite build` → 复制到 `${DZ_OUTPUT_DIR}/web`）。

直接 `npm run build`（已由权限 deny）的产物只落在 `frontend/dist/`，而 `dzweb` 加载的是 `build/.../web/` —— **UI 不会更新**，且没有报错。

用户验证 UI **永远访问 8080**（`dzweb.exe` 的 drogon HTTP 服务器，托管静态文件 + `/api` + `/ws` + `/health`）。5173 只是 vite dev server，仅在需要前端 HMR 时作为例外。

### 6. 配置文件不提交模板

配置文件一律由进程在缺失时**自动生成默认配置**（参照 `dztraderd.json` / `webui.json` 先例）。**仓库不保留 `*.example` 模板文件。**

### 7. 帧契约与 UI 协议

`docs/frame_contracts/` 是帧协议与 frontend↔dzweb 协议的**唯一语义真相源**。**先读 [general.md](docs/frame_contracts/general.md)（总则）**，再按需读各主题契约（目录地图见 [frame_contracts/README.md](docs/frame_contracts/README.md)，文件无编号）。

跨契约引用格式为《帧契约：\<主题\>》§N。**禁止以行号引用契约。**

> **权限机器化的生效范围**：`.opencode/opencode.json` 里的 permissions / deny（`apps/ctp/md/` 改前确认、直调 cmake / `npm run build` deny 等）只在 **opencode** 会话生效；DSH 等其它 agent 宿主没有这层机器约束，依赖宿主 approval policy + 本文件自律。**规则强度不变**——"没被权限系统拦住"不等于可以破例。

---

## 二、架构不变量

这 7 条是**代码的前置条件**，无法从代码或 git 历史推导。改动前必须理解，违反会产出"能编译、测试也过、运行时静默出错"的东西。

1. **一个策略一个进程**，提供 c / c++ 接口（Python 绑定规划中，尚未实现）
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

**仓库（含其 GitHub Issues）是唯一记忆。** 决策落 `docs/`（ADR / 契约 / 文档），设计与计划落 GitHub Issues（见「标准开发循环」）；不做工具私有 plans。工具目录只有 `.opencode/`；其他工具的历史产物归档到 `docs/plans/`（前缀 `legacy-tool-`）。

### ADR（架构决策记录）

**何时必须写**（任一条件满足）：

- 方向性改变：如"订阅请求由 master 转发改为策略直发行情进程"这类协议/拓扑变化
- 两套方案取舍：犹豫不决、或已并行实现过的能力
- 不可逆/高成本变更：删除模块、契约编号变更、帧布局变化、构建系统迁移

**写法**：`docs/adr/NNNN-<slug>.md`，编号**只增不回收**。模板 = Status / Context / Decision / Consequences，外加需要时的 Supersedes 段（参考 0002、0003 的实际结构）。Decision 必须写明**为什么不用另一个方案**；20 行内讲清，超过说明决策不聚焦。

**约束**：一经 **Accepted 不修改** —— 新的推翻写新 ADR，旧 ADR 加 Supersedes 引用。契约重编号的 old→new 映射见 ADR 0003。历史文档（`docs/specs/`、`docs/plans/`）不改。**唯一例外**：修正失效链接指向与错别字（随提交说明，不改决策内容）。

---

## 四、代码约定

摘要。命名以**现有代码为准**（代码库自身一致遵循）。日志格式（`scripts/check_log_format.py`）与 clang-format 格式检查均已接入 CI（linux job）；本地预检命令见 [docs/development-checklist.md](docs/development-checklist.md) 的 C 段。构建与跨平台的外围细节见 [docs/agents/build-appendix.md](docs/agents/build-appendix.md)。

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
- 提交前跑 `./scripts/preflight.sh`（日志格式/格式/文档链接/可移植性/MSVC hazard/内置 JSON 版本一致性/前端，与 CI 同源）；日志违规用 `// NOLINT(log-format)` 豁免

### 跨平台

仅 64 位，Windows x86_64 / MSVC 与 Linux x86_64 / GCC。策略接口（C API）不在此限制内。

非上述组合**必须 `#error`，禁止静默回退**（`((void)0)` 之类）。完整 `#if` 模板见 `docs/agents/build-appendix.md`。

---

## 五、工作方式

### 改动纪律（按此优先级）

1. **外科手术式改动。** 只碰必须碰的；不要"顺手改进"相邻代码、注释或格式；不要重构没坏的东西；匹配现有风格。**发现无关死代码只提示，不删除**；只清理**你自己**的改动造成的孤儿（多余的 import / 变量 / 函数）。判据：每一行改动都能直接追溯到用户这次的请求
2. **先说假设再动手。** 不假设、不掩饰困惑。多种解释时**全部列出**而不是默默选一个；有更简单的方案就直说；不清楚就停下来问
3. **发现文档与代码漂移：当场修文档**（见第三节收敛规则 4）
4. **源码改动优先 `edit`/`write`**：批量重写（python/sed）后必须重新读取再改（宿主文件守卫按「读过才可改」判定，stale 会拒绝）；常规路径不要用 `sed -i` / heredoc 直接写 `libs/`、`docs/`

### 日常工作流（单人 + AI 协作）

1. **开工前必读**：本文 → [docs/README.md](docs/README.md)（文档地图与冲突裁决规则）→ 相关帧契约（先读 `general.md`）
2. **小步提交**：一个提交只做一件事，每步可验证（构建 + 相关测试）。提交信息**中文**，前缀 `fix:` / `feat:` / `refactor:` / `docs:` / `test:` / `chore:` / `perf:` / `style:` / `build:` / `ci:`
3. **push 前自检**：见 [docs/development-checklist.md](docs/development-checklist.md) 的 C 段（权威且更全 —— 含 `type-check` / `lint` / `lint:css` / 契约 Freshness 门禁）。同一波内 PR 开完即可继续下一票的只读准备，CI 结果在自然断点或波末统一收；**本票合并前、开新波前 CI 必须绿**；**CI 红先修红，不叠加新改动**
4. **大改动先写文档**：spec 设计 + plan 实施计划（checkbox 制），我确认后再实现
5. **收尾**：工作区保持干净（运行产物已 gitignore；临时诊断脚本要么通用化提交、要么删除）。每波改动结束 `git status` 干净 + `git log` 可读 + CI 绿

### 标准开发循环（mattpocock 技能链路）

主循环（跑在既有硬门禁之上；按改动规模可裁剪）：

1. **设计定稿** —— `grill-with-docs`：问到无分歧；术语入 `GLOSSARY.md`、决策入 `docs/adr/`（当场）。大而模糊的探索用 `wayfinder` 建决策地图，收敛后并回 `to-spec`。
2. **立 spec** —— `to-spec` 写 GitHub Issue。涉及帧契约的 spec 必须先列契约影响面（哪份契约、哪些帧号），先改契约再改代码；满足 ADR 触发条件的在 spec 注明。
3. **拆票** —— `to-tickets`：tracer-bullet 子 issue + 原生阻塞边。
4. **实现** —— `implement` 单票（默认）或 `implement-spec` 大特性（worktree 内先跑 `setup`）。
5. **审查** —— `code-review`：从固定点起，双轴（仓库规范 / spec 一致性）。
6. **收尾** —— 动契约或有决策：契约、docs 地图、ADR 随同提交（`general.md` §11.3 checklist）；等 CI 绿；波末 `retro`。每票的冻结审查→门禁→push→**PR→合并→关 issue** 见 [docs/agents/ticket-closeout.md](docs/agents/ticket-closeout.md)。
7. **交接** —— `handoff` 文档写 `.cache/handoff/`（本沙箱 `/tmp` 每次 bash 调用隔离、不可跨会话，见 DSH 小节）。

上下文纪律：步骤 1–3 保持同一未打断窗口；每张票 `implement` 换新上下文；`retro` 在被复盘会话内、clear 之前跑。

备选入口：`diagnosing-bugs`（疑难 bug）｜`research`（后台调研成文）｜`prototype`（设计探查，经 `handoff` 双向桥接）｜`triage`（外部 issue 分诊）。

维护节奏：技能用 `npx skills@latest update` 更新（新增技能需重跑 `add`）；opencode 版本对比 [changelog](https://opencode.ai/changelog)；`doctor` 子代理体检；clang-format / conan 为固定版本，本地与 CI 保持一致（升级时同一次提交改两边）。

### 组合变更的特别提醒

- **跨进程协议改动**：先改 `docs/frame_contracts/` 契约，再按 `general.md` §11.3 checklist 逐项同步（platform 头文件、帧号登记、dzweb 领域服务、前端、测试）。契约定稿后**实现以契约为准**
- **涉及 `apps/ctp/md`**：见第一节硬门禁 1，改前必须先问我
- **帧/契约编号一经发布只增不改**（ADR 0003）

### Worktree 与 Git 禁令

- worktree 放 `.worktrees/`，`git worktree add .worktrees/<branch> -b <branch> main`
- worktree 内**需重新跑 `setup`**（conan 缓存用户级共享；沙箱下 `~/.conan2` 只读时 `setup.sh` 自动回退 `CONAN_HOME=<repo>/.cache/conan2` 并从 `~/.conan2` 播种一次，见 [build-appendix](docs/agents/build-appendix.md)）
- **并行波（同一时间多个 ticket）必须一票一 worktree**；禁止在共享工作区同时改两条线
- **共享工作区只允许当前票的 WIP**：收尾最后一步 `git switch main && git pull --ff-only`，并让 `git status` 除**已登记的并行线**外干净；把工作区留在 feature 分支就结束会话 = 直接阻塞下一条线（本波 #91→#92）
- **跨会话（>1 个会话）不提交的文档批次**：开 `docs/*` 分支提交/推送，或移出共享工作区；不要让未提交 docs 长期占着 `git status`（本波 ADR 0026–0030 从 10-22 挂到 10-23）
- 分支名 `^(feat|fix|docs|test|chore|ci|refactor|perf|style|build)/issue-<n>-<slug>`（pre-push 强制）；push 前跑 `./scripts/check-push-scope.sh <issue>`，确认 `@{u}..HEAD` 只含本票 commit
- 多终端可在不同 worktree 并行；**禁止同一 worktree 同时操作**
- 合并前确认共同祖先：`git merge-base main HEAD`，无则 cherry-pick
- **本地 git hooks 由 `setup` 自动安装**（`core.hooksPath=.githooks`；安装前 `setup` 会确认 `.githooks` 存在）：pre-commit 由 staged 文件触发——clang-format 校验 index 内容，日志格式 / docs links 为工作区全仓扫描；pre-push 拦直推 main、非快进、非白名单分支名与 tag，并按分支票号跑 `check-push-scope`。hooks 是快速护栏，**不能替代** `preflight` 与 CI。
- **push 分级授权**（见 [ADR 0036](docs/adr/0036-push-authorization-tiered-with-guardrails.md)；机器守卫 = pre-push hook + GitHub main ruleset + `.opencode` 权限）：
  - **免逐次确认**：把本票 commit push 到符合命名约定的 feature 分支、`-u` 建上游、创建/更新 PR、重跑 CI、合并后删除**本票**远端分支。前置：本票门禁绿（代码票 `gate build/test`；纯文档票 docs links / `preflight --c-only`）、`check-push-scope` 通过、一轮 review 的修复合并为一次 push。
  - **仍需确认**：合并 PR 到 main（最后一道；可按票或按波一次授权）、删除非本票分支、打 tag（opencode 会话里 `gh pr merge` / tag 为 ask；DSH 等宿主靠本规则人工确认）。
  - **禁止**：直推 main（pre-push + ruleset 双重拦截）、force push（pre-push 非快进拦截 + `.opencode` deny；ruleset 拒非快进）。应急逃生口 `DZ_ALLOW_MAIN_PUSH=1` / `DZ_ALLOW_FORCE_PUSH=1` / `DZ_SKIP_HOOKS=1` 只在真故障时用，用完在报告里写明；其中直推 main 的逃生口还需临时禁用 ruleset（`bypass_actors=[]`，admins 也无绕过）。

### 本机沙箱（DSH）

- 后台作业/CI 快照/门禁用法、跨调用 `/tmp` 与缓存回退、权限 preset、`.cache/handoff/` 约定见 [ticket-closeout](docs/agents/ticket-closeout.md) §2/§4/§8。

---

## 六、CodeGraph

仓库根存在 `.codegraph/` 时才用（没有索引就跳过）；shell 兜底 `codegraph explore "<符号>"`。**是否建索引由我决定，不要自行 `codegraph init`**

---

## 七、按需查阅

| 文档 | 内容 |
|---|---|
| [README.md](README.md) | 项目简介、特性、**4 命令工作流与命令详解**、调试流程、部署 |
| [docs/README.md](docs/README.md) | 文档地图：活跃真相源 / 架构参考分层与冲突裁决规则 |
| [docs/frame_contracts/](docs/frame_contracts/README.md) | 帧协议唯一语义真相源（先读 `general.md` 总则） |
| [docs/development-checklist.md](docs/development-checklist.md) | 新功能门禁清单（前端 / 后台 / 提交前 / 策略 SDK 四段） |
| [docs/unit-test-standard.md](docs/unit-test-standard.md) | 单元测试判据（有效性 / 覆盖 / 一致性 / 成本 / 基建），撰写与审查逐条对照 |
| [docs/architecture.md](docs/architecture.md) | 依赖方向、静态库策略、进程模型（蓝图级，细节以代码与契约为准） |
| [docs/components/](docs/components/) | 组件级设计（线程模型、通道、会话） |
| [docs/ui-trading.md](docs/ui-trading.md) | UI 系统与交易接口规划（蓝图级） |
| [docs/adr/](docs/adr/) | 架构决策记录，按编号追溯 |
| [GLOSSARY.md](GLOSSARY.md) | 领域术语表（`domain-modeling` 随改随更） |
| [docs/agents/](docs/agents/) | 技能配置（issue 载体 / 域文档规则 / 分诊标签）、构建外围细节（build-appendix）、每票收尾（ticket-closeout）、审查清单（review-checklist）、复盘取证（retro-evidence） |
| [third_party/ctp/VENDORED.md](third_party/ctp/VENDORED.md) | CTP 发布包 vendored 与升级流程（`scripts/vendor_ctp.py --sync/--check`）。**收到新的 CTP 发布包压缩包时，先读此文件并按「升级纪律」执行** |

`docs/specs/`、`docs/plans/`、`docs/superpowers/` 是**本地归档**（不在版本控制内）且**已冻结** —— 新设计与计划走 GitHub Issues；与现状冲突时以帧契约与代码为准，**不修改**。

---

## Agent skills

工程技能（mattpocock skills，全局安装于 `~/.agents/skills/`）按本仓配置工作：

- **Issue 载体**：GitHub Issues —— `gh` CLI、原生 sub-issue 与阻塞边；PR 不作为请求入口。见 [docs/agents/issue-tracker.md](docs/agents/issue-tracker.md)
- **分诊标签**：`needs-triage` / `needs-info` / `ready-for-agent` / `ready-for-human` / `wontfix`。见 [docs/agents/triage-labels.md](docs/agents/triage-labels.md)
- **域文档**：根 [GLOSSARY.md](GLOSSARY.md) + [docs/adr/](docs/adr/)（ADR 现行模板、编号只增、不引用本地归档）。见 [docs/agents/domain.md](docs/agents/domain.md)
