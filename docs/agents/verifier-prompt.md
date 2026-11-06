# verifier prompt 模板

> 用途：`code-review` 双轴 verifier 的 **prompt 骨架**。[ticket-closeout.md](ticket-closeout.md) §1 第 5 条只留指针，规则正文在这里。
> 直接复制下面对应的块、替换 `<>` 占位符即可——**不要**只写「预算 ≤N 分钟」，那是已知失效形态（见末节）。

---

## 按 diff 规模选档（§1 / §2 复制前先算）

档位按 `git diff --stat <base>...HEAD` 的**总增删行数**判定（= insertions + deletions，不是 PR 页面 files 列表的条数），先算档再填模板里的 `<12>` / `<30>`：

| 档位（按 `git diff --stat <base>...HEAD` 总增删行数判定） | 总增删行数 | 单轴预算 | 实测样本（diff 规模 → 实际耗时 / 调用数） |
|---|---|---|---|
| small | < 200 行 | 12 min / 30 calls | #272 +8/−49 = 57 行：183s / 392s，其中一轴 **46 calls 超 30**（可接受超支，未拖延交付） |
| medium | 200–1000 行 | 20 min / 60 calls | #284 +807/−30 = 837 行：316s / 598s（正常样本） |
| large | > 1000 行 | **拆块**：Lead 按文件群拆给两个同轴 reviewer，每块 20 min / 60 calls | #253 +1878/−44 = 1922 行：38.7min / 45.3min、133 / 167 calls（**超支样本**） |

- **large 档不是让单个 reviewer 跑更久**：预算越长，verifier 越先自由探索（见「为什么必须是步骤序」）。拆块 = 同一轴的两份报告由 Lead 合并，不是新增轴、也不占新的 teammate 名额。
- 预算只是上限，**不等于可以拖到上限**：T+8min 必须已发出 interim 结论（§1 / §2 模板的第 0 步）。

---

## 1. Standards 轴

```text
只读审查。严格 <12> 分钟内完成并直接输出报告；最多 <30> 次工具调用。工作目录 <repo>。
（<12> / <30> 按上一节「按 diff 规模选档」表填，不要沿用默认值。）

审查对象：**已冻结的 commit**，固定点 <base 短 hash>，HEAD = `<HEAD 短 hash>`。

步骤（照做即可，不要扩展）：
0. T+8min 先 send_message 一条 interim 结论（未完成也发：已完成哪些 AC / 正在查什么 / 预计还需多久）
1. 跑一次 `git diff <base>...<HEAD>`
2. 读 <只读锚点文件>（第 <a>-<b> 行）
3. 直接输出报告

不要跑构建/测试；不要探索代码库其它部分。

上一轮 findings（逐条确认已修 / 未修并说明理由）：
<把上一轮 findings 原文贴全>

报告 ≤400 词，只列 BLOCKING / NON-BLOCKING，每条附 `文件:行号` + 一句修复建议。
无 finding 就写「无 blocking」。
```

## 2. Spec 轴

```text
只读审查。严格 <12> 分钟内完成并直接输出报告；最多 <30> 次工具调用。工作目录 <repo>。
（<12> / <30> 按上一节「按 diff 规模选档」表填，不要沿用默认值。）

审查对象：**已冻结的 commit**，固定点 <base>，HEAD = `<HEAD>`。

步骤（照做即可，不要扩展）：
0. T+8min 先 send_message 一条 interim 结论（未完成也发：已完成哪些 AC / 正在查什么 / 预计还需多久）
1. 跑一次 `gh issue view <N> --json title,body` 取 AC 原文
2. 跑一次 `git diff --stat <base>...<HEAD>` 确认改动范围
3. 跑一次 `git diff <base>...<HEAD>` 取全量
4. 按 AC 逐条判定，直接输出报告

不要跑构建/测试；不要通读旧版全文（只在怀疑某条真相丢失时用 `git show <base>:<file> | sed -n` 查对应段落）。

报告按 AC 逐条 PASS / FAIL / 可接受偏差，每条附证据（行号或命令输出）。
最后一行给 `AC 通过 X/Y`。
```

## 3. 只读验证轮（修复之后）

```text
只读验证。严格 <6> 分钟内完成并直接输出报告；最多 <10> 次工具调用。

上一轮提了 <N> 条 finding，Lead 已在 commit `<hash>` 修复。逐条确认修复是否生效，
**不要提出新 finding**（只验证这 N 条）。

步骤（照做，不要扩展）：
1. 跑一次 `git diff <fixbase>...<fixhead>`
2. 跑一次 `<只读检查命令>`
3. 直接输出报告
4. 交付后执行 `python3 scripts/review_marker.py write <issue> <axis>`（默认把当前 HEAD 写入
   `.cache/review/<issue>-<axis>.done`；验证的是更早的 commit 才用第三个参数显式传 sha）

报告 ≤200 词，末尾一行：`<N> 条 finding：X PASS / Y FAIL`。
```

