import { describe, expect, it } from 'vitest'
import { tooltipNumericValue } from '../sysmonChartTooltip'

// #320 回归网：SysmonHistoryChart 在视图测试里被整体 stub，组件内部无覆盖；
// 这里钉死 tooltip 取值的六种输入形态。缺陷原形：`typeof item.value === 'number'`
// 把二维点 `[x, y]` 判成 null，导致 tooltip 三个数值列恒显示「不可用」。

describe('tooltipNumericValue：formatter 的 item.value → 数值 / null', () => {
  it('一维数字直接返回（含 0 这种合法读数）', () => {
    expect(tooltipNumericValue(0)).toBe(0)
    expect(tooltipNumericValue(12.5)).toBe(12.5)
    expect(tooltipNumericValue(-3)).toBe(-3)
  })

  it('二维点 [时间戳, 值] 取 y 值——本票修复的形态', () => {
    expect(tooltipNumericValue([1_790_000_000_000, 42.5])).toBe(42.5)
    expect(tooltipNumericValue([1_790_000_000_000, 0])).toBe(0)
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

  it('不可用与 0 严格区分（契约 sysmon §6.1：不得用 0 冒充）', () => {
    expect(tooltipNumericValue(0)).toBe(0)
    expect(tooltipNumericValue(null)).toBeNull()
    expect(tooltipNumericValue(0)).not.toBeNull()
  })
})
