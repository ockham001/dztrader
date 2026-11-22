/**
 * CPU 块两条纯规则的测试（#435）：固定 0–100% 趋势几何 + 每核条形列数。
 *
 * 为什么单独测纯函数：jsdom 没有布局，组件测试无法驱动「容器多宽 → 折成几行」；而
 * 「固定轴不拉伸」「多行等分」正是本票的验收标准，必须逐条锁死在这里
 * （与 `topbarOverflow.spec` 同一先例）。
 */
import { describe, it, expect } from 'vitest'
import {
  CORE_BAR_GAP,
  CORE_BAR_MIN_WIDTH,
  PERCENT_TREND_GRIDLINE_YS,
  coreBarColumns,
  percentTrendGeometry,
} from '../monitorCpu'

/** 解析 `polyline points` 成 [x, y] 数组（SVG viewBox 坐标） */
function parsePoints(points: string): Array<[number, number]> {
  return points.split(' ').map(pair => {
    const [x, y] = pair.split(',').map(Number)
    return [x, y] as [number, number]
  })
}

describe('percentTrendGeometry · 固定 0–100% 纵轴（#435 决策 2）', () => {
  it('空序列 / 只有一个点 / 全为非法值时不渲染（画不出趋势）', () => {
    expect(percentTrendGeometry([])).toBeNull()
    expect(percentTrendGeometry([{ ts: 0, value: 0.5 }])).toBeNull()
    expect(percentTrendGeometry([
      { ts: 0, value: Number.NaN },
      { ts: 1, value: 0.2 },
    ])).toBeNull()
  })

  it('y 按 0–100% 映射到整幅高度，不随序列 min/max 拉伸', () => {
    // 序列在 19%–21% 之间波动：固定轴下都在底部附近，而不是像自适应轴那样铺满全高
    const geometry = percentTrendGeometry([
      { ts: 0, value: 0.19 }, { ts: 1, value: 0.21 }, { ts: 2, value: 0.2 },
    ])
    expect(geometry).not.toBeNull()
    const points = parsePoints(geometry!.points)
    expect(points).toHaveLength(3)
    // 19% → y = 2 + 30 × 0.81 = 26.3；21% → 25.7（上下只差 0.6 单位，接近底边）
    for (const [, y] of points) {
      expect(y).toBeGreaterThan(25)
      expect(y).toBeLessThan(27)
    }
  })

  it('0% / 50% / 100% 分别落在下边框 / 中线 / 上边框', () => {
    const geometry = percentTrendGeometry([
      { ts: 0, value: 0 }, { ts: 1, value: 0.5 }, { ts: 2, value: 1 },
    ])
    const points = parsePoints(geometry!.points)
    expect(points[0][1]).toBe(32) // 底边（TREND_PAD + PLOT_HEIGHT）
    expect(points[1][1]).toBe(17) // 中线
    expect(points[2][1]).toBe(2) // 顶边（TREND_PAD）
  })

  it('越界值夹到 [0,1]：异常快照不会把曲线画到框外', () => {
    const geometry = percentTrendGeometry([
      { ts: 0, value: -0.4 }, { ts: 1, value: 1.6 },
    ])
    const points = parsePoints(geometry!.points)
    expect(points.map(([, y]) => y)).toEqual([32, 2])
  })

  it('x 按时间比例铺满 0–100（采集缺口如实反映为横向疏密）', () => {
    const geometry = percentTrendGeometry([
      { ts: 0, value: 0.2 }, { ts: 10, value: 0.2 }, { ts: 20, value: 0.2 },
    ])
    expect(parsePoints(geometry!.points).map(([x]) => x)).toEqual([0, 50, 100])
  })

  it('时间跨度为零时退回序号等距（不除零）', () => {
    const geometry = percentTrendGeometry([
      { ts: 5, value: 0.2 }, { ts: 5, value: 0.4 }, { ts: 5, value: 0.6 },
    ])
    expect(parsePoints(geometry!.points).map(([x]) => x)).toEqual([0, 50, 100])
  })

  it('lastValue 是末个有效点的原始值（供末点断言，与当前数字同源）', () => {
    const geometry = percentTrendGeometry([
      { ts: 0, value: 0.2 }, { ts: 1, value: 0.45 },
    ])
    expect(geometry!.lastValue).toBe(0.45)
  })

  it('网格线是静态几何：5 条、自下而上（y 递减）、覆盖 0/25/50/75/100%', () => {
    expect(PERCENT_TREND_GRIDLINE_YS).toHaveLength(5)
    for (let index = 1; index < PERCENT_TREND_GRIDLINE_YS.length; index += 1) {
      expect(PERCENT_TREND_GRIDLINE_YS[index]).toBeLessThan(PERCENT_TREND_GRIDLINE_YS[index - 1])
    }
    expect(PERCENT_TREND_GRIDLINE_YS[0]).toBe(32) // 0%
    expect(PERCENT_TREND_GRIDLINE_YS[4]).toBe(2) // 100%
  })
})

