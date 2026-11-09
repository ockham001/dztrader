# 每票收尾（Ticket Close-out）

> 用途：`/implement #N` 实现完成后的**唯一收尾流程**。人工 prompt 只说 `/implement #N` + "收尾见 docs/agents/ticket-closeout.md"，不再粘贴 6 步模板。
> 配套：[development-checklist.md](../development-checklist.md) C 段（门禁权威来源）、[review-checklist.md](review-checklist.md)（审查清单：AC 逐条 + 6 类高频 finding）、[issue-tracker.md](issue-tracker.md)（issue 操作）、`code-review` / `pr` 技能。

---

## 开工前置（实现前）

**每票流程的第一步**：实现前对照**全部前置票的实际交付**（读前置票 issue 的关闭评论 / 对应 commit），把本票 AC 中**已交付 / 已变更**的项以 **issue 评论**记录（**不删除原文 AC**），实现只做剩余部分。判据：本票 AC 不再重复交付前置票已完成项，后续会话无需逐条比对（反例：**#245** 的 AC 已被 **#244** commit `3d4dd80` 全部交付，issue 却无记录，后续会话重复比对）。

## 0. 确定同步点（不要留占位）

收尾前先看本票实际改了哪些路径，按下表机械推导要同步的文档；涉及帧/WS/REST 的**先改契约再改代码**（[general.md](../frame_contracts/general.md) §11.3）：

| 改动路径 | 必须同步 |
|---|---|
| `docs/frame_contracts/*.md` | 契约本身 + platform 头文件 + 帧号登记 + dzweb 领域服务 + 前端 + 测试（general §11.3 六项） |
| `docs/components/<topic>.md` | 组件文档与代码/契约一致 |
| 改进程状态机/失败语义/生命周期/线程模型 | `docs/components/<进程>.md` 同步（进程内行为以组件文档为准；契约只承载跨进程可观察语义） |
| 新增/改动公开行为 | `README.md`、`docs/README.md` 地图（按需） |
| 新术语 / 新决策 | `GLOSSARY.md` / `docs/adr/NNNN-*.md`（ADR 触发条件见 AGENTS §三） |
| 空实现 / 已知限制 | issue 正文或组件文档显式标注，**后续票范围必须引用票号；不跟踪的写明不跟踪理由**；不留在代码注释里 |

## 1. 冻结提交与审查

**审查对象是 commit，不是活动工作区。**

1. `git add <本票路径>`（显式路径，禁止 `git add -A`）→ `git commit`（WIP 提交也可）→ **`git status` 必须干净**，并把 `git rev-parse --short HEAD` 写进 verifier prompt。
2. `/code-review since <base>`，即审 `git diff <base>...HEAD`。
3. **冻结纪律**：从 spawn verifier 到最后一个 verifier 返回，**不改任何文件、不 amend、不 push**。审查期间继续改代码会让 findings 对旧 commit 作废，整轮白跑（本波 #93：HEAD 在审查中前移 4 次，审阅者只能对旧 commit 出结论并重跑；#164 的 Standards verifier 自己声明「working tree moved during review」，findings 只能对旧快照负责）。
4. **verifier 一律 `spawn_teammate`，不用 `subagent`**：`code-review` 技能写的是「并行 sub-agent」，但本宿主的 `subagent` id **不可寻址**——`send_message` 报 `TEAM_MEMBER_NOT_FOUND`、`wait_agent` 报 `no-active-peer`，跑完报告也容易整份丢失（#165 两个 review 轴的结果全丢，只能重派一轮；本波四张票共 15 次此类失败）。用 `spawn_teammate` 派 Standards / Spec 两个变体，各自 `wait_agent` 或等报告；两条到齐再动代码。
5. **verifier prompt 用模板，不要临场写**：**成本封套与 prompt 骨架见 [verifier-prompt.md](verifier-prompt.md)**（Standards / Spec / 只读验证三个可复制块 + 为什么必须写步骤序）。带齐固定点与 `HEAD`、`git diff <base>...HEAD` 命令、上一轮 findings（贴全，避免重复审查与相反结论）、只读约束。**先复制模板再填占位符**——只写「预算 ≤N 分钟」是已知失效形态（#216 首轮两个 verifier 19.4 / 14.4 min 零产出；换步骤序后 4.8 / 0.7 min 交付）。
6. **一个会话总共只能派 8 个 teammate**（`spawn_teammate` 第 9 个报 `Team member limit 8 reached`，本宿主硬上限）。**这是会话生命周期内的累计上限，不是并发上限**：成员跑完变成 `inactive` 后**仍占着名额**，`interrupt_agent` 也不释放，且没有工具能把成员移出花名册——#233 派满 8 个后第 9 个被拒；#237 同样（8 个历史成员全 inactive，第 9 个仍被拒）。**所以名额是稀缺资源，派之前先规划**：一次审查只派必要的那几个（Standards + Spec，验证轮能复用上下文就不新开轴）；机械工单（如 #233 的 97 条 lint）别按文件数拆——按文件群拆到 3–5 个就给验证轮留够余量。名额耗尽后只能 Lead 自己代轴（见 §1 第 10 条的 failed 恢复协议第 ③ 步）。

