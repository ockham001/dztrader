# 每票收尾（Ticket Close-out）

> 用途：`/implement #N` 实现完成后的**唯一收尾流程**。人工 prompt 只说 `/implement #N` + "收尾见 docs/agents/ticket-closeout.md"，不再粘贴 6 步模板。
> 配套：[development-checklist.md](../development-checklist.md) C 段（门禁权威来源）、[review-checklist.md](review-checklist.md)（审查清单：AC 逐条 + 5 类高频 finding）、[issue-tracker.md](issue-tracker.md)（issue 操作）、`code-review` / `pr` 技能。

---

## 0. 确定同步点（不要留占位）

收尾前先看本票实际改了哪些路径，按下表机械推导要同步的文档；涉及帧/WS/REST 的**先改契约再改代码**（[general.md](../frame_contracts/general.md) §11.3）：

| 改动路径 | 必须同步 |
|---|---|
| `docs/frame_contracts/*.md` | 契约本身 + platform 头文件 + 帧号登记 + dzweb 领域服务 + 前端 + 测试（general §11.3 六项） |
| `docs/components/<topic>.md` | 组件文档与代码/契约一致 |
| 新增/改动公开行为 | `README.md`、`docs/README.md` 地图（按需） |
| 新术语 / 新决策 | `GLOSSARY.md` / `docs/adr/NNNN-*.md`（ADR 触发条件见 AGENTS §三） |
| 空实现 / 已知限制 | issue 正文或组件文档显式标注，**后续票范围必须引用票号；不跟踪的写明不跟踪理由**；不留在代码注释里 |

## 1. 冻结提交与审查

**审查对象是 commit，不是活动工作区。**

1. `git add <本票路径>`（显式路径，禁止 `git add -A`）→ `git commit`（WIP 提交也可）。
2. `/code-review since <base>`，即审 `git diff <base>...HEAD`。
3. **冻结纪律**：从 spawn verifier 到最后一个 verifier 返回，**不改任何文件、不 amend、不 push**。审查期间继续改代码会让 findings 对旧 commit 作废，整轮白跑（本波 #93：HEAD 在审查中前移 4 次，审阅者只能对旧 commit 出结论并重跑）。
4. **双轴并行 + 异步投递**：`code-review` 技能会并行 spawn Standards / Spec 两个 verifier；本环境的子代理是异步的，结果以 `subagent-settled` 通知到达。**等两条都到齐再动代码，不要因为未立即返回就重跑**；verifier prompt 里把上一轮 findings 以编号列表贴全（"逐条确认/未修说明"），避免重复审查与相反结论。
5. 轮次口径：**1 轮 = 1 次修复 + 1 个 verifier**；最多 3 轮修复。第 3 轮仍有 blocking → 修完后必须再跑 **1 轮只读验证**（不算第 4 轮修复），并把结论写进报告。禁止用改写 prompt 重跑 verifier 来"刷新"结论。
6. **等 verifier 不要 `sleep` 轮询**：子代理是异步的，完成会以 `subagent-settled` 通知到达；`wait_agent` 只对 Team 成员（`spawn_teammate`）有效，对 `subagent` 会返回 `no-active-peer`。等待窗口做**不写文件**的工作（读 diff/契约、拟关闭评论与报告、跑幂等 `preflight --c-only`）；需要真正阻塞时用 `spawn_teammate` 派 verifier 再 `wait_agent`（本波 #77/#78/#81 因 sleep 等 verifier 累计 ≈3.9h）。
7. **verifier `failed` 恢复协议**（`failed` = 子代理异常退出、无报告）：① 原 prompt 重跑一次；② 仍 `failed` → 用 `spawn_teammate` 派 verifier（Team 成员可 `wait_agent` 真正阻塞）；③ 仍不可用 → **人工代轴**逐条核对（机器门禁须已全绿），并在报告写明"verifier failed N 次、人工代轴"。#123 两个审查线程丢失、用户被迫介入、Lead 人工补轴即反例；禁止改写 prompt 反复重跑刷结论。
8. **review 完成、修复收敛后才进入 §4 push**；push 之后只允许修 CI 红。
9. 并行波（同一时间多个 ticket）必须一票一 worktree（AGENTS §五）；共享工作区只留单线。

## 2. 门禁（全绿才算完）

```bash
./scripts/gate.sh build --config release
./scripts/gate.sh test --config release
./scripts/gate.sh preflight
```

