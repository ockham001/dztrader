# Issue 载体：GitHub

本仓库的 issue、spec 与票都落在 GitHub Issues，全部操作走 `gh` CLI（在仓库克隆内运行时自动推断仓库，也可用 `git remote -v` 核对）。

## 约定操作

- **建 issue**：`gh issue create --title "..." --body "..."`（多行正文用 heredoc）
- **读 issue**：`gh issue view <number> --json number,title,body,labels,comments`
- **列 issue**：`gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'`，配 `--label` / `--state` 过滤
- **子 issue**：`gh issue create --parent <parent> ...`，或事后 `gh issue edit <parent> --add-sub-issue <child>`；旧版 gh 用 `gh api --method POST repos/<owner>/<repo>/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>`（数据库 id）；不支持 sub-issue 时在子 issue 正文顶部写 `Part of #<parent>`
- **评论**：`gh issue comment <number> --body "..."`
- **打/去标签**：`gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **关闭**：`gh issue close <number> --comment "..."`

## PR 作为分诊入口

**PRs as a request surface: no**（本仓库不把外部 PR 当功能请求对待；`triage` 技能读这个标志）。

## 分诊拍板

规则与标签口径见 [triage-labels.md](triage-labels.md)「拍板闭环」（`ready-for-agent` 前置条件在那里）。

- `/implement` 遇 `needs-triage`：先跑决策清单（列出待拍板项 → 问用户 → 把决策写回 issue 正文 → 换标签），不要边实现边等拍板（#203 等人工决策 79 min = 该票 30% 墙钟）。

## 技能说「publish to the issue tracker」时

创建一个 GitHub issue。

## 技能说「fetch the relevant ticket」时

按上面的「读 issue」读取。

## 分诊知识库

被拒绝的功能请求记入仓库根的 `.out-of-scope/`（tracked、**惰性创建**：首次出现被拒请求时建立；case 文件从对应 issue 评论链接过去）。

## Wayfinding 操作（wayfinder）

**地图**是一个 issue、**票**是它的子 issue。

- **地图**：单个 issue，标签 `wayfinder:map`，正文承载 Notes / Decisions-so-far / Fog。`gh issue create --label wayfinder:map`
- **子票**：作为地图的 GitHub sub-issue 关联；标签 `wayfinder:<type>`（`research`/`prototype`/`grilling`/`task`）；被认领后指派给执行者
- **阻塞**：GitHub **原生 issue dependencies**（UI 可见的标准表示）。加边：`gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`，其中 `<blocker-db-id>` 是阻塞者的数字**数据库 id**（`gh api repos/<owner>/<repo>/issues/<n> --jq .id`，不是 `#number` 或 `node_id`）；`issue_dependencies_summary.blocked_by`（仅未关闭的阻塞者）是实时门槛；不可用时回退为子票正文顶部的 `Blocked by: #<n>, #<n>` 行
- **关掉阻塞者不是删除依赖边**：边保留；`blocked_by` 只统计**未关闭**的阻塞者，所以阻塞票一关，子票就自动从 0 → 可开工（别理解成有人手工解除了阻塞）。查询时注意 `gh issue view <n> --json blockedBy` 返回对象 `{nodes:[],totalCount}`（jq 用 `.blockedBy.nodes[].number`），要数组用 `gh api repos/<owner>/<repo>/issues/<n>/dependencies/blocked_by`
- **前沿查询**：列出地图未关闭的子票，剔除有未关闭阻塞者或已有指派者的，按地图顺序取第一个
- **认领**：`gh issue edit <n> --add-assignee @me`
- **了结**：`gh issue comment <n> --body "<答案>"` → `gh issue close <n>` → 把上下文指针追加到地图的 Decisions-so-far

## 台账检查与 spec 进度

- **spec 母票进度**（子票完成度一眼看，避免母票空挂；不硬编码 owner/repo）：
  ```bash
  repo="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
  gh api graphql -f query="{repository(owner:\"${repo%/*}\",name:\"${repo#*/}\"){issue(number:41){state subIssuesSummary{total completed}}}}"
  ```
- **波末 / 收口前台账体检**：`python3 scripts/check_ledger.py --strict` —— 只读报告三类问题：① open `[spec]` 且子票全关（应走 spec 收口）；② 活跃文档里「后续票 / 待实现 / 由 #N 提供 / 待 #N 落地」类承诺指向已关闭 issue（悬空承诺；脚本 `--selftest` 有正则自测）；③ `needs-triage` 创建超过 3 天未处置（warning）。输出可直接贴进 retro / 收口报告。
- **读长 spec body**：`gh issue view <n> --json body --jq .body | sed -n '1,160p'`，再按关键词 `grep -n`；不要整段灌进上下文。

## 版本要求

`gh` ≥ 2.94 才有原生 sub-issue 命令（本机为 2.102，满足）；阻塞边与依赖查询始终走 `gh api`，不受版本限制。