7. **票内多 teammate 并行必须划写作用域**：AGENTS 的「一票一 worktree」只覆盖**跨票**。票内并行时，同仓多成员同时改同一批文件会互相踩——#206/#233 共 7 次 `file changed since it was read`（`Modal.vue` / `useFocusTrap.ts` / `App.vue` 被多个成员碰过）。规则：**一个成员一个文件集，共享文件归 Lead**；用 `team_task_create` 的 `write_scopes` 字段登记该成员要改的路径前缀，派活时写进 prompt。审阅/验证类成员只读，不占作用域。**注意 `write_scopes` 是建议性声明、不是互斥锁**——它不会阻止两个成员写同一文件，纪律靠 prompt 与分工，不靠工具强制。**分工表（成员 / 写作用域 / 依赖 / 验收映射）必须先在 issue 评论落盘再 spawn 成员**——`.cache/handoff/` 可随时删除，不作为唯一载体；本仓以 issue 为唯一记忆。分工表与 `team_task_create` 的 `write_scopes` 登记同步维护，别只留在会话或 handoff 文档里。

8. 轮次口径：**1 轮 = 1 次修复 + 1 个 verifier**；最多 3 轮修复。第 3 轮仍有 blocking → 修完后必须再跑 **1 轮只读验证**（不算第 4 轮修复），并把结论写进报告。禁止用改写 prompt 重跑 verifier 来"刷新"结论。**只修 NON-BLOCKING 的轮次不重开验证**，见第 13 条。
9. **等 verifier 不要 `sleep` 轮询**（AGENTS §五「等待一律后台作业」）：`spawn_teammate` 的成员用 `wait_agent` 真正阻塞；派 `subagent` 时结果以 `subagent-settled` 通知到达。等待窗口做**不写文件**的工作（读 diff/契约、拟关闭评论与报告、跑幂等 `preflight --c-only`）。反例：#77/#78/#81 因 sleep 等 verifier 累计 ≈3.9h。**Lead 侧动作点**：verifier **无进展**（长时间既无工具调用、也无进度 interim）时主动 `send_message` 一条进度询问「**先发结论**（哪怕只发已完成部分），现在到哪一步了」，不要让审查无上限拖住关键路径——#253 两轴跑了 38.7min / 45.3min，T+26 / T+44min 一催，**1.5min 内即交付**，说明卡住多半是缺一个交付信号而不是缺时间。**不要以时长或工具调用数为触发条件**（#309 AC2 已删掉时间盒：107 个历史子会话实测首轮 median 0.42×N、超支 9%、nudge 命中率 3%，时间盒既拦不住长尾又让 verifier 提前分心）。
10. **verifier / 实现 teammate `failed` ≠ 无产出**（#289 AC8）：`failed` 只表示该 turn 异常退出、没有报告，**不代表它没写文件**。恢复顺序：① 先查产出——`git status --porcelain`（它可能已落盘部分改动）、`team_task_list`（共享任务状态与 `write_scopes`）、其 session 日志（`~/.dsh/sessions/`，见 [retro-evidence.md](retro-evidence.md)）；② 成员仍可寻址（`list_agents` 里 `inactive` 而非名额释放）时用 `send_message` **续跑**，prompt 里写「上一步无产出/部分产出 + 先落最小骨架」——不要重派新成员（名额是稀缺资源，见第 6 条）；③ 原 prompt 重跑一次；④ 仍失败 → 改用 `spawn_teammate` 派 verifier（Team 成员可 `wait_agent` 真正阻塞）；⑤ 仍不可用 → **人工代轴**逐条核对（机器门禁须已全绿），并在报告写明"verifier failed N 次、人工代轴"。#123 两个审查线程丢失、用户被迫介入、Lead 人工补轴即反例；禁止改写 prompt 反复重跑刷结论。**实现类成员的 prompt 必须带「T+15min 先落最小骨架」**（#253 recon 37–39min 才写第一行，模板见 [verifier-prompt.md](verifier-prompt.md) §4）。
11. **review 全部收敛（含最后一轮只读验证）后才进入 §4 push**；push 之后只允许修 CI 红。只读验证轮提出的 finding 必须**并入同一个 commit** 再推——先推再验会让验证轮 findings 又产生新 commit，上一轮 CI 作废重跑（旧守卫下第二次 push 还会被「CI 在跑」拒绝、白等一轮；#248 实测 986s；新口径见 §4）。**#252 实测（规则存在 ≠ 动作点看得见）**：23:15 建 PR 起 CI → 23:23:08 修复 commit → 23:23:30 push 被旧 run 拒绝 → 等旧 CI 1431s（到 23:47:57）→ 23:48:07 才推上去 → 新 run 31m42s → 00:20:44 才等到结果，**一次修复被放大成两个串行 CI 周期（≈56min）**，并在 00:20:28 触发用户 abort turn。因此 §4 把「最后一轮只读验证已返回且无 BLOCKING」写成 push / 建 PR 的**动作点前置条件**；只读验证轮模板见 [verifier-prompt.md](verifier-prompt.md) §3（模板返回后才允许进入 §4）。
12. 并行波（同一时间多个 ticket）必须一票一 worktree（AGENTS §五）；共享工作区只留单线。
13. **审查配比：`docs/`-only 的小票不该派满员额**（#316 AC1）。本波 #303 的 diff 是 **+74/−7（81 行，纯 `docs/`）**，按 [verifier-prompt.md](verifier-prompt.md) 的档位表是 **small = 单块单轴**，却派了 **5 个 `spawn_teammate`**，review 阶段占 **8,865s = 该票 turn 的 59%**（Lead 侧 `wait_agent` 17 次共 5,016s），结论是 **0 BLOCKING / 4 条 NON-BLOCKING**。结构性放大器：每多一个修复 commit，两个 marker 的 sha 就被打旧，`scripts/push-ticket.sh`（`--strict --expect-axis standards --expect-axis spec`，要求标记**停在 HEAD**）随即拒绝推送 → **修一次就必须再验一次**，81 行文档票被动跑成 5 个 teammate（该会话已用掉 8 个额度中的 5 个）。三条规则：
    - **`docs/`-only 且 `git diff --stat <base>...HEAD` 总增删 < 200 行 → 至多 2 个 teammate**（Standards + Spec 各一个），**不再按文件群拆分**；large 档的拆块规则（[verifier-prompt.md](verifier-prompt.md) 档位表）只对含代码/契约/前端的改动生效。
    - **只修 NON-BLOCKING 时由 Lead 自查，不派新 teammate**：Lead 自己跑 `git diff <fixbase>..HEAD` 逐条核对，然后**自己**执行 `python3 scripts/review_marker.py write <issue> <axis>`（标记的语义本就是「验证停在哪个 sha」，Lead 自查同样算一次验证，`push-ticket.sh` 只看标记、不看谁写的）。只有修了 **BLOCKING** 才派新 teammate（那是改结论，需要独立轴）。
    - **写文档类交付物不派 teammate**：Lead 已有取证时自己写一次；确需分工时要求 teammate 直接 `gh issue comment` 落 issue，**不要把文件写进 `.cache/handoff` 再被 Lead 重写一遍**（第 7 条已写明 `.cache/handoff/` 不作为唯一载体；#303 的 `spec-auditor` 交 141 行清单、Lead 整份重写为 188 行，该段墙钟归零）。
