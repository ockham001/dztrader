# 每票收尾（Ticket Close-out）

> 用途：`/implement #N` 实现完成后的**唯一收尾流程**。人工 prompt 只说 `/implement #N` + "收尾见 docs/agents/ticket-closeout.md"，不再粘贴 6 步模板。
> 配套：[development-checklist.md](../development-checklist.md) C 段（门禁权威来源）、[issue-tracker.md](issue-tracker.md)（issue 操作）、`code-review` / `pr` 技能。

---

## 0. 确定同步点（不要留占位）

收尾前先看本票实际改了哪些路径，按下表机械推导要同步的文档；涉及帧/WS/REST 的**先改契约再改代码**（[general.md](../frame_contracts/general.md) §11.3）：

| 改动路径 | 必须同步 |
|---|---|
| `docs/frame_contracts/*.md` | 契约本身 + platform 头文件 + 帧号登记 + dzweb 领域服务 + 前端 + 测试（general §11.3 六项） |
| `docs/components/<topic>.md` | 组件文档与代码/契约一致 |
| 新增/改动公开行为 | `README.md`、`docs/README.md` 地图（按需） |
| 新术语 / 新决策 | `GLOSSARY.md` / `docs/adr/NNNN-*.md`（ADR 触发条件见 AGENTS §三） |
| 空实现 / 已知限制 | issue 正文或组件文档显式标注，不留在代码注释里 |

## 1. 冻结提交与审查

**审查对象是 commit，不是活动工作区。**

1. `git add <本票路径>`（显式路径，禁止 `git add -A`）→ `git commit`（WIP 提交也可）。
2. `/code-review since <base>`，即审 `git diff <base>...HEAD`。
3. **冻结纪律**：从 spawn verifier 到最后一个 verifier 返回，**不改任何文件、不 amend、不 push**。审查期间继续改代码会让 findings 对旧 commit 作废，整轮白跑（本波 #93：HEAD 在审查中前移 4 次，审阅者只能对旧 commit 出结论并重跑）。
4. **双轴并行 + 异步投递**：`code-review` 技能会并行 spawn Standards / Spec 两个 verifier；本环境的子代理是异步的，结果以 `subagent-settled` 通知到达。**等两条都到齐再动代码，不要因为未立即返回就重跑**；verifier prompt 里把上一轮 findings 以编号列表贴全（"逐条确认/未修说明"），避免重复审查与相反结论。
5. 轮次口径：**1 轮 = 1 次修复 + 1 个 verifier**；最多 3 轮修复。第 3 轮仍有 blocking → 修完后必须再跑 **1 轮只读验证**（不算第 4 轮修复），并把结论写进报告。禁止用改写 prompt 重跑 verifier 来"刷新"结论。
6. **review 完成、修复收敛后才进入 §4 push**；push 之后只允许修 CI 红。
7. 并行波（同一时间多个 ticket）必须一票一 worktree（AGENTS §五）；共享工作区只留单线。

## 2. 门禁（全绿才算完）

```bash
./scripts/build.sh --config release
./scripts/test.sh --config release
./scripts/preflight.sh
```

`./scripts/preflight.sh` 覆盖：日志格式、clang-format 21.x、文档链接、测试可移植性、前端 `type-check` / `lint` / `lint:css` / `vitest` / `gen:types` 新鲜度。任一项红就停，**先修红，不叠加新改动**。CI linux job 复用同一脚本（`--c-only`）。

## 3. 提交（按路径）

- 只 `git add` 本票路径；`git status` 核对，**严禁捎带另一条线的产物**（本波踩过：提交父节点落在另一条线未 push 的 commit 上）。
- 提交信息中文，前缀 `fix:` / `feat:` / `refactor:` / `docs:` / `test:` / `chore:` / `perf:` / `style:` / `build:` / `ci:`。
- 契约、组件文档、ADR、GLOSSARY、docs 地图随同提交（第三节收敛规则 4）。

## 4. 推送与 PR

```bash
./scripts/check-push-scope.sh <issue>     # @{u}..HEAD 只允许本票 commit；分支有 CI 在跑会被拒绝
git push -u origin <branch>               # 推送需用户同意（AGENTS 硬门禁）
gh pr create --base main --head <branch> --title "<type>: <摘要>（#<issue>）" --body-file <body 文件>
./scripts/wait-ci.sh --pr <n>             # 静默等 PR checks（Windows 约 35–37 分钟）
```

- 本仓用 PR 落地（`ci.yml` 只在 PR / main 触发；main 不受保护也保持一致）。创建 PR 前可用 `pr` 技能整理 body。
- **推送前先看 `gh run list --branch <branch> --json status`**：同一分支已有 in-progress run 时不要推——会排队/取消，每次 ≈35–40 分钟。把剩余修复攒成 **1 个 commit** 再推；review 修复不要在 CI 跑的时候逐条推（本波 #93 因此取消 3 个 run）。
- 等待期间不要 `gh run watch` 全量输出、不要 `sleep` 轮询、不要反复 tail 后台日志；CI 红用 `./scripts/ci-log.sh <run-id>` 拉失败 job 日志。
- 修红单独提交，不叠加新改动。

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

## 6. 关闭 issue（PR 已合并后）

```bash
gh issue view <n> --json number,state,title    # 先确认状态与标题
gh pr view <n> --json state -q '.state'        # 必须输出 MERGED
gh issue close <n> --comment "<关闭评论>"
```

关闭评论模板（交付 / 已知限制 / 提交 / CI / PR / 验证 / 审查）：

```
交付：<1-2 句，对应用户可感知的行为>
已知限制：<无则写"无"；若是后续票范围，注明票号>
提交：<hash...>（<一句话范围>）
CI：<run link> Windows + Linux 绿
PR：#<n>（merged <merge-commit>）
验证：ctest <x>/<y>、preflight 全绿、<E2E/真机验证（如适用）>
审查：<n> 轮双轴收敛 / 剩余 <m> 条 non-blocking（列票）
Refs: <contract/component/docs 路径>
```

关闭后检查下游依赖是否解除：`gh issue view <child> --json blockedBy`（返回 `{nodes:[],totalCount}`，用 `.blockedBy.nodes[]`；或 `gh api repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by` 拿数组），或在票的报告里写清"#X 的 blocked_by 从 N → 0"。

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
- 省略 issue close → 人工追问，下游 `blocked_by` 卡住。
- 共享工作区做并行票 → 提交父节点带上另一条线，push 时一并发布。
- `/tmp` 存跨调用状态 → 下次 bash 调用文件消失（DSH 沙箱每次调用隔离 `/tmp`；临时文件写 `.cache/`）。
