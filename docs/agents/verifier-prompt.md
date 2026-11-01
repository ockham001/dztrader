# verifier prompt 模板

> 用途：`code-review` 双轴 verifier 的 **prompt 骨架**。[ticket-closeout.md](ticket-closeout.md) §1 第 5 条只留指针，规则正文在这里。
> 直接复制下面对应的块、替换 `<>` 占位符即可——**不要**只写「预算 ≤N 分钟」，那是已知失效形态（见末节）。

---

## 1. Standards 轴

```text
只读审查。严格 <12> 分钟内完成并直接输出报告；最多 <30> 次工具调用。工作目录 <repo>。

审查对象：**已冻结的 commit**，固定点 <base 短 hash>，HEAD = `<HEAD 短 hash>`。

步骤（照做即可，不要扩展）：
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

审查对象：**已冻结的 commit**，固定点 <base>，HEAD = `<HEAD>`。

步骤（照做即可，不要扩展）：
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

报告 ≤200 词，末尾一行：`<N> 条 finding：X PASS / Y FAIL`。
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