14. **NON-BLOCKING 与「修复-验证」放大链**（#316 AC1 的结构性部分）：`push-ticket.sh` 要求两个轴的 marker 停在 HEAD，所以**任何**新增 commit 都会让上一轮标记失效。因此把 NON-BLOCKING 修复合进**下一次本来就要产生的 commit**（例如 CI 红修复），或按第 13 条由 Lead 自查后重写标记；**不要**为 1–2 条措辞 finding 单独产生一个 commit + 一轮验证。

## 2. 门禁（全绿才算完）

```bash
./scripts/gate.sh build --config release
./scripts/gate.sh test --config release
./scripts/gate.sh preflight
```

`gate.sh` 是 agent 侧的统一入口：同一 build tree 用 `flock` 串行（并行子代理不会互相踩 ninja）、自动导出 ccache 环境、输出落 `.cache/gate/`、返回**真实退出码**。人工可直接跑 `build.sh` / `test.sh` / `preflight.sh` 等价命令。**要短输出用 `./scripts/gate.sh build|test|preflight --tail N`**（成功留 N 行、失败留 max(N,30) 行）。给门禁命令接 `| tail` / `| head` 只会丢退出码（#75 的 wait-ci 超时、#81 的 ninja 失败曾因此被记成假绿），但 `gate.sh` 的 verdict 行固定打在输出**末尾**、结果同时原子写入分命令槽 `.cache/gate/last-status-<cmd>`（`build` / `test` / `preflight` / `ledger`，`#316 AC2`；兼容视图 `last-status` = 最近一次任意门禁），所以**结论仍可见**：判绿看 `gate: OK` 行或对应槽位，不要只看 shell 退出码（#249–#251 实测 83 次接管道、0 次假绿）。输出长度来自失败摘录 max(N,30)，与是否接管道无关——**不要**靠调小 `--tail` 默认值解决长度问题。**`wait-ci.sh` 例外**：它本就静默、退出码就是 CI 结论、没有 verdict 行兜底，别接管道。

票内并行期间只跑定向作业即可（不必每轮全量，收尾仍须跑上面的全量门禁）：

```bash
./scripts/gate.sh build --tail 5 --target <target>
./scripts/gate.sh test --tail 8 --test-name <regex>
```

`gate.sh` 只解析自己的 `--tail N`，其余参数原样透传：`build.sh` 支持 `-t|--target`（→ `cmake --build --target`）、`test.sh` 支持 `-t|--test-name <regex>`（→ `ctest -R`）与 `--list`（→ `ctest -N`，只读列出用例名，不取构建锁）。**票内并行期间用 `--target` / `--test-name` 迭代；所有写作用域完成后才跑全量 build/test**——build 树中间态（他人在途文件未收敛）下的全量构建会因别人的半成品而红，纯浪费（#243/#244/#245 门禁墙钟 13–23 min，其中多轮全量 build 落在写作用域未完成期间）。`--target` / `--test-name` 都**可重复**，重复值累积后一次透传（#281；修复前是覆盖式赋值，只留最后一个且退出码 0，造成门禁假绿）。**唯一例外是 `--list`**：它直通 `ctest -N`，不落 gate 日志、不写 `last-status`（它不是门禁判定）。

定向迭代的两个高频踩坑（#249–#251 合计浪费 10 次 gate 调用）：

- **`--test-name` 的 regex 匹配 `<Fixture>.<Case>`（gtest 用例全名），不是源文件名**：`--test-name td_config_test` 匹配不到任何用例，`ctest -R` 零匹配退出码 **8**（`No tests were found!!!`）——那**不是**测试失败，与「用例红了」要分开判断。查用例名用 `./scripts/gate.sh test --list`（直通 `ctest -N`，一行一个用例名，一次调用查全量）。
- **`--target` 的命名规则 = 模块前缀 + 测试源文件名去 `.cpp`（保留 `_test`）**：`apps/webui/tests/<name>.cpp` → `dzweb_<name>`、`apps/ctp/tests/td_<name>.cpp` → `dztd_ctp_td_<name>`、`apps/master/tests/<name>.cpp` → `dztraderd_<name>`、`libs/<mod>/tests/<name>.cpp` → `dz<mod>_<name>`；冒烟测试是例外 `dz_smoke_test`（#251 曾按源文件名猜 target、连错 3 次：前缀不是 `dztd_` 而是 `dztd_ctp_`）。

