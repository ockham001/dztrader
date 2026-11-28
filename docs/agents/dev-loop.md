# 标准开发循环（mattpocock 技能链路）

`AGENTS.md` §五「标准开发循环」指向本文件。这里放**全序列**；`AGENTS.md` 只保留两个必须当场判断的点（停点、为什么停点会被跳过）和机器守卫指针。

主循环跑在既有硬门禁之上，按改动规模可裁剪。

## 七步

1. **设计定稿** —— `grill-with-docs`：问到无分歧；术语入 `GLOSSARY.md`、决策入 `docs/adr/`（当场）。大而模糊的探索用 `wayfinder` 建决策地图，收敛后并回 `to-spec`。

2. **立 spec** —— `to-spec` 写 GitHub Issue。涉及帧契约的 spec 必须先列契约影响面（哪份契约、哪些帧号），先改契约再改代码；满足 ADR 触发条件的在 spec 注明。

3. **拆票** —— `to-tickets`：tracer-bullet 子 issue + 原生阻塞边。**拆完即停**：本会话到此为止，不要接着实现。逐票的实现各自开新会话跑 `implement`——理由见下面「上下文纪律」。

4. **实现** —— `implement` 单票（默认）或 `implement-spec` 大特性（worktree 内先跑 `setup`）；提交 review 前按 [review-checklist](review-checklist.md) §2 自搜高频 finding。

5. **审查** —— `code-review`：从固定点起，双轴（仓库规范 / spec 一致性），**按宿主派**：opencode 用 `spawn_teammate`（那里 `subagent` 的 id 不可寻址、报告易整份丢失）；DSH 用 `subagent`，靠 `wait_agent` 阻塞或等 `subagent-settled` 通知，报告正常到达（#411–#416 六票实测）。冻结纪律与 verifier prompt 模板见 [ticket-closeout](ticket-closeout.md) §1.4–1.5。

6. **收尾** —— 动契约或有决策：契约、docs 地图、ADR 随同提交（`general.md` §11.3 checklist）；等 CI 绿；波末 `retro`。每票的冻结审查→门禁→push→**PR→合并→关 issue** 见 [ticket-closeout](ticket-closeout.md)。

7. **交接** —— `handoff` 文档写 `.cache/handoff/`（DSH 沙箱 `/tmp` 每次 bash 调用隔离、不可跨会话）。环境断言（二进制新旧、端口、进程、配置位置）必须带取证（命令 / mtime / hash），下游会话据此前先复核。

## 上下文纪律

- 步骤 1–3 保持同一未打断窗口
- 每张票 `implement` 换新上下文
- `retro` 在被复盘会话内、clear 之前跑

## 为什么「拆完即停」会被跳过（#535 复盘）

主循环里五个技能的 `SKILL.md` 都带 `disable-model-invocation: true`：
`grill-with-docs` / `to-spec` / `to-tickets` / `implement` / `retro`。

这意味着**只有用户敲斜杠命令才会加载这些技能**，agent 无法用 `skill` 工具调用它们
（报 `not available for model invocation`）。当用户没敲命令、而 agent 又要"走流程"时，
它面前只有两条路：

1. **停下来告诉用户该敲哪个命令**（正确，但需要 agent 意识到自己不该继续）；
2. **照着 read 出来的 `SKILL.md` 手工硬做**（2026-11-28 #534 就是这样——设计、spec、
   直接改 CSS 全在同一会话里完成，`to-tickets` 那步的「停」被跨过去了）。

外加一个结构性弱点：`to-tickets` 的「拆完即停」原本只是 `AGENTS.md` 里的一句话，
23 步 preflight 没有一步检查流程顺序，于是「先实现后补文档」在机器眼里与
「按流程开发」完全同形。

## 机器守卫（#535）

`scripts/check_flow.py`，两处接入：

- **`.githooks/pre-push`**（真实拦截，会把 `BLOCKED` 置 1）
- **`preflight.sh` 的 `dev flow order (#535)` 一步**（跑 `--selftest`，保证脚本本身没退化）

判据（离线、不查 GitHub）：

- 分支改了**实现类文件**（`.css`/`.vue`/`.ts`/`.cpp`/`.h`/`.py` 等）时，票号必须来自分支名，
  且必须能在 `docs/adr/*.md` 找到引用该票号的 ADR，**或** commit message 带 `[spec]`/`spec #N`；
- 纯文档 / 脚本票（`docs/`、`*.md`、`scripts/`）豁免。

逃生口 `DZ_ALLOW_FLOW=1`，仅真故障时用，用完在报告里写明。

**它挡不住什么**：挡不住「同一会话里没停」——那是会话级事实，git 里看不见。所以
停点仍须 agent 自己守；机器守卫只保证**漏掉的后果**（无 spec/ADR 的实现）推不上去。

## 备选入口

`diagnosing-bugs`（疑难 bug）｜`research`（后台调研成文）｜`prototype`（设计探查，经 `handoff` 双向桥接）｜`triage`（外部 issue 分诊）。

## 维护节奏

技能用 `npx skills@latest update` 更新（新增技能需重跑 `add`）；opencode 版本对比 [changelog](https://opencode.ai/changelog)；`doctor` 子代理体检；clang-format / conan 为固定版本，本地与 CI 保持一致（升级时同一次提交改两边）。