`gate.sh` 是 agent 侧的统一入口：同一 build tree 用 `flock` 串行（并行子代理不会互相踩 ninja）、自动导出 ccache 环境、输出落 `.cache/gate/`、返回**真实退出码**。人工可直接跑 `build.sh` / `test.sh` / `preflight.sh` 等价命令。**要短输出用 `./scripts/gate.sh build|test|preflight --tail N`**（成功留 N 行、失败留 max(N,30) 行）；**不要**给门禁命令接 `| tail` / `| head`——管道会把失败吞成 exit 0（#75 的 wait-ci 超时、#81 的 ninja 失败都因此被记成假绿）。

`./scripts/preflight.sh` 覆盖：日志格式、clang-format 21.x、文档链接（含契约 §N 引用）、测试可移植性、**MSVC hazard lint**（Windows 头顺序 / C4101 等 Linux 编译器拦不到的失败）、**内置 JSON 版本一致性**（conanfile / 单头 / `VENDORED.md` 三方，含自测）、前端 `type-check` / `lint` / `lint:css` / `vitest` / `gen:types` 新鲜度。任一项红就停，**先修红，不叠加新改动**。CI linux job 复用同一脚本（`--c-only`）。

> **门禁结果可追溯（#190）**：`gate.sh` 的最终 `gate: OK|FAILED` 行固定打在输出**末尾**（`| tail -2` 也可见），每次结果原子写入 `.cache/gate/last-status`。`.githooks/pre-push` 会拒绝「当前 HEAD 上最近一次门禁失败」的推送（HEAD 不符/缺失只警告；应急逃生 `DZ_ALLOW_RED_GATE=1`）。

> preflight 是**只读**门禁：它不得改动工作区任何受版本控制的文件。新增检查若需要"注入已知错误形态"来自测，必须在临时副本上做（参照 `scripts/check_json_vendor_version.py`），否则一次 Ctrl-C / SIGKILL 就会把工作区留在改坏的状态——而审查冻结窗口恰好禁止这种改动。

## 3. 提交（按路径）

- 只 `git add` 本票路径；`git status` 核对，**严禁捎带另一条线的产物**（本波踩过：提交父节点落在另一条线未 push 的 commit 上）。
- 提交信息中文，前缀 `fix:` / `feat:` / `refactor:` / `docs:` / `test:` / `chore:` / `perf:` / `style:` / `build:` / `ci:`。
- 契约、组件文档、ADR、GLOSSARY、docs 地图随同提交（第三节收敛规则 4）。

## 4. 推送与 PR

```bash
./scripts/check-push-scope.sh <issue>     # @{u}..HEAD 只允许本票 commit；分支有 CI 在跑会被拒绝
git push -u origin <branch>               # 按 AGENTS「push 分级授权」：feature 分支免逐次确认
gh pr create --base main --head <branch> --title "<type>: <摘要>（#<issue>）" --body-file <body 文件>
./scripts/wait-ci.sh --pr <n>             # 静默等 PR checks（后台作业启动，见下）
```