`./scripts/preflight.sh` 的**检查清单只有一个真相源：脚本自身的 `run_step` 行**（`./scripts/preflight.sh --help` 直接打印全部步骤名与数量；#289 AC7——此前 AGENTS / 本文 / development-checklist 三处手抄，`AGENTS.md` 漏了「gtest 发现超时」让 #284 终轮验证判 FAIL，白跑 229s + 作废 150s 机时）。每项检查的细节（为什么需要 / 规则 / `--selftest`）写在各检查脚本的文件头。任一项红就停，**先修红，不叠加新改动**。CI linux job 复用同一脚本的 C 段（`--c-only`）。

> **门禁结果可追溯（#190 / #316 AC2）**：`gate.sh` 的最终 `gate: OK|FAILED` 行固定打在输出**末尾**（`| tail -2` 也可见），每次结果原子写入**分命令槽** `.cache/gate/last-status-<cmd>`（`build` / `test` / `preflight` / `ledger`），并同步写兼容视图 `last-status`（= 最近一次任意门禁，只用于提示）。`.githooks/pre-push` 调 `scripts/gate_status.py check` 按**票型**放行：代码票要求 `build` 与 `test` 都停在当前 HEAD 且 rc=0，纯文档/脚本票要求 `preflight`，**`ledger` 永不参与判定**（旧实现所有子命令写同一个槽且 hook 不看 `cmd`，于是「build 红 → 跑 ledger → 放行」成立，本波 #303 push 后跑 2 次 ledger 实测覆盖掉 preflight 记录）。记录缺失/HEAD 不符只警告；当前 HEAD 上失败才拦截；应急逃生 `DZ_ALLOW_RED_GATE=1`。

> preflight 是**只读**门禁：它不得改动工作区任何受版本控制的文件。新增检查若需要"注入已知错误形态"来自测，必须在临时副本上做（参照 `scripts/check_json_vendor_version.py`），否则一次 Ctrl-C / SIGKILL 就会把工作区留在改坏的状态——而审查冻结窗口恰好禁止这种改动。

## 3. 提交（按路径）

- 只 `git add` 本票路径；`git status` 核对，**严禁捎带另一条线的产物**（本波踩过：提交父节点落在另一条线未 push 的 commit 上）。
- 提交信息中文，前缀 `fix:` / `feat:` / `refactor:` / `docs:` / `test:` / `chore:` / `perf:` / `style:` / `build:` / `ci:`。
- **票号写进作用域**：`fix(#246): ...`，不要 `fix: #246 ...`。后者是 GitHub closing keyword 形态，PR 合并即提前关单、关闭评论被静默丢弃（§6）。`scripts/check_commit_subject.py` 在 pre-commit / CI / preflight 三处强制。
- 契约、组件文档、ADR、GLOSSARY、docs 地图随同提交（第三节收敛规则 4）。

## 4. 推送与 PR

> **push / 建 PR 的硬前置（#284 AC2）**：先确认**最后一轮只读验证已返回且无 BLOCKING**。未返回时先 `wait_agent` 阻塞等待（`subagent` 等 `subagent-settled` 通知），**不得先 push / 建 PR 再验**——#252 因此把一次修复放大成两个串行 CI 周期（≈56min，时间线见 §1 第 11 条）。只读验证轮模板见 [verifier-prompt.md](verifier-prompt.md) §3。

```bash
./scripts/push-ticket.sh <issue> -u origin <branch>   # scope + 审查验证标记 + git push 串行（#309 AC6）
gh pr create --base main --head <branch> --title "<type>: <摘要>（#<issue>）" --body-file <body 文件>
./scripts/wait-ci.sh --pr <n>             # 静默等 PR checks（后台作业启动，见下）
```

- **push 用 `scripts/push-ticket.sh <issue> [-u origin <branch>]`，不要裸 `git push`**（#309 AC6）：它串行做三件事——`./scripts/check-push-scope.sh <issue>` → `python3 scripts/review_marker.py check <issue>` **严格判定**（该票任一轴无标记 / 标记 sha 与 HEAD 不一致 → **默认拒绝**，exit 3）→ `git push "$@"`。逃生口 `DZ_ALLOW_UNREVIEWED_PUSH=1`：拒绝时先打印两个轴缺失/不匹配的标记与当前 HEAD，再放行。**worktree / 非标准分支**或需要推多个 ref 的场合才退回裸 `git push`（并手工跑前两条命令确认）。背景：#297 是本波唯一违例——`git push` 距首个 `spawn_teammate` 仅 19 s，`pre-push` 的 warning 被无视，随后 `--force-with-lease` 产生第二个 CI run（19 min）。

