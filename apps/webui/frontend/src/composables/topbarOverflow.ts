/**
 * 顶栏资源组的「按宽度自适应折叠」算法（ADR 0048；契约 sysmon §2.4 的顶栏取景）。
 *
 * 为什么单独成模块：真正的折叠判定发生在**浏览器布局之后**（要知道每一项渲染出来多宽），
 * 而 jsdom 没有布局（所有元素宽度都是 0），因此组件测试无法驱动它。把判定抽成纯函数后，
 * 「放得下多少项」这件事可以逐条锁死在单测里，组件只负责测量与渲染。
 *
 * 术语：
 * - **可用宽度**（`availableWidth`）：资源组容器的 `clientWidth`，即服务徽章与分隔线之后剩下的空间。
 * - **溢出入口**（overflow trigger）：折叠发生时行尾出现的 `▾ N` 按钮，它自己也要占宽度。
 *
 * 规则（与主流 pattern 一致：Apple HIG 的 system-managed overflow menu、GitLab 的 `+N more`）：
 * 能全部显示就全部显示；放不下时**从尾部开始**收起，直到「可见项 + 溢出入口」装得下。
 * 收起数量为 0 时不渲染溢出入口（不留一个点不开的空按钮）。
 */

/**
 * 可容纳的**可见项数量**。
 *
 * @param itemWidths 每一项的渲染宽度（按展示顺序，含被折叠的项）
 * @param availableWidth 资源组的可用宽度
 * @param gap 项间距（容器 `column-gap`）
 * @param overflowWidth 溢出入口自身的宽度（它只在与项同行时才占位）
 * @returns 可见项数量（`itemWidths.length` 表示全部可见；`0` 表示连一项都放不下）
 *
 * 边界：
 * - 全部放得下（`含间距的总宽 ≤ 可用宽度`）→ 返回全部，**不预留**溢出入口的宽度。
 * - 放不下 → 预算里先扣掉「溢出入口 + 一个间距」，再从头部逐项累加。
 * - `availableWidth` 为 0（jsdom / 尚未测量）时总宽也为 0，按「全部可见」处理 —— 不会误把
 *   所有徽章折叠掉，测试环境里因此看到的是完整徽章组。
 */
export function visibleItemCount(
  itemWidths: readonly number[],
  availableWidth: number,
  gap: number,
  overflowWidth: number,
): number {
  if (itemWidths.length === 0) return 0
  const safeGap = Number.isFinite(gap) && gap > 0 ? gap : 0
  const totalWidth = itemWidths.reduce((sum, width) => sum + width, 0)
    + safeGap * (itemWidths.length - 1)
  if (!(availableWidth > 0) || totalWidth <= availableWidth) return itemWidths.length

  const safeOverflow = Number.isFinite(overflowWidth) && overflowWidth > 0 ? overflowWidth : 0
  const budget = availableWidth - safeOverflow - safeGap
  let used = 0
  let count = 0
  for (const width of itemWidths) {
    const next = used + width + (count > 0 ? safeGap : 0)
    if (next > budget) break
    used = next
    count += 1
  }
  return count
}