- 本仓用 PR 落地（`ci.yml` 只在 PR / main 触发）；main 有 GitHub ruleset `main-protection`（require PR、禁非快进、禁删除、`bypass_actors=[]`，admins 也无绕过）。payload 见 `.github/rulesets/main-protection.json`，查看/重建：`gh api repos/<owner>/<repo>/rulesets`、`gh api --method POST repos/<owner>/<repo>/rulesets --input .github/rulesets/main-protection.json`；仓库同时开启 `delete_branch_on_merge=true`。应急直推 main 需临时禁用 ruleset（`DZ_ALLOW_MAIN_PUSH=1` 只绕过本地 hook）。
- **push 分级**（[ADR 0036](../adr/0036-push-authorization-tiered-with-guardrails.md)，AGENTS 硬门禁的细化）：feature 分支 push / PR 创建更新免逐次确认，前置是本票门禁绿 + `check-push-scope` 通过 + 一轮修复一次 push；**合并 PR 到 main 仍需确认**（可按票或按波一次授权）；删除非本票分支、打 tag 逐次确认；**force push 禁止**（`pre-push` 非快进拦截 + `.opencode` deny）。本地 `.githooks/pre-push`、main ruleset 与 `.opencode` 权限是机器守卫。
- **PR 开完不阻塞等 CI**：`gh pr create` 成功后即可转下一票（读契约 / 定 spec）或本票收尾准备（拟关闭评论、清理 worktree 计划），CI 结果在自然断点或波末统一用 `./scripts/wait-ci.sh --pr <n>` 收；不要把 CI 等待写成同步步骤（本波 `wait-ci` 阻塞约 3.4h）。**边界**：不阻塞只适用于同波内的下一步准备；本票**合并前必须 CI 绿**，开新波前必须已完成合并（见 §5）。
- **`wait-ci.sh` 的退出码就是 CI 结论**：不要接 `| tail` / `| head`（会吞掉退出码）；它本来就是静默输出，只打状态变化与失败链接。feature 分支没开 PR 时 `--sha` 约 180s 就报错并提示先建 PR，不会空等到 60 分钟总超时。
- **推送前先看 `gh run list --branch <branch> --json status`**：同一分支已有 in-progress run 时不要推——会排队/取消，每次 ≈35–40 分钟。把剩余修复攒成 **1 个 commit** 再推；review 修复不要在 CI 跑的时候逐条推（本波 #93 因此取消 3 个 run）。
- 等待期间不要 `gh run watch` 全量输出、不要 `sleep` 轮询、不要反复 tail 后台日志。CI 红先用 `./scripts/ci-log.sh <run-id>` 定位：它**优先取 `last-test-log` artifact（约 200KB，秒级给出失败用例名）**，再退回失败 job 日志（Windows 可达数十 MB）；同一 run/job 已下载过会复用（`--refresh` 强制重拉，`--context N` 调上下文行数）。
- **等 CI 用后台作业**：`wait-ci.sh --pr <n>` 以 `run_in_background` 启动，等待期做只读准备（拟关闭评论 / 清理 worktree 计划 / 读下一票契约）；需要结果时 `job_output(wait=true)`，不要前台空等（2026-10-26 #146 / #147 各阻塞 21–22 分钟）。
- 修红单独提交，不叠加新改动。
- **CI 红过的失败类别，修完必须补一条静态规则**：修完只把当次修绿，下一票会以同样形态再红一次。把该类加进对应 linter 并附上该 run/票号（Windows 独有的编译失败 → `scripts/check_msvc_hazards.py` 的 R 编号；Linux 侧 → 对应 check 脚本）。理由：`check_msvc_hazards.py` 的每一项都对应一次真实的 Windows CI 红，它是**类别登记表**而不是一次性修复；被它拦下的失败（#157 的 `_wfopen` C4996）只花 5 行就永久关闭同一类盲区。修完顺手补规则，比下一票重跑一轮 35–45 分钟的 CI 便宜一个量级。

## 5. 合并 PR（硬顺序：先合并，后关 issue）

```bash
gh pr merge <n> --merge --delete-branch   # 或 --auto 交给 GitHub 排队；worktree 内会跳过本地分支删除
gh pr view <n> --json state,mergedAt,mergeCommit -q '.state, .mergedAt, .mergeCommit.oid'   # 必须是 MERGED
git switch main && git pull --ff-only
git merge-base --is-ancestor <merge-commit> main && echo merged
```

- **PR 未 MERGED 前不得关闭 issue**。CI 绿 ≠ 已落地（本波 #91 关单 40 分钟后才合、#92 关单 5 小时后才合，期间 #93 合入造成冲突，收尾多解一次冲突 + 多跑两轮 CI）。
- `--delete-branch` 在 worktree 里会跳过本地删除（分支被 checkout）：先 `git worktree remove`，再 `git branch -d`；远端分支用 `git ls-remote --heads origin <branch>` 确认为空。
- 合并会触发 main 的 CI（merge commit 跑 Linux，约 15 分钟；Windows job 只在 PR 跑）；共享工作区切回 main 后不要停在旧 feature 分支。
- 若要显式等 main CI：`./scripts/wait-ci.sh --branch main`（约 11–18 分钟；不想阻塞收尾就留到波末批量抽查）。

## 6. 关闭 issue（PR 已合并后）

```bash
gh issue view <n> --json number,state,title    # 先确认状态与标题
gh pr view <n> --json state -q '.state'        # 必须输出 MERGED
gh issue close <n> --comment "<关闭评论>"
```

关闭评论模板（交付 / 已知限制 / 提交 / CI / PR / 验证 / 审查）：

```
交付：<1-2 句，对应用户可感知的行为>
已知限制：<无则写"无"；后续票范围注明票号；不跟踪的写明不跟踪理由>
提交：<hash...>（<一句话范围>）
CI：<run link> Windows + Linux 绿
PR：#<n>（merged <merge-commit>）
验证：ctest <x>/<y>、preflight 全绿、<E2E/真机验证（如适用）>
审查：<n> 轮双轴收敛 / 剩余 <m> 条 non-blocking（列票）
Refs: <contract/component/docs 路径>
```

