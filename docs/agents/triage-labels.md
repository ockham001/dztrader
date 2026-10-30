# 分诊标签

技能用五个规范分诊角色表述；本文件把角色映射到本仓 issue 载体的实际标签字符串。

| mattpocock/skills 中的标签 | 本仓标签          | 含义                       |
| -------------------------- | ----------------- | -------------------------- |
| `needs-triage`             | `needs-triage`    | 维护者需要评估此 issue     |
| `needs-info`               | `needs-info`      | 等待报告者补充信息         |
| `ready-for-agent`          | `ready-for-agent` | 规格完整，可交给 AFK agent |
| `ready-for-human`          | `ready-for-human` | 需要人来实现               |
| `wontfix`                  | `wontfix`         | 不会处理                   |

技能提到某角色（如「apply the AFK-ready triage label」）时，用本表对应的标签字符串。

## 拍板闭环

- **`ready-for-agent` 前置**：spec **无未拍板项**；有未拍板项时先拍板，并把决策写进 issue 正文——不要等实现会话中途再问。
- **`/implement` 遇 `needs-triage`**：先跑决策清单（列出待拍板项 → 问用户 → 把决策写回 issue 正文 → 换标签），不要边实现边等拍板（#203 等人工决策 79 min，占该票墙钟 30%）。
