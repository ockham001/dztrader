# 新功能开发 Checklist（P6 持续门禁配套）

> **用途**：新增或改动功能（前端组件/store/composable、后端 controller/领域服务/契约、策略 SDK）时，
> 逐项对照。每一项都对应**已有的自动化检查**——不是人肉勾选，而是"做对了 CI 自然绿"。
> 提交时若某项对应的 CI 门禁未包含到你的改动，说明该改动可能越过了系统边界，需复核。
>
> 配套：仓库根 `AGENTS.md`（硬门禁与日常工作流）+ `docs/frame_contracts/README.md`（跨进程协议清单）。
> 本文 C 段是 **push 前验证清单的权威来源**，比 AGENTS.md 的摘要更全。

---

## A. 前端（`apps/webui/frontend`）

| # | 动作 | 对应的自动化门禁（机器强制） |
|---|------|------------------------------|
| A1 | 新增/修改组件或 store | `npm run type-check`（Vue/TS 类型）；`npm run lint`（eslint 0 警告） |
| A2 | 组件内写样式 | `npm run lint:css`：禁止裸色/字号/间距字面量（`dz/token-literal`）；禁止 domain 里重定义 `.ds-*`（`dz/no-ds-namespace`）；@media 宽度必须 ∈ 标准档（断点白名单，实际由 `breakpointWhitelist.spec.ts` 守）；`.css` 必须能被标准 CSS 解析器解析（语法错必红——`customSyntax` 只对 `**/*.vue` 生效；注释提前闭合由选择器类规则 + `themeTokens.spec.ts` 兜底） |
| A3 | 新增颜色/间距/圆角 | 先进 `design-tokens.css`/`theme.css` token，再引用 `var(--x)`；勿裸写。主题色 token 须三段对称（`.dark` 与 system token 集合相等且同值、`:root` 字面量 ⊆ 深色段、深色段 ⊆ `:root` 浅色基线、`var()` 别名目标三段均覆盖），由 `themeTokens.spec.ts` 守 |
| A4 | 新增响应式布局 | 用 Tailwind 档断点（640/768/1024/1280）写 CSS @media，或行为分支用 `useBreakpoint`；勿造野断点 |
| A5 | 新增操作（pending 流程） | 走领域 runner（`useOperation`）+ 在 `CLEAR_BY_RTN` 声明清理触点；勿手写分散清理 |
| A6 | 新增行情源类型 | 四件套：新建 `<Type>Card.vue`（复制 `MarketSourceCardTemplate`）+ `marketSourcesCardRegistry` 加一行（键小写）；store/api/view 免改 |
| A7 | 处理 WS 广播消息 | 类型经 `src/types/generated.ts`（schema 单源）+ 判别联合 `WsDataByType` 类型化；handler 勿用宽松 cast |
| A8 | 加测试 | 现有 spec 目录补充；改动后 `npm test` 全绿 |
| A9 | 新增可点击/可操作元素、对话框 | 无障碍规则门禁（`eslint-plugin-vuejs-accessibility` recommended，见 `eslint.config.js`）：`div` + `@click` 必须补键盘等效路径与 `role`/`tabindex`（键盘可达）、对话框必须有 `role="dialog"` + `aria-modal` + `aria-labelledby` 并走 `useFocusTrap`（焦点移入/框内循环/关闭还原，见 `src/composables/useFocusTrap.ts`）、需要锁背景滚动的走 `useScrollLock`（引用计数，见 `src/composables/useScrollLock.ts`）、图标按钮必须有 `aria-label`。**静态装饰性元素**（拖拽分隔条、点击遮罩）用 `role="presentation"` 或行内 `eslint-disable-next-line` 并写明理由。存量豁免：`label-has-for` / `form-control-has-label` 两条当前关闭（97 条存量，修完并把 `eslint.config.js` 里两行改回 `error` 才算完，清单见 #233） |

## B. 后台（`apps/webui` C++ / 契约）

| # | 动作 | 对应的自动化门禁 |
|---|------|------------------|
| B1 | 新增控制器/领域服务 | `ctest`（webui 套件）绿色；（后台门禁在 CI 的 C++ Tests 步骤） |
| B2 | 改帧/WS/REST 契约 | **先改 `docs/frame_contracts/`**（先读 `general.md` 总则），再按 `general.md` §11.3 逐项同步（platform 头文件、帧号登记、dzweb、前端、测试）；契约 Freshness 步骤防生成类型漂移。项目技能 `frame-contract-change`（`.opencode/skills/frame-contract-change/SKILL.md`）封装了此流程：opencode 会话可加载；DSH 等其它宿主按 `general.md` §11.3 手动执行（技能不可用不等于流程可省） |
| B3 | 改 WS payload 字段 | 改 schema → 跑 `npm run gen:types` → 前端类型自动更新；CI 校验 generated.ts 最新 |
| B4 | 新增角色/权限判断 | 对照 `ControlGuard` 共享守卫，避免 REST/WS 平行实现 |

