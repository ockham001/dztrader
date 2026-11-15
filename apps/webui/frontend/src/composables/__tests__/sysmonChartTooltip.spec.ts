import { describe, expect, it } from 'vitest'
import { tooltipNumericValue } from '../sysmonChartTooltip'
import { UNAVAILABLE_TEXT, formatPercent } from '../format'

// #320 回归网：SysmonHistoryChart 在视图测试里被整体 stub，组件内部无覆盖；
// 这里钉死 #320 验收标准 3 列的七种输入：number / [x,y] / [x,null] / [x,undefined] /
// NaN / Infinity / undefined（另含空数组、单元素数组、字符串、对象的类型兜底）。
// 缺陷原形：`typeof item.value === 'number'` 把二维点 `[x, y]` 判成 null，
// 导致 tooltip 三个数值列恒显示「不可用」。

describe('tooltipNumericValue：formatter 的 item.value → 数值 / null', () => {
  it('一维数字直接返回（含 0 这种合法读数）', () => {
    expect(tooltipNumericValue(0)).toBe(0)
    expect(tooltipNumericValue(12.5)).toBe(12.5)
    expect(tooltipNumericValue(-3)).toBe(-3)
  })

  it('二维点 [时间戳, 值] 取 y 值——本票修复的形态', () => {
    expect(tooltipNumericValue([1_790_000_000_000, 42.5])).toBe(42.5)
    expect(tooltipNumericValue([1_790_000_000_000, 0])).toBe(0)
    // 多于两元素时仍取索引 1（ECharts 允许 [x, y, …]）
    expect(tooltipNumericValue([1_790_000_000_000, 42.5, 7])).toBe(42.5)
  })

  it('二维点 y 值为 null / undefined 时判不可用', () => {
    expect(tooltipNumericValue([1_790_000_000_000, null])).toBeNull()
    expect(tooltipNumericValue([1_790_000_000_000, undefined])).toBeNull()
  })

  it('非有限数判不可用（NaN / ±Infinity 不得画成读数）', () => {
    expect(tooltipNumericValue(Number.NaN)).toBeNull()
    expect(tooltipNumericValue(Number.POSITIVE_INFINITY)).toBeNull()
    expect(tooltipNumericValue([1_790_000_000_000, Number.NaN])).toBeNull()
    expect(tooltipNumericValue([1_790_000_000_000, Number.NEGATIVE_INFINITY])).toBeNull()
  })

  it('空值与其它类型判不可用', () => {
    expect(tooltipNumericValue(undefined)).toBeNull()
    expect(tooltipNumericValue(null)).toBeNull()
    expect(tooltipNumericValue('42')).toBeNull()
    expect(tooltipNumericValue({})).toBeNull()
    expect(tooltipNumericValue([])).toBeNull()
    expect(tooltipNumericValue([42])).toBeNull()
  })

  it('不可用与 0 在呈现上也必须可区分（《帧契约：系统监控》§6.1：不得用 0 冒充）', () => {
    // 单看返回值已由上面各例钉住；这里钉到链路的另一端：同一个格式化器对两者的输出不同，
    // 否则「区分」只是类型层面的事实，用户看不出差别。
    expect(formatPercent(tooltipNumericValue(0))).toBe('0.0%')
    expect(formatPercent(tooltipNumericValue(null))).toBe(UNAVAILABLE_TEXT)
    expect(formatPercent(tooltipNumericValue(0))).not.toBe(formatPercent(tooltipNumericValue(null)))
  })
})