> 该模板返回后，Lead 才能进入 [ticket-closeout.md](ticket-closeout.md) §4 的 push / 建 PR（硬前置：最后一轮只读验证已返回且无 BLOCKING）；未返回先 `wait_agent`，#252 因跳过该动作点把一次修复放大成两个串行 CI 周期（≈56min，证据见 ticket-closeout §1 第 11 条）。
>
> 第 4 步写的标记由 `.githooks/pre-push` 只读检查（`review_marker.py check`，只 warning 不拦截），专门防 #253 的「验证轮被中止 → 未验证即合并」：那次修复 commit 推送时验证轮还没跑，验证准备随即被宿主 turn 中止，合并时该轮**从未发生**。标记让「哪个轴的验证停留在哪个 sha」在推送时可见；文件名为 `.cache/review/<issue>-<axis>.done`，`<axis>` 用 `standards` / `spec`。

---

## 4. 实现类 teammate prompt 模板

审查模板之外，实现类（写代码 / 写脚本 / 写文档）teammate 也需要骨架——**没有时间盒的探索会吃掉整条关键路径**：#253 的 recon 花了 37–39 min 才写第一行代码，两个实现成员中途死亡后产出全压在 Lead 侧（`failed` ≠ 无产出，见 [ticket-closeout.md](ticket-closeout.md) §1 第 10 条）。

```text
你是 <角色>，负责 GitHub issue #<N> 的 <AC 编号 + 一句话范围>。工作目录 <repo>，
当前分支 <branch>（不要切分支、不要建 worktree）。

固定点：base = `<base sha>`，HEAD = `<HEAD sha>`；冻结后不得 amend / rebase / 改历史。

写作用域（只允许改这些路径，其它文件一律不碰——其他成员同时在改别的文件）：
- <path 1>（<改动性质>）
- <path 2>（<改动性质>）

硬纪律：
- **T+15min 先落最小骨架**（哪怕只是空实现 + 测试骨架 + 接线），再继续探索与补细节。
  骨架落盘前不要长时间只读：Lead 看不到任何可复用产出，也无法判断进度（#253 反例）。
- 禁止事项：不 commit、不 push、不跑 build/test（门禁由 Lead 跑）；不 `sed -i` / heredoc
  重写已有文件；不碰写作用域外的路径；不顺手重构相邻代码。
- 自测命令（只跑这些；输出与退出码都要留在报告里）：
  `python3 scripts/<checker>.py --selftest`
  `<其它最小验证命令>`

报告格式（send_message 给 Lead，按下面四段，不写散文）：
1. 改动文件清单 + 每个文件一句话说明；
2. 每条 AC 的验收命令 + 真实输出 + 退出码；
3. 反向验证：旧实现红 → 新实现绿（或明确写「不适用 + 理由」）；
4. 未决事项 / 已知偏差 / 需要 Lead 决策的点。
```

---

## 为什么必须是步骤序

上限是软约束：verifier 会先自由探索，撞到上限才收尾，而「先探索」正是超支来源。**把步骤写死，它就没有探索的余地。**

实测对照（同仓同会话，同一类审查）：

| prompt 形态 | 结果 |
|---|---|
| 只写「≤15 min」 | #200 两个 verifier 跑 40+ min 零报告，被迫 interrupt；#216 `loss-audit` 19.4 min、`review-standards` 14.4 min 均零产出 |
| 步骤序 + 「不许做什么」 | #200 重派版 5 min 内交付；#216 重派版 4.8 min / 0.7 min 交付；#203 每轮 8–16 min 且照样抓出真问题 |

判据：prompt 里若只出现「预算 ≤N min」而**没有**「步骤 1/2/3 + 不许做什么」，就没有达标。

## 成本封套（每次都带）

1. 先一次 `git diff <base>...HEAD` 取全量，**禁止**逐文件/逐目录 diff。
2. 默认不跑构建/测试（门禁由 Lead 跑）；确需实测只跑与 AC 直接相关的最小命令。
3. 变异实验只在 `mktemp -d` 副本，**禁止**临时改仓库文件（冻结纪律硬边界）。
4. 报告有词数上限——没有上限的报告会变成第二份代码库。

依据：#207 Standards verifier 85 min / 345 次 bash 只产出 3 条 non-blocking + 3 条 nit，而 blocking finding 来自 28.6 min 的 Spec verifier。
