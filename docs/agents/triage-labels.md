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
- **建 `ready-for-agent` 票时就带上 `## 拍板记录`**：`scripts/check_ledger.py` 的 R4 只认这个二级标题（`### 拍板记录` 与 `## 拍板记录结论` 都不算），缺段或空段就报警。可复制模板：

```markdown
## 拍板记录

- **来源**：#NNN 复盘 / 用户当面拍板 / ADR NNNN 决策 N（写清决策从哪来）
- **合并授权**：CI 绿 + 双轴审查（仓库规范 / spec 一致性）通过即合并关单；用户要自己看 PR 时改为「先不合并，PR 留给我审」
- **关单纪律**：AC 全部勾选后才关闭；确需真机 / 人工验证的项标 `- [ ] ⏳`
- **父票**：Part of #NNN（无母票则删掉本行）
```

  R4 报警**不是开工阻塞**：缺段时照常实现，把合并授权提问放到 PR 创建后（[ticket-closeout.md](ticket-closeout.md) §5「开工时 R4 报警的处理」）。
