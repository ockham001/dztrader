# ADR 0017: AI 协作开发流程定型（mattpocock 技能链路 + GitHub Issues）

## Status

Accepted（2026-10-16）

> 决策 4 的「`git push` ask」口径由 [ADR 0036](0036-push-authorization-tiered-with-guardrails.md) 取代（2026-10-27）；其余决策继续有效。

## Context

项目历经 claude / trae / workbuddy / zcode / superpowers 多套 AI 协作工具，制品与配置散落；2026-09-30 完成 AGENTS.md 开放约定迁移后，流程层仍是空白：无 issue 载体、无标准开发循环、硬门禁靠口头约定、日志与格式检查未入 CI。2026-10-16 经两轮 grilling + 一轮「项目初期、不考虑兼容、按最佳实践」复审逐项确认后定型。

## Decision

1. **唯一流程**：mattpocock skills（全局 `~/.agents/skills/`，`npx skills` 维护）为标准 AI 协作流程；superpowers 退役并清退（配置、缓存、SDD 制品）。
2. **标准开发循环**（以 AGENTS.md「标准开发循环」为准）：`grill-with-docs` → `to-spec` → `to-tickets` → `implement`（或 `implement-spec`）→ `code-review` → `retro`；`wayfinder` 收敛后并回 `to-spec`，`triage` 处理外部 issue。
3. **issue 载体**：GitHub Issues（`gh` CLI、原生 sub-issue 与阻塞边）；设计与计划不再落本地归档（`docs/specs`、`docs/plans` 冻结）。
4. **硬门禁机器化**：opencode v2 权限（`apps/ctp/md/*` 编辑 ask、`cmake --build` 与 `npm run build` deny、`git push` ask 且 force deny）。
5. **防漂移**：log-format 与 clang-format（21.x）双门禁入 CI；全仓一次性格式基线提交并记入 `.git-blame-ignore-revs`。

## 为什么不选另一条路

- **本地 markdown 载体（`.scratch/`）**：单人 + 公开仓库下，GitHub Issues 不占目录树，原生阻塞边与五标签分诊开箱即用。
- **保留 superpowers 并行**：与「同一能力不允许两套实现」直接冲突；其唯一缺口（并行子代理编排）由 `implement-spec` 覆盖。
- **引入 PR 流程**：solo + trunk-based；`code-review` 以提交区间为固定点、不依赖 PR；出现外部贡献者时再议。
- **迁就旧制品与旧约定**：项目初期无兼容包袱，以官方文档与上游最佳实践为准（对齐审计见设计文档 §4）。

## Consequences

- 流程产物默认中文；ADR 沿用现有模板（`docs/agents/domain.md` 记录）；新 ADR 不引用本地归档路径。
- 权限 deny 为绝对约束（同意也无法绕过两条被禁命令与 force push）；如需放开，走新 ADR 推翻本决策。
- 技能更新走 `npx skills@latest update`；项目级 opencode 配置以 `.opencode/opencode.json`（入库）为准。