关闭后检查下游依赖是否解除：`gh issue view <child> --json blockedBy`（返回 `{nodes:[],totalCount}`，用 `.blockedBy.nodes[]`；或 `gh api repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by` 拿数组），或在票的报告里写清"#X 的 blocked_by 从 N → 0"。

### spec 母票收口（子票全部关闭后）

实现票逐张关闭不等于 spec 收口。`[spec]` 母票必须在子票全部关闭后单独处置：

0. **先跑台账体检**：`python3 scripts/check_ledger.py --strict`，按报告清掉悬空承诺（docs 里指向已关闭票的「后续票」）与过期的 `needs-triage`，再开始收口。
1. **清点差异**：逐条核对 spec 的用户故事 / 实现决策，分成三类——已交付（给出票号）、已被后续决策推翻或收窄（引用 ADR / spec / PR，并在母票留评论）、不跟踪（写明理由，遵守 §0「不跟踪的写明不跟踪理由」）。
2. **写收口评论**：沿用 §6 模板（交付 / 已知限制 / 提交 / CI / PR / 验证 / 审查），另加「spec 逐条符合性」与差异清单；参考 #120 的关闭评论。
3. **关闭母票**：`gh issue close <spec> --comment "<收口评论>"`。母票不作为长期真相源留在 backlog——真相源是契约 / ADR / 文档；遗留的 `ready-for-agent` 等标签会让后续 agent 把已完成的 spec 当成待实现任务。
4. **先有票再关**：仍想做的事必须先有票（或明确的 out-of-scope 记录），否则不允许关闭母票。

> 动机：#41（dzsysmon spec）的 13 张子票于 2026-10-24 全部关闭，母票却一直开放到 2026-10-26 才被发现——本流程原先只覆盖「单票 → 关本票」（mattpocock 原版 `implement-spec` 靠 integration branch 的 PR 关闭 spec，本仓改成一票一 PR 后该机制丢失）。

## 7. 清理、交还与报告

- `git worktree remove .worktrees/<branch>` + `git branch -d <branch>`；确认 `.worktrees/` 为空（含 `.cache/` 残留）。
- 远端分支：`git push origin --delete <branch>`（`gh pr merge --delete-branch` 已删则跳过），`git ls-remote --heads origin <branch>` 确认为空。
- **共享工作区交还**：`git switch main && git pull --ff-only`；`git status` 只允许出现**已登记的并行线**产物（没有并行线时应完全干净）。不把工作区留在 feature 分支就结束会话——会直接阻塞下一条线（本波 #91→#92：共享工作区留在 feat 分支，另一会话卡 15 分钟 + 人工介入）。
- 报告固定包含：审查轮数与结论、改动文件数、commit hash、CI run 链接与结论、PR 链接与 merge commit、preflight 结果、issue 状态、工作区/分支状态。

## 反模式（实际踩过）

- 审未提交工作区 → 审查期间工作区被改（含并行线提交），结论失效，提交后还要 amend。
- 审查期间继续 amend/改代码、或 verifier 没立即返回就重跑 → findings 对旧 commit 作废、重复审查（#93）。
- 并行 spawn 多个 verifier → 同一分钟给出相反结论，白跑一轮。
- `sleep` / `gh run watch` 等 CI、`gh run view --log-failed` 因 `~/.cache/gh` 只读失败 → 时间与上下文双浪费。
- **CI 绿就关 issue / PR 未合并就关 issue** → 下游卡住、PR 悬空与冲突返工（#91/#92）。
- **共享工作区留在 feature 分支 / 长期不提交的文档批次占着 `git status`** → 阻塞并行会话（#91→#92；ADR 0026–0030 从 10-22 挂到 10-23）。
- **逐条 push review 修复** → 每次推送排队/取消 ≈35–40 分钟 CI（#93 取消 3 个 run）。
- 门禁/等待命令接 `| tail`、`| head` → 退出码被管道吞掉，失败记成 exit 0 假绿（#75 wait-ci 超时、#81 ninja 失败）。要短输出用 `./scripts/gate.sh … --tail N`。
- 用 bash `sleep` 等异步子代理 → 白等且触发重复工具提醒（#78 19 次 5028s、#81 10 次 2400s）；子代理完成会自动通知。
- 省略 issue close → 人工追问，下游 `blocked_by` 卡住。
- 共享工作区做并行票 → 提交父节点带上另一条线，push 时一并发布。
- `/tmp` 存跨调用状态 → 下次 bash 调用文件消失（DSH 沙箱每次调用隔离 `/tmp`；临时文件写 `.cache/`）。
