# 每票收尾（Ticket Close-out）

> 用途：`/implement #N` 实现完成后的**唯一收尾流程**。人工 prompt 只说 `/implement #N` + "收尾见 docs/agents/ticket-closeout.md"，不再粘贴 6 步模板。
> 配套：[development-checklist.md](../development-checklist.md) C 段（门禁权威来源）、[issue-tracker.md](issue-tracker.md)（issue 操作）、`code-review` 技能。

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
2. `/code-review since <base>`，即审 `git diff <base>...HEAD`；**审查期间禁止改工作区**。若必须改，先 commit 再让 verifier 重读新 commit。
3. 双轴并行（Standards + Spec）；**一次只跑一个 verifier**，等它返回再 spawn 下一个。verifier prompt 里把上一轮 findings 以编号列表贴全（"逐条确认/未修说明"），避免重复审查与相反结论。
4. 轮次口径：**1 轮 = 1 次修复 + 1 个 verifier**；最多 3 轮修复。第 3 轮仍有 blocking → 修完后必须再跑 **1 轮只读验证**（不算第 4 轮修复），并把结论写进报告。禁止用改写 prompt 重跑 verifier 来"刷新"结论。
5. **并行波（同一时间多个 ticket）必须一票一 worktree**（AGENTS §五）；共享工作区只留单线。

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

## 4. push 与 CI

```bash
./scripts/check-push-scope.sh <issue>            # @{u}..HEAD 只允许本票 commit
git push                                          # 推送需用户同意（AGENTS 硬门禁）
./scripts/wait-ci.sh --sha "$(git rev-parse HEAD)"   # 或 --run <id> / PR 号
```

- 等待期间不要 `gh run watch` 全量输出、不要 `sleep` 轮询、不要反复 tail 后台日志。
- CI 红：`./scripts/ci-log.sh <run-id>` 拉失败 job 日志，只打印 `error C*` / `FAILED` 等错误行（完整日志落 `.cache/ci/`）。
- 修红单独提交，不叠加新改动；Windows job 约 30–37 分钟，期间可做不冲突的只读工作。

## 5. 关闭 issue

```bash
gh issue view <n> --json number,state,title    # 先确认状态与标题
gh issue close <n> --comment "<关闭评论>"
```

关闭评论模板（交付 / 已知限制 / 提交 / CI / 验证 / 审查）：

```
交付：<1-2 句，对应用户可感知的行为>
已知限制：<无则写"无"；若是后续票范围，注明票号>
提交：<hash...>（<一句话范围>）
CI：<run link> Windows + Linux 绿
验证：ctest <x>/<y>、preflight 全绿、<E2E/真机验证（如适用）>
审查：<n> 轮双轴收敛 / 剩余 <m> 条 non-blocking（列票）
Refs: <contract/component/docs 路径>
```

关闭后检查下游依赖是否解除：`gh issue view <child> --json blockedBy`，或在票的报告里写清"#X 的 blocked_by 从 N → 0"。

## 6. 清理与报告

- `git worktree remove .worktrees/<branch>` + `git branch -d <branch>`；确认 `.worktrees/` 为空（含 `.cache/` 残留）。
- `git status` 应只剩**另一条线**的未提交产物；本票产物全部已提交。
- 报告固定包含：审查轮数与结论、改动文件数、commit hash、CI run 链接与结论、preflight 结果、issue 状态。

## 反模式（本波实际踩过）

- 审未提交工作区 → 审查期间工作区被改（含并行线提交），结论失效，提交后还要 amend。
- 并行 spawn 多个 verifier → 同一分钟给出相反结论，白跑一轮。
- `sleep` / `gh run watch` 等 CI、`gh run view --log-failed` 因 `~/.cache/gh` 只读失败 → 时间与上下文双浪费。
- 省略 issue close → 人工追问，下游 `blocked_by` 卡住。
- 共享工作区做并行票 → 提交父节点带上另一条线，push 时一并发布。
- `/tmp` 存跨调用状态 → 下次 bash 调用文件消失（DSH 沙箱每次调用隔离 `/tmp`；临时文件写 `.cache/`）。