describe('coreBarColumns · 每核条形按容器宽度自适应且多行等分（#435 决策 6）', () => {
  it('无核时不渲染（0 列）', () => {
    expect(coreBarColumns(0, 1300)).toBe(0)
    expect(coreBarColumns(-3, 1300)).toBe(0)
  })

  it('宽度未测量（0 / NaN）时退回单行，不把条形误折成多行', () => {
    expect(coreBarColumns(16, 0)).toBe(16)
    expect(coreBarColumns(16, Number.NaN)).toBe(16)
  })

  it('宽容器放得下就一行（8 / 16 核在 1300px 下不折行）', () => {
    expect(coreBarColumns(8, 1300)).toBe(8)
    expect(coreBarColumns(16, 1300)).toBe(16)
  })

  it('核数多时折成多行且每行完全相等：32 → 16+16，48 → 16+16+16', () => {
    expect(coreBarColumns(32, 1300)).toBe(16)
    expect(coreBarColumns(48, 1300)).toBe(16)
  })

  it('窄容器更早折行，且优先选能整除的行数：16 核 500px → 4+4+4+4（不是 6+6+4）', () => {
    expect(coreBarColumns(16, 500)).toBe(4)
  })

  it('128 核在 1300px 下 8 行 × 16（等于 128，完全等分）', () => {
    expect(coreBarColumns(128, 1300)).toBe(16)
  })

  it('列数受最小格宽约束：不窄于 CORE_BAR_MIN_WIDTH（加上间距后按 1/72 递减）', () => {
    const width = 1300
    const columns = coreBarColumns(64, width)
    const cellWidth = (width - (columns - 1) * CORE_BAR_GAP) / columns
    expect(cellWidth).toBeGreaterThanOrEqual(CORE_BAR_MIN_WIDTH)
  })

  it('典型核数在常见宽度下：末行至少占一半（不出现「23 + 1」）', () => {
    // 窄容器只出现在手机 / 窄卡片，那边不可能有 64+ 核；宽容器才跑大核数机器。
    const cases: Array<{ width: number; counts: number[] }> = [
      { width: 350, counts: [4, 6, 8, 12, 16, 20, 24, 32] },
      { width: 500, counts: [4, 6, 8, 12, 16, 20, 24, 32] },
      { width: 750, counts: [4, 6, 8, 12, 16, 20, 24, 32, 40, 48, 64, 96, 128] },
      { width: 900, counts: [4, 6, 8, 12, 16, 20, 24, 32, 40, 48, 64, 96, 128] },
      { width: 1300, counts: [4, 6, 8, 12, 16, 20, 24, 32, 40, 48, 64, 96, 128] },
      { width: 2400, counts: [4, 6, 8, 12, 16, 20, 24, 32, 40, 48, 64, 96, 128] },
    ]
    for (const { width, counts } of cases) {
      for (const count of counts) {
        const columns = coreBarColumns(count, width)
        const rows = Math.ceil(count / columns)
        if (rows === 1) continue
        const lastRow = count - (rows - 1) * columns
        expect(lastRow, `count=${count} width=${width} columns=${columns}`)
          .toBeGreaterThanOrEqual(Math.ceil(columns / 2))
      }
    }
  })
})