- 本仓用 PR 落地（`ci.yml` 只在 PR / main 触发）；main 有 GitHub ruleset `main-protection`（require PR、禁非快进、禁删除、`bypass_actors=[]`，admins 也无绕过）。payload 见 `.github/rulesets/main-protection.json`，查看/重建：`gh api repos/<owner>/<repo>/rulesets`、`gh api --method POST repos/<owner>/<repo>/rulesets --input .github/rulesets/main-protection.json`；仓库同时开启 `delete_branch_on_merge=true`。应急直推 main 需临时禁用 ruleset（`DZ_ALLOW_MAIN_PUSH=1` 只绕过本地 hook）。
- **push 分级**（[ADR 0036](../adr/0036-push-authorization-tiered-with-guardrails.md)，AGENTS 硬门禁的细化）：feature 分支 push / PR 创建更新免逐次确认，前置是本票门禁绿 + `check-push-scope` 通过 + 一轮修复一次 push；**合并 PR 到 main 仍需确认**（可按票或按波一次授权，且必须在开工/PR 创建时一次性提出——见 §5）；删除非本票分支、打 tag 逐次确认；**force push 禁止**（`pre-push` 非快进拦截 + `.opencode` deny）。本地 `.githooks/pre-push`、main ruleset 与 `.opencode` 权限是机器守卫。
- **认证失败时先换通道，绝不往 URL 里塞凭据**（#265 复盘 #246）：SSH agent 会间歇性失效——同一个会话里 `Permission denied (publickey)` 可出现多次，而紧接着 `ssh -T git@github.com` 又是通的。此时正确做法是走 HTTPS：`setup.sh` 已装好 gh credential helper（全局不可写时回退仓库级），直接 `git push https://github.com/<owner>/<repo>.git HEAD:refs/heads/<branch>` 即可。**禁止** `git push https://user:<token>@github.com/...`：`-u` 会把 token 连同 upstream 一起写进 `.git/config`（#246 实际发生，事后需人工清理，且有泄漏风险）。若已误写：`git config branch.<branch>.remote origin` 覆盖回去，再用 `grep -rq "$(gh auth token)" .git` 确认无残留。
- **实现完成即自动推进到本节，无需等用户口令**：门禁绿后直接 push → 建 PR → 起 CI 等待；#159 曾在「实现完成」处停下，需要用户催「进行收尾，可以推送」。唯一需确认的是 §5 合并 main、打 tag、删除非本票分支（ADR 0036）。
- **PR 开完不阻塞等 CI**：`gh pr create` 成功后即可转下一票（读契约 / 定 spec）或本票收尾准备（拟关闭评论、清理 worktree 计划），CI 结果在自然断点或波末统一用 `./scripts/wait-ci.sh --pr <n>` 收；不要把 CI 等待写成同步步骤（本波 `wait-ci` 阻塞约 3.4h）。**边界**：不阻塞只适用于同波内的下一步准备；本票**合并前必须 CI 绿**，开新波前必须已完成合并（见 §5）。
- **`wait-ci.sh` 的退出码就是 CI 结论**：它本来就是静默输出，只打状态变化与失败链接，别接管道（理由见 §2）。feature 分支没开 PR 时 `--sha` 约 180s 就报错并提示先建 PR，不会空等到 60 分钟总超时。
- **pre-push 拦截「当前 HEAD 上失败的门禁」**（#316 AC2 起按票型判定，实现在 `scripts/gate_status.py`）：代码票读 `.cache/gate/last-status-build` 与 `-test`，纯文档/脚本票读 `-preflight`，`ledger` 不参与；槽位缺失或 HEAD 不符只警告；被拦时先重跑被点名的那条 `./scripts/gate.sh build|test|preflight`，应急 `DZ_ALLOW_RED_GATE=1`。
- **修复性提交立刻推，不要再等旧 run（#284 AC1）**：PR run 的 `concurrency.cancel-in-progress`（`ci.yml`）会取消旧 run，新 run 覆盖全量内容；`check-push-scope.sh` 现在按 in-progress run 的 `headSha` 判定——旧 run 的 SHA 是被推 HEAD 的祖先（已被新 commit 取代）→ **放行 + supersede warning**；只有同 SHA（没有新内容可推）才 exit 3 拦截，不可本地解析的 SHA 保守拦截。**唯一仍要攒批的情形**：把同一次修复拆成多个 commit 反复推（每个 commit 都会取消并重起一个 ≈35–40 分钟的 run）——修复请合并成 **1 个 commit** 一次推（本波 #93 曾因逐条推取消 3 个 run）。`PUSH_SCOPE_ALLOW_CI_BUSY=1` 仍是逃生口，仅真故障时用。
- 等待期间不要 `gh run watch` 全量输出、不要 `sleep` 轮询、不要反复 tail 后台日志。CI 红先用 `./scripts/ci-log.sh <run-id>` 定位：它**优先取 `last-test-log` artifact（约 200KB，秒级给出失败用例名）**，再退回失败 job 日志（Windows 可达数十 MB）；同一 run/job 已下载过会复用（`--refresh` 强制重拉，`--context N` 调上下文行数）。
- **等 CI 用后台作业**：`wait-ci.sh --pr <n>` 以 `run_in_background` 启动，等待期做只读准备（拟关闭评论 / 清理 worktree 计划 / 读下一票契约）；需要结果时 `job_output(wait=true)`，不要前台空等（2026-10-26 #146 / #147 各阻塞 21–22 分钟）。
- **单次快照（#192）**：任意时刻想看 CI 现在到哪了，用 `./scripts/wait-ci.sh --status [--pr <n>|--sha <sha>|--run <id>|--branch <b>]`——立即返回，0 绿 / 1 红（打印失败 job）/ 2 无 run 或错误 / 3 仍在跑。**不要** `sleep` 轮询（#159/#160 曾各 sleep 6 次、共约 13 分钟）：把它当「随时可查的仪表盘」，等待本身交给后台作业。
- **不要自建后台进程**：DSH 会在一次工具调用结束时回收直接 fork 的孙进程（实测 `setsid` 子进程也活不过调用边界），所以 `wait-ci.sh` 不提供 `--detach`；后台等待一律用宿主的 `run_in_background`。
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
- **合并授权：唯一默认形态是「开票/批准时预授权」（#309 AC1 修订 #289 AC2 / #284 AC3）**：用户在场时秒级答复；把**条件式授权**——「CI 绿 + 双轴审查通过即合并关单」，或「按票/按波一次授权」——写进 issue 正文的 `## 拍板记录` 段，实现会话据此**零提问**（#289 即此形态）。**「PR 创建后提问」是兜底，只用于开票时用户不在场**（提问窗口与 `wait-ci` 重叠，答案到达时 CI 还在跑，零关键路径代价）。开工时提问是**唯一没有并行工作可垫**的时点，`ask_user_question` 会阻塞整个回合：#284 实测提问 07:40:54 → 答复 08:04:32 = **1418s（26.5% 墙钟）**，其中 933s 纯 idle。「开工时问」仅限「预计本票无 CI / 无并行工作可垫」（例如纯文档票且下游已全部就绪）。**禁止把该问题留到 CI 绿之后才问**——#281 因此在 CI 全绿后才问，用户 6177s 后答复，等待占会话 49.6%（#252 同一问题在用户在场时 23s 内答复）。提问必须保留「**先不合并，我要自己看 PR**」选项；用户选此项时 issue 保持 open、不阻塞其它票（其它票照常开工）。**缺「拍板记录」会被机器报出**：`scripts/check_ledger.py` 规则 R4 对 open 且带 `ready-for-agent` 的票报警（`--strict` 升 error）——#294–#298 五票全部无该段、整波退回「PR 后提问」，阻塞合计 403 min = 43% 墙钟，纯文档约束已两次失效。
- `--delete-branch` 在 worktree 里会跳过本地删除（分支被 checkout）：先 `git worktree remove`，再 `git branch -d`；远端分支用 `git ls-remote --heads origin <branch>` 确认为空。
- 合并会触发 main 的 CI（merge commit 跑 Linux，约 15 分钟；Windows job 只在 PR 跑）；共享工作区切回 main 后不要停在旧 feature 分支。
- **默认不等 main CI**：squash 合并后 main 上的 tree 与 PR head tree **完全相同**，main run 重测的是同一份内容，等它拿不到任何新信息。落地确认用秒级命令：`git merge-base --is-ancestor <merge-commit> main`；想直接验 tree 相同用 `git diff --quiet <pr-head-commit> <merge-commit>`（静默即相同）。实测代价：五票里四票选择等，合计约 113 分钟（#246 4286s / #260 1633s / #248 620s / #258 251s）。
- 只有下列情况才抽查 main run：本次 PR 的 CI 曾红过、或改动了 `ci.yml` / ruleset / 构建缓存配置（这些是「main 与 PR 环境不同」的唯一现实来源）。真要等就留到波末批量：`./scripts/wait-ci.sh --branch main`。

