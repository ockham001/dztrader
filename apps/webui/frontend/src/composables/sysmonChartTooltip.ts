/**
 * 历史图表 tooltip 的取值整形（#320）。
 *
 * 为什么单独成模块：`SysmonHistoryChart` 在视图测试里被整体 stub（`MonitorHistoryView.spec.ts`
 * 与 `MonitorViewTrend.spec.ts` 都是 `<div class="chart-stub" />`），组件内部逻辑没有回归网；
 * 而 tooltip 恒显示「不可用」正是这类"组件没人测、回调里一行判断写错"的典型缺陷。把取值
 * 抽成纯函数后可以用确定性单测钉死，组件只负责把返回值交给格式化函数。
 *
 * 背景事实（#320 复现）：ECharts 的 line series 数据以二维点 `[时间戳, 值]` 给出时，
 * formatter 回调里的 `item.value` 是**数组**而不是数字；早期实现用
 * `typeof item.value === 'number'` 判空，于是数组一律被判成 null，tooltip 三个数值列
 * 全部渲染成「不可用」，而同一时刻曲线本身画得出来。
 */

/** tooltip formatter 回调里单条参数的最小形状（ECharts 传对象，字段可能缺失） */
export interface ChartTooltipParam {
  seriesName?: string
  value?: unknown
  marker?: string
  /** 轴触发时该点的 x 值（毫秒时间戳）；用于 tooltip 头部的完整时刻 */
  axisValue?: unknown
}

/**
 * 从 formatter 回调的 `item.value` 取标量数值。
 *
 * 支持的形态：
 * - `number` —— 直接使用（须有限；`NaN` / `±Infinity` 视为不可用）；
 * - `[x, y]` / `[x, y, …]` —— 取索引 1 作为 y 值（ECharts 二维点约定）；
 * - 其余（`undefined` / `null` / 字符串 / 对象）—— 不可用。
 *
 * 返回 `null` 表示「不可用」，与真实读数 0 严格区分
 * （《帧契约：系统监控》§6.1：不得用 0 冒充不可用）。
 */
export function tooltipNumericValue(value: unknown): number | null {
  const candidate = Array.isArray(value) ? value[1] : value
  if (typeof candidate !== 'number') return null
  return Number.isFinite(candidate) ? candidate : null
}
