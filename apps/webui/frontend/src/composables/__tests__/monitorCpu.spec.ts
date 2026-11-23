/**
 * CPU 块两条纯规则的测试（#435）：固定 0–100% 趋势几何 + 每核条形列数。
 *
 * 为什么单独测纯函数：jsdom 没有布局，组件测试无法驱动「容器多宽 → 折成几行」；而
 * 「固定轴不拉伸」「多行等分」正是本票的验收标准，必须逐条锁死在这里
 * （与 `topbarOverflow.spec` 同一先例）。
 */
import { describe, it, expect } from 'vitest'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { parseCssFile, tokenDeclarationsOf } from '@/test-utils/cssTheme'
import {
  CORE_BAR_GAP,
  PERCENT_TREND_GRIDLINE_YS,
  coreBarColumns,
  percentTrendGeometry,
  percentTrendY,
} from '../monitorCpu'

const testDir = dirname(fileURLToPath(import.meta.url))
/** design-tokens.css 的 token 表（:root 段）：交叉校验 JS 常量与 CSS 真实值 */
function designTokens(): Map<string, string> {
  return tokenDeclarationsOf(
    parseCssFile(join(testDir, '..', '..', 'assets', 'css', 'design-tokens.css')),
    ':root',
    false,
  )
}

/** 解析 `polyline points` 成 [x, y] 数组（SVG viewBox 坐标） */
function parsePoints(points: string): Array<[number, number]> {
  return points.split(' ').map(pair => {
    const [x, y] = pair.split(',').map(Number)
    return [x, y] as [number, number]
  })
}

describe('趋势图几何 token（#445 第四轮：72→88 + 面积填充）', () => {
  it('绘图区高度与组成带高度在 design-tokens.css 里是 88px / 12px', () => {
    const tokens = designTokens()
    expect(tokens.get('--chart-height-trend')).toBe('88px')
    expect(tokens.get('--memory-band-height')).toBe('12px')
  })
})

describe('percentTrendGeometry · 固定 0–100% 纵轴（#435 决策 2）', () => {
  it('空序列 / 只有一个点 / 全为非法值时不渲染（画不出趋势）', () => {
    expect(percentTrendGeometry([])).toBeNull()
    expect(percentTrendGeometry([{ ts: 0, value: 0.5 }])).toBeNull()
    expect(percentTrendGeometry([
      { ts: 0, value: Number.NaN },
      { ts: 1, value: 0.2 },
    ])).toBeNull()
  })

  it('面积多边形从 0% 基线闭合（#445 第四轮）：首尾都落在 y=32，中间是折线本身', () => {
    const geometry = percentTrendGeometry([
      { ts: 0, value: 0.2 }, { ts: 1, value: 0.5 }, { ts: 2, value: 0.9 },
    ])
    expect(geometry).not.toBeNull()
    const area = parsePoints(geometry!.areaPoints)
    // 起点 (0, 32) → 折线三点 → 终点 (100, 32)
    expect(area[0]).toEqual([0, 32])
    expect(area[area.length - 1]).toEqual([100, 32])
    expect(area.slice(1, -1)).toEqual(parsePoints(geometry!.points))
  })

  it('阈值参考线 y 与折线共用同一映射：0%→32、100%→2、90%→5，越界夹取', () => {
    expect(percentTrendY(0)).toBe(32)
    expect(percentTrendY(1)).toBe(2)
    expect(percentTrendY(0.9)).toBe(5)
    expect(percentTrendY(0.5)).toBe(17)
    expect(percentTrendY(-1)).toBe(32)
    expect(percentTrendY(2)).toBe(2)
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

  it('firstTs / lastTs 取首个 / 末个有效点（横轴两端标签与曲线实际起止同源）', () => {
    const geometry = percentTrendGeometry([
      { ts: 5, value: Number.NaN },
      { ts: 10, value: 0.2 },
      { ts: 20, value: 0.3 },
      { ts: 25, value: Number.POSITIVE_INFINITY },
    ])
    expect(geometry!.firstTs).toBe(10)
    expect(geometry!.lastTs).toBe(20)
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
    // 精确几何：100/75/50/25/0% → y=2/9.5/17/24.5/32（TREND_PAD=2、PLOT_HEIGHT=30）
    expect([...PERCENT_TREND_GRIDLINE_YS]).toEqual([32, 24.5, 17, 9.5, 2])
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

  it('核数多时折成多行且每行完全相等：32 → 16+16，48 → 24+24', () => {
    expect(coreBarColumns(32, 1300)).toBe(16)
    expect(coreBarColumns(48, 1300)).toBe(24)
  })

  it('窄容器更早折行，且优先选能整除的行数：16 核 500px → 8+8（不是 6+6+4）', () => {
    expect(coreBarColumns(16, 500)).toBe(8)
  })

  it('128 核在 1300px 下 26 + 26 + 26 + 26 + 24（末行 24，仍过半）', () => {
    expect(coreBarColumns(128, 1300)).toBe(26)
  })

  it('8080 验收：柱更窄后更晚换行——16 核 750px 一行、640px 两行 8+8、手机宽度也 8+8', () => {
    // 750px 容器：16 × 43px ≤ 750 → 一行（柱约 44px 宽，仍看得清高低）
    expect(coreBarColumns(16, 750)).toBe(16)
    // 640px 容器：一行最多 14 根 → 8 + 8（不是 6 + 6 + 4）
    expect(coreBarColumns(16, 640)).toBe(8)
    // 390px 手机卡片内层（约 354px）：8 + 8
    expect(coreBarColumns(16, 354)).toBe(8)
  })

  it('最小格宽 / 间距常量与 design-tokens 的真实值一致（JS 与 CSS 不得各排一份间距）', () => {
    const tokens = designTokens()
    // 间距：CSS 用 var(--spacer-3) 排格间距，JS 用同一个数值算列数
    expect(tokens.get('--spacer-3')).toBe(`${CORE_BAR_GAP}px`)
    // 条形轨道高度（#445 密度压缩：54→46px，仍能看清单核高低）
    expect(tokens.get('--core-bar-height')).toBe('46px')
  })

  it('列数受最小格宽约束：1300px 下 64 核的每格宽度不窄于 CORE_BAR_MIN_WIDTH', () => {
    // 用字面量 40/3 断言（而不是同一常量自证）：最小格宽 / 间距改了这条必须跟着改，挡「常量漂移」
    const width = 1300
    const columns = coreBarColumns(64, width)
    expect(columns).toBe(16)
    const cellWidth = (width - (columns - 1) * 3) / columns
    expect(cellWidth).toBeGreaterThanOrEqual(40)
  })

  it('窄容器 + 大核数：48/128 仍折行且末行过半；350px 下 128 核刚好 16 行 × 8', () => {
    expect(coreBarColumns(48, 350)).toBe(8) // 6 行 × 8
    expect(coreBarColumns(48, 500)).toBe(8) // 6 行 × 8
    expect(coreBarColumns(128, 750)).toBe(16) // 8 行 × 16（完全等分）
    expect(coreBarColumns(128, 500)).toBe(10) // 13 行：12 × 10 + 8（末行 8 / 10）
    // 350px 下 8 列 × 16 行正好等分（40px 最小格宽让最窄容器也能放下 8 根）
    expect(coreBarColumns(128, 350)).toBe(8)
    expect(128 - (Math.ceil(128 / 8) - 1) * 8).toBe(8)
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