## 6. 关闭 issue（PR 已合并后）

```bash
gh issue view <n> --json number,state,title    # 先确认状态与标题
gh pr view <n> --json state -q '.state'        # 必须输出 MERGED
gh issue comment <n> --body-file <关闭评论文件>  # 先发评论（幂等）
gh issue close <n> --reason completed          # 再关单
```

**必须两步、且顺序是「先评论后关闭」**（#265）：`fix: #246 xxx` 这种提交标题会被 GitHub
当 closing keyword 解析，PR 一合并 issue 就**已经**关掉了——此时 `gh issue close <n>
--comment "..."` 只输出 `! Issue ... is already closed`，**评论从未发出**（#246/#260/#243
实测；#260 的补充评论开头还写着「本票被 GitHub 按 `fix:` 提交关键字自动关闭」）。
拆成两步后，无论 issue 是否已被 auto-close，评论都会落地。

治本措施同时生效：提交标题把票号写进**作用域**，即 `fix(#246): ...` 而非 `fix: #246 ...`。
GitHub 的 closing keyword 要求关键字后紧跟空白/冒号再接 `#N`，括号形态不匹配；而
`check-push-scope.sh` 的正则 `#<issue>([^0-9]|$)` 照样命中。该形态由
`scripts/check_commit_subject.py` 强制（pre-commit + 三个 workflow + preflight），
**不要**用 `--no-verify` 绕过。

**PR 正文与提交标题是同一个 auto-close 面**（#316 AC4）：正文里写 `Closes #N` / `Fixes #N` /
`Resolves #N` 同样会在合并瞬间关单、让本节的关闭评论被静默丢弃（#313 `Closes #301`、
#314 `Closes #302`、#315 `Closes #303` 三个 PR 都犯了，#301 的关闭评论因此事后补发）。
正文守卫由 `.github/workflows/pr-body.yml` 强制（调 `scripts/check_commit_subject.py
--pr-body-file`，与 R3 共用同一个 closing-keyword 正则）；本地可用
`python3 scripts/check_commit_subject.py --pr-body-file <body 文件>` 预检。**两处都要避免**：
正文写 `Refs #N`，或干脆不写票号（PR 标题已带 `（#<issue>）`，`check-push-scope.sh` 看的是
commit subject）。

