# 新功能开发 Checklist（P6 持续门禁配套）

> **用途**：新增或改动功能（前端组件/store/composable、后端 controller/领域服务/契约）时，
> 逐项对照。每一项都对应**已有的自动化检查**——不是人肉勾选，而是"做对了 CI 自然绿"。
> 提交时若某项对应的 CI 门禁未包含到你的改动，说明该改动可能越过了系统边界，需复核。
>
> 配套：`.claude/rules/workflow.md`（push 前验证清单）+ `docs/frame_contracts/README.md`（跨进程协议清单）。

---

## A. 前端（`apps/webui/frontend`）

| # | 动作 | 对应的自动化门禁（机器强制） |
|---|------|------------------------------|
| A1 | 新增/修改组件或 store | `npm run type-check`（Vue/TS 类型）；`npm run lint`（eslint 0 警告） |
| A2 | 组件内写样式 | `npm run lint:css`：禁止裸色/字号/间距字面量（`dz/token-literal`）；禁止 domain 里重定义 `.ds-*`（`dz/no-ds-namespace`）；@media 宽度必须 ∈ 标准档（断点白名单） |
| A3 | 新增颜色/间距/圆角 | 先进 `design-tokens.css`/`theme.css` token，再引用 `var(--x)`；勿裸写 |
| A4 | 新增响应式布局 | 用 Tailwind 档断点（640/768/1024/1280）写 CSS @media，或行为分支用 `useBreakpoint`；勿造野断点 |
| A5 | 新增操作（pending 流程） | 走领域 runner（`useOperation`）+ 在 `CLEAR_BY_RTN` 声明清理触点；勿手写分散清理 |
| A6 | 新增行情源类型 | 四件套：新建 `<Type>Card.vue`（复制 `MarketSourceCardTemplate`）+ `marketSourcesCardRegistry` 加一行（键小写）；store/api/view 免改 |
| A7 | 处理 WS 广播消息 | 类型经 `src/types/generated.ts`（schema 单源）+ 判别联合 `WsDataByType` 类型化；handler 勿用宽松 cast |
| A8 | 加测试 | 现有 spec 目录补充；改动后 `npm test` 全绿 |

## B. 后台（`apps/webui` C++ / 契约）

| # | 动作 | 对应的自动化门禁 |
|---|------|------------------|
| B1 | 新增控制器/领域服务 | `ctest`（webui 套件）绿色；（后台门禁在 CI 的 C++ Tests 步骤） |
| B2 | 改帧/WS/REST 契约 | **先改 `docs/frame_contracts/`**，再按 00-general §11.3 逐项同步（platform 头文件、帧号登记、dzweb、前端、测试）；契约 Freshness 步骤防生成类型漂移 |
| B3 | 改 WS payload 字段 | 改 schema → 跑 `npm run gen:types` → 前端类型自动更新；CI 校验 generated.ts 最新 |
| B4 | 新增角色/权限判断 | 对照 `ControlGuard` 共享守卫，避免 REST/WS 平行实现 |

## C. 提交前（每次 push）

- [ ] `npm run type-check` ✅
- [ ] `npm run lint` + `npm run lint:css` ✅（0 error / 0 warning）
- [ ] `npm test` 全绿（前端 vitest）
- [ ] `ctest` 全绿（后台；含冒烟 smoke）
- [ ] `git status` 干净，改动为小步提交（信息中文，`fix:/feat:/refactor:/docs:/test:/chore:` 前缀）
- [ ] 契约/生成物有改动时 CI 的 Freshness 步骤绿

> 若某项门禁**当前未在你的改动上跑**（例如只改了一个 `.vue` 但改了全局样式），
> 请人工确认它确无跨层影响——机器门禁覆盖不到的正是这类"看似局部其实全局"的改动。