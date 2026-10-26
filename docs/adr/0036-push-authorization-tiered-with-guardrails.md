# ADR 0036: push 分级授权 + 机器护栏

## Status

Accepted（2026-10-27）

Supersedes：ADR 0017 决策 4 中「`git push` 为 ask」的权限口径（其余决策继续有效）。

## Context

ADR 0017（2026-10-16）把 opencode 的 `git push` 设为 ask，并由 AGENTS 规定「未经用户同意不得推送远程」。当时没有分支保护、没有本地 hooks，人工确认是唯一防线。

2026-10-26 的 retro 复盘（#142 / #145 / #149）暴露三个问题：

1. **人工确认挡不住真正的风险**：确认只覆盖「要不要 push」，不校验分支范围、是否直推 main、是否 force；DSH 等宿主没有 opencode 权限层，`git push` 在 workspace-write 沙箱下完全不弹审批（当天 4 次 push 均无机器拦截）。
2. **每票一次确认是纯摩擦**：feature 分支 push / 开 PR 没有需要人决策的信息，却要把每张票停下来等一句「可以推送」。
3. **main 完全无护栏**：无 branch protection / ruleset，一次手滑就能绕过 PR 与 CI 直推 main。

## Decision

1. **主守卫交给机器**：main 的 GitHub ruleset（require PR、禁非快进、禁删除、`bypass_actors=[]`、`current_user_can_bypass=never`）+ 本地 `.githooks/pre-push`（拦直推 main / tag / 非快进 / 非白名单分支名，并按被推的 `local_sha` 跑 `check-push-scope`）。
2. **分级授权**（AGENTS「push 分级授权」）：
   - **免逐次确认**：白名单 feature 分支（`^(feat|fix|docs|test|chore|ci|refactor|perf|style|build)/`）的 push、创建/更新 PR、重跑 CI、合并后删除本票远端分支。前置：本票门禁绿 + `check-push-scope` 通过 + 一轮 review 的修复合并为一次 push。
   - **仍需人工确认**：合并 PR 到 main（可按票或按波一次授权）、删除非本票分支、打 tag。
   - **禁止**：直推 main、force push。
3. **opencode 权限层只做粗粒度补充**：`git push *` allow，force / mirror / `+refspec` / main / tag / `--delete` / `--prune` / `gh pr merge` 用 ask 或 deny 覆盖常见拼写；权威判定仍在 hook + ruleset。
4. **逃生口**只在真故障时使用，用完在报告里写明：`DZ_SKIP_HOOKS=1`（跳过 hooks）、`DZ_ALLOW_FORCE_PUSH=1`、`DZ_ALLOW_TAGS=1`、`DZ_ALLOW_NONSTANDARD_BRANCH=1`、`DZ_ALLOW_MAIN_PUSH=1`（直推 main 还需临时禁用 ruleset）。

## Alternatives

- **(A) 维持「每次 push 都 ask」**：否决。确认不校验内容，真正的风险（main / force / 范围）仍需另建护栏；而每票一次等待是纯摩擦。
- **(B) 全 allow、无任何护栏**：否决。DSH 等无权限层的宿主下等于放弃 main 保护；当天已证明没有机器拦截。
- **(C) 只靠 opencode 权限**：否决。`.opencode/opencode.json` 只对 opencode 生效；DSH 会话、脚本、人工操作都绕过。
- **(D) 依赖 CI 作为唯一门禁**：否决。CI 在 PR 阶段才有，保护不了 main 直推与 force push。

## Consequences

- ADR 0017 决策 4 的「git push ask」由本 ADR 取代；ADR 0017 其余决策（流程链路 / issue 载体 / 其他硬门禁）继续有效。
- `.githooks/`、`.gitattributes`（hooks 必须 LF）、`setup.sh` / `.ps1`（安装 hooks）成为流程的一部分；hooks 是快速护栏，**不能替代** preflight 与 CI。
- `.opencode/opencode.json` 权限与 AGENTS / ticket-closeout 三处口径必须同步（本次同批修改）。
- **残余风险**：`bypass_actors=[]` 意味着 ruleset 无法被任何人绕过，紧急修复 main 需先临时禁用 ruleset——这是有意取舍（JIT 运维成本换主分支完整性）。
- 后续（Batch B）：CI 增加 always-run 的 `pr-gate` 聚合 job 后，可给 ruleset 加 required status checks。

## References

- 证据与复盘：2026-10-26 retro（#149 背景）、#142 / #145 收尾记录
- 相关 ADR：[0017](0017-ai-collaboration-workflow.md)（本 ADR 取代其决策 4 的 push 口径）
- 相关文档：[ticket-closeout.md](../agents/ticket-closeout.md) §4、[开发清单](../development-checklist.md) C 段、`.github/rulesets/main-protection.json`