关闭评论模板（交付 / 已知限制 / 提交 / CI / PR / 验证 / 审查）：

```
交付：<1-2 句，对应用户可感知的行为>
已知限制：<无则写"无"；后续票范围注明票号；不跟踪的写明不跟踪理由>
提交：<hash...>（<一句话范围>）
CI：<run link> Windows + Linux 绿
PR：#<n>（merged <merge-commit>）
验证：ctest <x>/<y>、preflight 全绿、<E2E/真机验证（如适用）>
审查：<n> 轮双轴收敛 / 剩余 <m> 条 non-blocking（列票）/ 验证标记 <issue>-standards@<sha7>、<issue>-spec@<sha7>
Refs: <contract/component/docs 路径>
```

> **「验证标记」必填（#289 AC6）**：只读验证轮交付时由 verifier 执行
> `python3 scripts/review_marker.py write <issue> <axis>`，标记文件 `.cache/review/<issue>-<axis>.done`
> 记录被验证的 sha。关单报告必须列出这两条标记及其 sha——它是「这次合并前真的跑过验证」的
> 唯一物证（#253 的验证轮被宿主 turn 中止，合并时该轮从未发生，报告里却看不出来）。
> 取法：`cat .cache/review/<issue>-*.done`；轴没跑就别编（写「未跑 + 理由」，不要留空）。

关闭后检查下游依赖是否解除：`gh issue view <child> --json blockedBy`（返回 `{nodes:[],totalCount}`，用 `.blockedBy.nodes[]`；或 `gh api repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by` 拿数组），或在票的报告里写清"#X 的 blocked_by 从 N → 0"。

**关完这张票再跑一次** `./scripts/gate.sh ledger`：若它以「spec 母票可收口」点名某张 `[spec]`，说明刚关的这张就是最后一张子票 → 立刻按 §6「spec 母票收口」处置母票，**不要留到下一波**（#41、#153 都是这样空挂的）。

> **口径澄清（#289 AC8）**：子票正文里常见的「本票不关闭母票 / 母票另行收口」**只约束子票自己的动作**，不改变上面这条流水线——刚关的子票如果就是最后一张，母票仍要按台账指引处理。反过来也别把它读成「母票不能由本会话关」：#253 为这句读了 8 段正文、花 4–5min 推理还提了一次人工问题，最终结论就是本条。**判断依据只有台账点名**，不看子票正文怎么写。

### spec 母票收口（子票全部关闭后）

实现票逐张关闭不等于 spec 收口。`[spec]` 母票必须在子票全部关闭后单独处置：

0. **先跑台账体检**：`./scripts/gate.sh ledger`（默认 `--strict`，error 级即退出 1），按报告清掉悬空承诺（docs 里指向已关闭票的「后续票」）与过期的 `needs-triage`，再开始收口。
1. **清点差异**：逐条核对 spec 的用户故事 / 实现决策，分成三类——已交付（给出票号）、已被后续决策推翻或收窄（引用 ADR / spec / PR，并在母票留评论）、不跟踪（写明理由，遵守 §0「不跟踪的写明不跟踪理由」）。
2. **写收口评论**：沿用 §6 模板（交付 / 已知限制 / 提交 / CI / PR / 验证 / 审查），另加「spec 逐条符合性」与差异清单；参考 #120 的关闭评论。
3. **关闭母票**：`gh issue close <spec> --comment "<收口评论>"`。母票不作为长期真相源留在 backlog——真相源是契约 / ADR / 文档；遗留的 `ready-for-agent` 等标签会让后续 agent 把已完成的 spec 当成待实现任务。
4. **先有票再关**：仍想做的事必须先有票（或明确的 out-of-scope 记录），否则不允许关闭母票。

> **什么时候该想起这一节**：每次关闭一张实现票之前跑 `./scripts/gate.sh ledger` —— 它会直接点名「哪张 `[spec]` 的子票已经全关、母票还开着」。不要靠记忆判断「这是不是最后一张子票」。
>
> 动机：#41（dzsysmon spec）的 13 张子票于 2026-10-24 全部关闭，母票却一直开放到 2026-10-26 才被发现；#153（BarStrategy v1）的 12 张子票于 2026-10-30 全关，母票同样还开着——同一形态第二次发生（issue #213）。本流程原先只覆盖「单票 → 关本票」（mattpocock 原版 `implement-spec` 靠 integration branch 的 PR 关闭 spec，本仓改成一票一 PR 后该机制丢失）。

## 7. 清理、交还与报告

- `git worktree remove .agents/worktrees/<branch>` + `git branch -d <branch>`（worktree 实际产物路径见 AGENTS §五；历史路径 `.worktrees/` 若存在也要清）；确认 `.agents/worktrees/` 与 `.cache/` 无本票残留。
- 远端分支：`git push origin --delete <branch>`（`gh pr merge --delete-branch` 已删则跳过），`git ls-remote --heads origin <branch>` 确认为空。
- **UI / 进程实测起栈后立即停栈**：只要本票起过 dev 栈（`./scripts/run-dev.sh`，含 `--no-build`），收尾必须执行 `./scripts/run-dev.sh --stop` 并确认无遗留 dev 栈进程（`pgrep -f' dztraderd|dzweb' ` 之类）。#302 的 digest 里最长作业是 **11,992s（3.3h）** 的 `scripts/run-dev.sh --no-build`，比该票真实 CI（1,374s）大一个量级——报告里的「长作业」若不分归属就会把实测成本记成 wait 成本（#316 AC7）。
- **共享工作区交还**：`git switch main && git pull --ff-only`；`git status` 只允许出现**已登记的并行线**产物（没有并行线时应完全干净）。不把工作区留在 feature 分支就结束会话——会直接阻塞下一条线（本波 #91→#92：共享工作区留在 feat 分支，另一会话卡 15 分钟 + 人工介入）。
- 报告固定包含：审查轮数与结论、改动文件数、commit hash、CI run 链接与结论、PR 链接与 merge commit、preflight 结果、issue 状态、工作区/分支状态；**引用长作业时指名归属（起栈 / 门禁 / CI），>1h 的作业说明它为什么长**（#316 AC7）。

