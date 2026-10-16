# 域文档规则

本仓库的工程技能在探索代码库前按此消费域文档。

## 动手前读这些

- 根目录的 [`GLOSSARY.md`](../../GLOSSARY.md)（单上下文仓库；本仓无 `GLOSSARY-MAP.md`）
- [`docs/adr/`](../adr/)：读与你要动的区域相关的 ADR

这些文件暂时不存在时**静默继续**——不要提示缺失、不要建议先创建；术语与决策在真正解决时才由 `domain-modeling` 惰性补上。

## 使用术语表的词汇

输出里出现领域概念时（issue 标题、重构提案、假设、测试名），用 `GLOSSARY.md` 定义的词；不要漂移到它标记为 _避免_ 的同义词。

概念不在表里是信号：要么你在发明项目不用的语言（重新考虑），要么确有缺口（留给 `domain-modeling` 补）。

## 本仓 ADR 规则（覆盖技能默认格式）

- 格式与摆放：`docs/adr/NNNN-<slug>.md`，模板 = Status / Context / Decision / Consequences（+ 需要时的 Supersedes）；决策 20 行内讲清，Decision 必须写明**为什么不用另一个方案**
- **编号只增不回收**；一经 Accepted 不修改——推翻写新 ADR，旧 ADR 加 Supersedes 引用
- **不引用本地归档路径**：`docs/plans/`、`docs/specs/`、`docs/superpowers/` 不在版本控制内（公开仓库不可见），新 ADR 引用它们等于制造悬空链接

## 与 ADR 冲突时

输出与既有 ADR 矛盾时**显式指出**，不要静默覆盖：

> _与 ADR-0013（不做状态与费率）矛盾，但值得重开，因为……_