## C. 提交前（每次 push）

- [ ] 本地 git hooks 已装：`git config --get core.hooksPath` 输出 `.githooks`（`setup` 自动安装）。pre-commit 由 staged 文件触发——clang-format 校验 index 内容，日志格式 / docs links 为工作区全仓扫描；**不能替代** `preflight`。pre-push 拦直推 main / 非快进 / 非白名单分支名；最近一次门禁在**当前 HEAD** 失败会拒绝推送（`.cache/gate/last-status`；HEAD 不符/缺失只警告，应急 `DZ_ALLOW_RED_GATE=1`）。
- [ ] `./scripts/preflight.sh` 全绿（日志格式 / clang-format 21.x / 文档链接（含契约 §N 引用：活跃文档 + apps/libs 源码注释）/ 测试可移植性+临时目录唯一化 / **MSVC hazard lint** / **内置 JSON 版本一致性** / **gtest 用例发现超时纪律（#227）** / 前端 `type-check` + `lint` + `lint:css` + `vitest` + `gen:types` 新鲜度，一条命令一个结论）——CI linux job 复用同脚本的 C 段（`--c-only`）；首次需先 `./scripts/build.sh` 生成 `node_modules`
- [ ] `./scripts/build.sh --config release` 通过（agent 侧用 `./scripts/gate.sh build --config release`：同 build tree 串行 + ccache 环境 + 真实退出码；管道用法见 [ticket-closeout](agents/ticket-closeout.md) §2）
- [ ] `./scripts/test.sh` 全绿（后台 fast 门禁；含冒烟 smoke）；穷举/基准按需 `--label slow` / `--label perf`（Windows `-Label`）；agent 侧 `./scripts/gate.sh test`
- [ ] `git status` 干净，改动为小步提交（信息中文，`fix:/feat:/refactor:/docs:/test:/chore:/perf:/style:/build:/ci:` 前缀）
- [ ] 契约/生成物有改动时 CI 的 Freshness 步骤绿

> 收尾按 [agents/ticket-closeout.md](agents/ticket-closeout.md)：push 后建 PR → `./scripts/wait-ci.sh --pr <n>` **静默等 PR checks** → **合并 PR 后**再关 issue。等待只输出状态变化和失败摘要；失败用 `./scripts/ci-log.sh <run-id>` 只取错误行（优先取 200KB 的 `last-test-log` artifact）。不要 `gh run watch` 全量输出，不要 `sleep` 轮询，也不要在同一分支有 CI 在跑时重复 push（会排队/取消，每次 ≈35–40 分钟）。

> 若某项门禁**当前未在你的改动上跑**（例如只改了一个 `.vue` 但改了全局样式），
> 请人工确认它确无跨层影响——机器门禁覆盖不到的正是这类"看似局部其实全局"的改动。

---

## D. 策略 SDK（`libs/strategy_api`）

| # | 动作 | 对应的自动化门禁 / 检查 |
|---|------|--------------------------|
| D1 | 改 C 接口 / 公开头文件 | `ctest` 全绿；`-Werror`（Release）/ `-Wconversion` 由构建脚本强制 |
| D2 | 改 `dz_next_event` 白名单/拦截、引擎回调或生命周期 | **先改 `docs/frame_contracts/strategy.md`**（白名单/拦截清单/引擎注释同步），再改代码与测试；`general.md` §11.3 checklist |
| D3 | 新增/修改 `DZ_API` 函数或错误码 | 头文件 doxygen + `dz_errstr` 映射 + 至少一条测试（错误路径也要覆盖） |
| D4 | 改 C++ 引擎（`strategy_engine.h`/`strategy_base.h`） | 引擎测试套件（`StrategyEngineTest`）全绿；启动自检行/回调探测变更需同步注释与测试 |
| D5 | 策略 SDK 变更（无前端/契约联动时） | 仍须跑 C 段的本地预检：log-format / clang-format / test-portability / docs-links / JSON vendor version |
| D6 | 改内置第三方头（`dztrader/third_party/`，当前只有 nlohmann/json 单头） | **整文件替换**，不做局部补丁；`conanfile.py` 与单头版本**同一次提交**升级（ADR 0026 决策 6）；`scripts/check_json_vendor_version.py --selftest` + 无参校验三方一致（conanfile / 版本宏 / `VENDORED.md` 的版本与 SHA-256），已接入 preflight |
| D7 | 改 `dztrader/json.h`（JSON 取用入口）或增删内置头 | 双模编译检查 `ctest -R "json_probe"`（`json_sdk_probe_builtin` / `_external` 各编译并运行一次，**只链 SDK include 路径**）+ `aggregate_header_probe`（`<dztrader.h>` 不得引入内置单头）；两种模式都必须绿 |
