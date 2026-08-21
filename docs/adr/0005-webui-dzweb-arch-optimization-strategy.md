# ADR 0005: WebUI/dzweb 架构优化采用"保护网先行"的六阶段增量演进战略

## Status

Accepted

## Context

WebUI 前后端长期累积四类痛点：新增功能重复出错（范式只在注释里）、组件规则不统一（UI 无收敛底座）、改 A 坏 B（无回归保护网）、前后端联动出错（帧契约 md ↔ C++ ↔ 手写 TS 类型三层人工对齐）。2026-08-21 前后端全面审查结论：模型层（WS 镜像模型、单写者 store、契约先行、dzweb 两线程事件驱动模型）健康，问题集中在执行层——规则停留在"文档约定"档，无机器强制力。

## Decision

采用六阶段增量演进战略：P0 保护网（类型/lint/测试 CI 门禁）→ P1 契约单源化（TS 类型从 schema 单源生成）→ P2 dzweb 结构收尾（角色降级踢连接、全局指针注入化、双通道守卫收敛）→ P3 交互范式固化（usePending 实例化、useOperation 抽取、领域功能模板）→ P4 设计系统与组件三层规范 → P5 多端适配 → P6 持续门禁。

核心原则：**先立保护网再动结构**；规则从"文档约定"逐档下沉到"类型/抽象/lint 强制"；**不推倒重写**（模型层资产不动，只修执行层）。

不用另一方案（推倒重写/大爆炸重构）的原因：模型层经审查确认健康，痛点全在执行层；且无保护网时任何重构都会放大"改 A 坏 B"。

## Consequences

- **Positive**：每阶段独立可验收、可暂停；四个痛点逐个根治；规则获得机器执行力后新增功能从"读注释照抄"变为"填模板"。
- **Negative**：P0/P1 前期投入无直接功能产出，整体周期拉长；P1 引入生成工具链的维护成本。
- **已知约束**：dzweb 线程模型（2 工作线程 + 闲置主循环 + IO 线程惰性日志 tail 定时器）经用户确认锁定，本战略所有阶段不得触碰。
- 详细规划、审查问题清单与进展记录：`docs/plans/2026-08-21-webui-dzweb-arch-optimization.md`（唯一计划真相源，跨会话滚动维护）。

## References

- 用户裁决：2026-08-21——"先有个优秀的架构，在此基础之上进行规则设定"（架构优化 + 规则约束统一立项）
- 前置审查：同日前端（stores/composables/api/views）+ dzweb（controller/领域服务/帧路由）全面审查
- 相关既有资产：ADR 0004（IO 循环与线程模型）、契约目录 frame_contracts（单源化对象）

## 收官总结（2026-08-21 六阶段 P0–P6 全部完成）

> 战略目标：把"文档约定"档的规则逐层下沉到"类型/抽象/lint 强制"档，四条痛点根因彻底机器化。
> 约束：dzweb 线程模型锁定不可触碰；增量演进不推倒重写；每阶段独立可验收。四痛点逐一处理如下。

| 阶段 | 目标 | 落地成果（机器强制档） |
|---|---|---|
| P0 保护网 | 改错被机器发现 | type-check 独立 + 进 CI；eslint+stylelint 引入；7 view 测试补齐；后端角色降级踢 WS 连接测试 |
| P1 契约单源 | 前后端联动错根治 | WS/领域类型迁入 schema；`scripts/gen-types.mjs`→`generated.ts`；`WsDataByType` 判别联合；CI 契约 Freshness（改动未重新生成即失败） |
| P2 后端收尾 | dzweb 可复制模板 | 全局函数指针→`DataChangeNotifier` 注入；REST/WS 双通道守卫收 `ControlGuard`；C2S 下沉领域服务；io 线程 SQLite 边界归档 |
| P3 交互范式 | 新增功能=填模板 | `usePending` 实例化+领域单例；`useOperation` 抽领域 runner；行情源四件套模板+注册表契约测试；`CLEAR_BY_RTN` 清理触点声明式；userManagement 原型残留清理 |
| P4 设计系统 | 视觉规则收敛一处 | token 补全灭 fallback(55→0)；`dz/token-literal` 修复休眠→升 error→CI；`dz/no-ds-namespace` 组件三层；theme.css 主题扩展五步规范 |
| P5 多端适配 | 断点归布局组件 | Tailwind 断点档+`useBreakpoint`(matchMedia 事件驱动)；AppShell CSS 主导归一(去 inline/!important 打架)；触摸规范 pointer:coarse≥44px；dashboard 一屏监控规范固化；断点白名单测试防野断点 |
| P6 持续门禁 | 常驻守护 | 门禁对账确认双平台 CI 全就位；`docs/development-checklist.md` 新功能清单逐项映射自动化检查 |

**成果量化**：前端测试从 P0 前的 ~200 增至 275；lint:css 瑕疵 55→0 且红线升 error；断点从 5+ 个散点归一为 4 档标准；新增字段/组件/断点/操作均有对应门禁。

**明确不做（预留给结构稳定后）**：Playwright 视觉回归截图 diff——P4/P5 刚改完三层与断点，布局结构未冻结，现做基准图易频繁失效。

**后续可持续入口**：新增功能对照 `docs/development-checklist.md`；契约/生成物改动走 `frame_contracts/`；规则再演进以本 ADR 与本地 plan §6/§8 为准。
