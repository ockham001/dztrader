# ADR 0062: WebUI 配色体系切到 DSH——品牌身份去色相化

## Status

Accepted（2026-11-28，来自 #534；用户明确拍板方向与范围）

## Context

`DESIGN.md` 的世界观是 **"The Instrument Panel"**：中性灰脊 + 三族彩色（品牌紫、涨跌、五个状态色）。其中品牌紫 `#4B3FE3` 来自 traework 时期的 export，用户要求"脱离一点痕迹"。

本次不是"微调色相"，而是**换整套配色世界**。用户给出三个参照系（DSH / opencode v2 / macOS 27），选定 **DSH（DeepSeek Harness）**，并明确两项边界：**只动颜色，不涉后端、不涉交互逻辑**；**logo 形态可以改**。

DSH 的配色哲学（源码：`@deepseek-ai/dsh-client-ui-theme/src/styles/design-platform.css`）与本项目现状正面冲突：

| 维度 | 本项目现状 | DSH |
|---|---|---|
| 品牌主色 | 紫 `#4B3FE3`（有彩） | **近黑/近白（无彩）**：浅色 `#0F1115`、深色 `#F9FAFB` |
| 品牌色角色 | 身份 + 登录按钮 + admin 徽章 | 身份**只剩对比度**，不携带色相 |
| 彩色落点 | 品牌紫 | 只落 `link` / `info`（DeepSeek 蓝 `#4176E6` / `#7AAAFF`） |
| 中性脊 | 纯中性灰（`#171717`/`#202020`/…） | 带蓝偏移的冷灰（`neutral-bluish-*`） |
| token 分层 | 两段（`:root` / `.dark`） | **静态色板层 + 语义别名层**：静态 82 个跨主题不变，仅 1 个（`-60`）例外；别名 146 个分主题 |

关键事实：**DSH 的 `brand-primary` 是一个"最高对比度中性色"**，不是一种颜色。浅色用近黑 `#0F1115` 填主按钮，深色用近白 `#F9FAFB`；文字随底翻转（`label-primary-foreground` / `label-primary-inverted`）。彩色只出现在 `link`、`button-info-fill`、`state-business-primary` 这类小面积语义位，绝不承担大面积背景。

这与 `DESIGN.md` 已有的 **Achromatic Chrome Rule**（"背景、边框、文字、图标和每个普通控件都是灰色；如果某个表面需要显得重要，就给它加字重或位置，而不是加颜色"）**同源**——DSH 只是把这条规则推到了"连品牌位也交给中性色"。

## Decision

WebUI 的深浅色主题 token 全部对齐 DSH 的语义别名层，品牌身份由**紫**改为**中性色**。

### 决策 1：品牌色去色相化

- `--bg-brand` = DSH `brand-primary`：浅色 `#0F1115`、深色 `#F9FAFB`（近黑/近白）
- `--text-onbrand` 随底翻转：浅色 `#FFFFFF`、深色 `#151517`
- `--text-brand` 同取中性色（DSH 的 `brand-text` 与 `brand-primary` 同值）

**为什么不用另外两个参照**：

- **保留紫色只微调色相**（本轮讨论过的 A/B/C 方案）：用户要的是"换风格"而非"换紫"，且紫色在深色底上必须靠提亮维持对比度，会持续与"中性灰脊"打架。
- **opencode v2**：它把 accent 让给 link blue，但中性脊是**纯中性**（官方明写"no warm or tinted greys"），缺少 DSH 的冷灰偏移；而本项目的状态色体系比 opencode 完整，硬套会塌掉层次。
- **macOS 27**：Apple 的 System Indigo 是 275°，与本项目现状 277° 几乎重合，等于没换；且 Apple **不公布 macOS 专用中性灰的 hex**，只能靠观感推导，证据最弱。

**为什么不保留"品牌紫只在 logo 上出现"**：logo 是全站最显眼的品牌实例，若界面是单色品牌而 logo 仍是紫渐变，两者会互相打架；且 `--bg-brand` 被 15 处组件消费（勾选框、单选框、滑块、进度条、日期选择器、时分选择、分栏拖拽、admin 标签等），保留紫色等于保留"品牌色 = 可操作控件色"这个 DSH 明确反对的用法。