## 8. 本机沙箱（DSH）环境约定

AGENTS §五「本机沙箱」的细节都放在这里（AGENTS 只留指针，避免常驻上下文重复）：

- **等待与门禁**：长命令（门禁 / CI / 子代理）用**后台作业**启动（`run_in_background`），需要结果时 `job_output(wait=true)`；等待期做只读准备，不要 `sleep` 轮询或前台空等（AGENTS §五「等待一律后台作业」）。CI 用 `./scripts/wait-ci.sh --pr <n>`（随时查状态加 `--status`，0 绿 / 1 红 / 2 错误 / 3 仍在跑）；门禁用 `./scripts/gate.sh build|test|preflight|ledger`（短输出用 `--tail N`；管道规则见 §2）。
- **`job_output(wait=true)` 有宿主侧等待上限（默认 600s，上限值随宿主配置，以 `job_output` 的实际返回文案为准）**：传更大的 `timeout_ms` 会被宿主静默截断，返回 `[status: running]`——不是作业出错，也不是结束。#253 因此丢了 600s 并错过只读验证轮的入口；本波（#294–#298）再撞顶 3 次、每次空转 10 min。**等一个可能超过 10 分钟的作业要分段**：每次 `running` 返回后做一件只读工作（读 diff / 拟关闭评论 / 查 `wait-ci.sh --status`）再等下一段；**不要**用一次超长 wait 假装能阻塞到底。宿主侧放开该上限的交接片段见 #309 AC8（需改 `~/.dsh` 的 `tool-jobs` config，沙箱只读，由用户执行）。
- **仓内搜索用 `git grep`（#289 AC8）**：它只扫版本控制内文件，天然跳过 `build/`、`install/`、`.cache/`。`grep -rn --include=*` 全仓跑会把编译产物与缓存一起扫（#253 实测单次 131.8s，`find .cache` 另花 94.2s）；要搜运行产物就显式给 `.cache/` 路径。
- **跨调用文件与缓存**：每次 bash 调用的 `/tmp` 相互隔离，跨调用产物写 `.cache/`（已 gitignore）；`~/.conan2` / `~/.cache/gh` / `~/.npm` 只读时相关脚本已回退仓库 `.cache/`，仍被拒时一次性申请更高权限，不要反复重试。
- **权限与交接**：会话从 `read-only` 转执行（写文件 / commit / push）前请用户切回一次 `workspace-write`，不要逐操作申请 escalation；`handoff` 文档写 `.cache/handoff/`（`/tmp` 不可跨会话）。
- **不要自建后台进程**：DSH 在工具调用结束时回收直接 fork 的孙进程（实测 `setsid` 子进程也活不过调用边界），后台等待一律用 `run_in_background`。
- **读写守卫**：宿主文件守卫按「读过才可改」判定，批量重写后直接改会报 stale；此时重新读取再改，不要反复重试同一改动。

## 反模式（实际踩过）

- 审未提交工作区 → 审查期间工作区被改（含并行线提交），结论失效，提交后还要 amend。
- 审查期间继续 amend/改代码、或 verifier 没立即返回就重跑 → findings 对旧 commit 作废、重复审查（#93）。
- 并行 spawn 多个 verifier → 同一分钟给出相反结论，白跑一轮。
- `sleep` / `gh run watch` 等 CI、`gh run view --log-failed` 因 `~/.cache/gh` 只读失败 → 时间与上下文双浪费。
- **CI 绿就关 issue / PR 未合并就关 issue** → 下游卡住、PR 悬空与冲突返工（#91/#92）。
- **共享工作区留在 feature 分支 / 长期不提交的文档批次占着 `git status`** → 阻塞并行会话（#91→#92；ADR 0026–0030 从 10-22 挂到 10-23）。
- **把一次修复拆成多个 commit 反复 push** → 每个 commit 取消并重起 ≈35–40 分钟 CI（#93 取消 3 个 run；#252 在旧守卫下还串行等了两个周期 ≈56min）。
- 门禁命令接 `| tail`、`| head` → 退出码被管道吞掉；结论本身仍可从 verdict 行 / 分命令槽 `.cache/gate/last-status-<cmd>` 读到（`wait-ci.sh` 无兜底，别接）。判绿口径见本文 §2；`ledger` 槽永不放行推送（#316 AC2）。
- 用 bash `sleep` 等异步子代理 → 白等且触发重复工具提醒（#78 19 次 5028s、#81 10 次 2400s）；子代理完成会自动通知。
- 省略 issue close → 人工追问，下游 `blocked_by` 卡住。
- 共享工作区做并行票 → 提交父节点带上另一条线，push 时一并发布。
- `/tmp` 存跨调用状态 → 下次 bash 调用文件消失（DSH 沙箱每次调用隔离 `/tmp`；临时文件写 `.cache/`）。