### 决策 2：logo 改为单色

`DZ-Bolt.svg` 从"紫渐变底 + 白 T"改为"近白底 + 近黑 T"（深色）。几何不变（圆角方块 + 块状 T），只换填色。这是 DSH/opencode 的单色身份法。

### 决策 3：涨跌色保留方向，只调明度

**红涨绿跌是中国期货惯例，是不变量**，色相也不动（橙红 32° / 青绿 154°）。但深色卡底由 `#202020` 变为 `#1B1B1C`（更亮），原 `#D4543E` 会掉到 3.41:1，故：

| 主题 | rise | fall | 卡底对比度 |
|---|---|---|---|
| 深色 | `#ED6A53`（L 68%） | `#4AB071` | 5.56 / 6.34 |
| 浅色 | `#C3432E`（L 56%） | `#048145` | 4.83 / 4.75 |

涨跌色由此**随主题变化**（原实现深浅同值），落进 `theme.css` 的三段，与 `--log-*` 同一分工方式。

### 决策 4：与 DSH 刻意不同的三处

1. **浅色文字 ramp 整体下移一档**（primary 1000 / secondary 750 / tertiary 700）。DSH 的 `label-tertiary` = `#81858C`，在白底只有 **3.71:1**，低于本仓 `tokenContrast.spec.ts` 的 4.5 门禁。DSH 自己不设这条门禁；本项目必须设。下移后实测 5.55–5.80，全部取自 DSH 自己的阶梯，**不引入新色**。
2. **`--text-disabled` 取 `label-tertiary` 档而非 `label-dimmed`**。DSH 的 `label-dimmed` 在深色是 `#43454A`，在 `#151517` 上约 2.3:1；本仓对 disabled 有 1.5:1 下限，用 tertiary 档（`#81858C`）留余量。
3. **深色 `--bg-card` 取 `neutral-900` 而非 DSH 的 `card-fill`(`neutral-850`)**。本项目的 `--bg-card` 是"面板"（页面底上一档、且要铺密集表格），不是 DSH 的"卡片"。直接用 850 会让 `tertiary` 文字对比度从 8.4 掉到 6.5，且与 `--bg-base-secondary`(875) 只差一档、层次糊掉。

## Consequences

**门禁净收益**：`tokenContrast.spec.ts` 三条登记在 `KNOWN_VIOLATIONS` 的欠账**全部修复**，基线可收缩：

| 配对 | 旧值 | 新值（深/浅） |
|---|---|---|
| `tag-brand` | 3.33 | 17.45 / 18.90 |
| `tertiary-on-base` | 3.78 | 8.54 / 5.80 |
| `tertiary-on-card` | 3.44 | 8.06 / 5.55 |
| `rise-on-card` | 3.99 | 5.56 / 4.83 |

**观感变化（不可避免）**：

- 登录主按钮从紫色实心变成白底黑字（深色）/ 黑底白字（浅色）；
- 勾选框、单选框、滑块、进度条、日期选择、分时选择的选中态从紫变中性；
- `admin` 徽章与"部分成交"标签从紫变灰白——**语义上更正确**：它们本来就该是中性标签，紫只是历史遗留；
- 中性面整体带极轻的蓝偏移（DSH 冷灰）。

**资产变更**：`DZ-Bolt.svg` 换填色，属于品牌资产变更，已获用户同意。

**范围外**：不动任何 `.vue` 的逻辑、布局、间距、圆角、字号；不动后端与交互。`--text-brand` 的**语义**保持不变（仍是"品牌标签/强调文字"），只换值。

**文档同步**：`DESIGN.md` 前 metadata 与 Colors 章节重写（"Brand Violet" 一节作废，改为"Brand: neutral identity"）；`.impeccable/design.json` 的 brand 条目同步。

**未来若要回退**：本 ADR 记录的是"整套配色世界"的选择，不是单个色值；回退等于另写一份 ADR 并整体替换 `theme.css` 三段与 `design-tokens.css` 的状态/涨跌段。
