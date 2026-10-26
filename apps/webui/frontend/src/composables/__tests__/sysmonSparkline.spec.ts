import { describe, expect, it } from 'vitest'
import {
  SPARKLINE_MAX_POINTS,
  SPARKLINE_WINDOW_SEC,
  appendSparklinePoint,
  historySparklinePoints,
  mergeSparklinePoints,
  sparklineAriaLabel,
  sparklineGeometry,
  sparklineTitle,
  sparklineWindow,
  sparklineWindowComplete,
  sparklineWindowText,
  upsertLiveSparklinePoint,
  type SparklineHistoryWindow,
  type SparklinePoint,
  type SparklineWindow,
} from '../sysmonSparkline'
import type { SysmonHistoryPoint } from '@/types/api'

/** 历史桶（min/max/avg/last/n 同 null = 该桶无有效样本） */
function bucket(ts: number, avg: number | null, extra: Partial<SysmonHistoryPoint> = {}): SysmonHistoryPoint {
  return { ts, min: avg, max: avg, avg, last: avg, n: avg === null ? null : 1, ...extra }
}

function series(...values: number[]): SparklinePoint[] {
  return values.map((value, index) => ({ ts: 1_790_000_000 + index * 60, value }))
}

describe('historySparklinePoints（历史 REST 点 → sparkline 点）', () => {
  it('取每个桶的 avg 作为值，按 ts 升序', () => {
    const points = historySparklinePoints([
      bucket(1_790_000_060, 0.4),
      bucket(1_790_000_000, 0.3),
    ])

    expect(points).toEqual([
      { ts: 1_790_000_000, value: 0.3 },
      { ts: 1_790_000_060, value: 0.4 },
    ])
  })

  it('该桶无有效样本（四值全 null）时跳过，不用 0 冒充', () => {
    const points = historySparklinePoints([
      bucket(1_790_000_000, 0.3),
      bucket(1_790_000_060, null),
      bucket(1_790_000_120, 0.5),
    ])

    expect(points.map(point => point.value)).toEqual([0.3, 0.5])
  })

  it('avg 缺失但 last 有效时回退 last（部分数据仍可画）', () => {
    const points = historySparklinePoints([
      { ts: 1_790_000_000, min: null, max: null, avg: null, last: 7.5, n: 1 },
    ])

    expect(points).toEqual([{ ts: 1_790_000_000, value: 7.5 }])
  })

  it('过滤非法 ts / 非有限值', () => {
    const points = historySparklinePoints([
      { ts: Number.NaN, min: 1, max: 1, avg: 1, last: 1, n: 1 },
      { ts: 1_790_000_000, min: 1, max: 1, avg: Number.NaN, last: null, n: 1 },
      bucket(1_790_000_060, 0.5),
    ])

    expect(points).toEqual([{ ts: 1_790_000_060, value: 0.5 }])
  })

  it('空输入返回空数组（历史库可用但范围内无数据时 sparkline 不渲染）', () => {
    expect(historySparklinePoints([])).toEqual([])
  })
})

describe('appendSparklinePoint（WS 增量推进）', () => {
  it('追加新点：最新点为当前实时值（末点 = 旁边数字）', () => {
    const next = appendSparklinePoint(series(0.1, 0.2), { ts: 1_790_000_120, value: 0.7 })

    expect(next).toHaveLength(3)
    expect(next[next.length - 1]).toEqual({ ts: 1_790_000_120, value: 0.7 })
  })

  it('同一时刻的点替换而不是重复追加（快照时间戳未推进时不堆积）', () => {
    const next = appendSparklinePoint(series(0.1, 0.2), { ts: 1_790_000_060, value: 0.9 })

    expect(next).toHaveLength(2)
    expect(next[1]).toEqual({ ts: 1_790_000_060, value: 0.9 })
  })

  it('比末点更旧的点忽略（时钟回退不把线画成折返）', () => {
    const current = series(0.1, 0.2)
    const next = appendSparklinePoint(current, { ts: 1_790_000_000 + 30, value: 0.9 })

    expect(next).toEqual(current)
  })

  it('丢弃时间窗之外的旧点，只保留最近 15 分钟', () => {
    const newest = { ts: 1_790_001_000, value: 0.5 }
    const next = appendSparklinePoint(
      [
        { ts: newest.ts - SPARKLINE_WINDOW_SEC - 1, value: 0.1 },
        { ts: newest.ts - SPARKLINE_WINDOW_SEC, value: 0.2 },
      ],
      newest,
    )

    expect(next.map(point => point.value)).toEqual([0.2, 0.5])
  })

  it('点数超过上限时丢最旧的点', () => {
    // 10 秒一个点：15 分钟窗口内可容纳 60+ 个点，才能触发点数上限而不是先被时间窗裁掉
    const long = Array.from({ length: SPARKLINE_MAX_POINTS }, (_, index) => ({
      ts: 1_790_000_000 + index * 10,
      value: index,
    }))
    const next = appendSparklinePoint(long, { ts: 1_790_000_000 + SPARKLINE_MAX_POINTS * 10, value: 999 })

    expect(next).toHaveLength(SPARKLINE_MAX_POINTS)
    expect(next[0].value).toBe(1)  // 最旧的 0 被丢掉
    expect(next[next.length - 1].value).toBe(999)
  })

  it('纯函数：不修改入参数组', () => {
    const current = series(0.1, 0.2)
    appendSparklinePoint(current, { ts: 1_790_000_120, value: 0.7 })

    expect(current).toEqual(series(0.1, 0.2))
  })
})

describe('upsertLiveSparklinePoint（实时推进：末点恒等于当前值）', () => {
  it('实时桶更晚：追加新点（同 append 语义）', () => {
    const next = upsertLiveSparklinePoint(series(0.1, 0.2), { ts: 1_790_000_120, value: 0.7 })

    expect(next).toHaveLength(3)
    expect(next[next.length - 1]).toEqual({ ts: 1_790_000_120, value: 0.7 })
  })

  it('同一桶：替换末点值，不堆积', () => {
    const next = upsertLiveSparklinePoint(series(0.1, 0.2), { ts: 1_790_000_060, value: 0.9 })

    expect(next).toEqual([...series(0.1), { ts: 1_790_000_060, value: 0.9 }])
  })

  it('历史末桶比实时桶更靠右（桶边界 / 快照滞后）：仍以当前值作为末点，横轴单调', () => {
    const current = series(0.1, 0.2)
    const next = upsertLiveSparklinePoint(current, { ts: 1_790_000_000, value: 0.9 })

    expect(next).toHaveLength(2)
    expect(next[next.length - 1]).toEqual({ ts: 1_790_000_060, value: 0.9 })
  })

  it('空序列：实时点成为首个点（不足以画线时由调用方按点数判定不渲染）', () => {
    expect(upsertLiveSparklinePoint([], { ts: 1_790_000_000, value: 0.3 }))
      .toEqual([{ ts: 1_790_000_000, value: 0.3 }])
  })

  it('非法值不写入', () => {
    const current = series(0.1, 0.2)
    expect(upsertLiveSparklinePoint(current, { ts: Number.NaN, value: 0.3 })).toBe(current)
    expect(upsertLiveSparklinePoint(current, { ts: 1_790_000_120, value: Number.NaN })).toBe(current)
  })
})

describe('sparklineGeometry（纯几何：SVG viewBox 0 0 100 24）', () => {
  it('少于两个点时返回 null（只有一个点画不出趋势，不渲染）', () => {
    expect(sparklineGeometry([])).toBeNull()
    expect(sparklineGeometry(series(0.5))).toBeNull()
  })

  it('等值序列画中线（最小 == 最大时不得除零）', () => {
    const geometry = sparklineGeometry(series(0.5, 0.5, 0.5))

    expect(geometry).not.toBeNull()
    expect(geometry?.points).toBe('0,12 50,12 100,12')
    expect(geometry?.lastValue).toBe(0.5)
  })

  it('上升序列末点 y 更小（SVG y 轴向下），末点值 = 最后一个数据点', () => {
    const geometry = sparklineGeometry(series(10, 20))

    // 值域 10→20 映射到 22（底部）→ 2（顶部）
    expect(geometry?.points).toBe('0,22 100,2')
    expect(geometry?.lastValue).toBe(20)
  })

  it('x 按时间比例而不是序号等距：混合桶距（历史 60s + 实时 15s）不被拉伸', () => {
    const geometry = sparklineGeometry([
      { ts: 0, value: 1 },
      { ts: 100, value: 2 },
      { ts: 900, value: 3 },
    ])

    // 100/900 → 11.11（若按序号会是 50）
    expect(geometry?.points).toBe('0,22 11.11,12 100,2')
  })

  it('时间跨度为零（重复时刻）：退回序号等距，不除零', () => {
    const geometry = sparklineGeometry([
      { ts: 5, value: 10 },
      { ts: 5, value: 20 },
    ])

    expect(geometry?.points).toBe('0,22 100,2')
  })

  it('过滤非有限值后不足两点同样返回 null', () => {
    const geometry = sparklineGeometry([
      { ts: 1, value: 0.1 },
      { ts: 2, value: Number.NaN },
      { ts: 3, value: Number.POSITIVE_INFINITY },
    ])

    expect(geometry).toBeNull()
  })
})

describe('sparklineWindow（#142：时间窗口径如实）', () => {
  const TO = 1_790_000_000

  /** 60 秒桶的历史窗口元信息（完整 15 分钟，首桶在 TO-840） */
  function fullHistory(): SparklineHistoryWindow {
    return {
      from: TO - SPARKLINE_WINDOW_SEC,
      to: TO,
      truncated: false,
      intervalSec: 60,
      firstBucketTs: TO - 840,
    }
  }

  it('生效 from/to 足满窗口：称完整覆盖（15 分钟）', () => {
    const window = sparklineWindow(
      [{ ts: TO - 840, value: 0.1 }, { ts: TO - 60, value: 0.2 }],
      fullHistory(),
    )

    expect(window).toEqual({ coveredSec: SPARKLINE_WINDOW_SEC, truncated: false, stale: false })
    expect(sparklineWindowComplete(window)).toBe(true)
  })

  it('历史范围内无桶（interval_sec=null）：按实时点跨度标注，不冒充 15 分钟', () => {
    const window = sparklineWindow(
      [{ ts: TO - 30, value: 0.1 }, { ts: TO - 15, value: 0.2 }],
      { ...fullHistory(), intervalSec: null },
    )

    expect(window.coveredSec).toBe(15)
    expect(sparklineWindowComplete(window)).toBe(false)
    expect(window.truncated).toBe(false)
  })

  it('max_points 截断：按生效 from/to（from 已前移为首个返回桶）标注实际覆盖', () => {
    const window = sparklineWindow(
      [{ ts: TO - 295, value: 0.1 }, { ts: TO - 5, value: 0.2 }],
      { from: TO - 295, to: TO, truncated: true, intervalSec: 5, firstBucketTs: TO - 295 },
    )

    expect(window.coveredSec).toBe(295)
    expect(sparklineWindowComplete(window)).toBe(false)
    expect(window.truncated).toBe(true)
  })

  it('历史刚启用（窗口内只有后半段桶）：按首个桶起点标注实际覆盖', () => {
    const window = sparklineWindow(
      [{ ts: TO - 300, value: 0.1 }, { ts: TO - 60, value: 0.2 }],
      { ...fullHistory(), firstBucketTs: TO - 300 },
    )

    expect(window.coveredSec).toBe(300)
    expect(sparklineWindowComplete(window)).toBe(false)
  })

  it('实时点已推进到历史终点之后：覆盖按实际数据末端计（不出现「最近 0 秒」）', () => {
    const window = sparklineWindow(
      [
        { ts: TO - 840, value: 0.1 },
        { ts: TO - 60, value: 0.2 },
        { ts: TO + 120, value: 0.3 },
      ],
      fullHistory(),
    )

    expect(window.coveredSec).toBe(SPARKLINE_WINDOW_SEC + 120)
    expect(sparklineWindowComplete(window)).toBe(true)
  })

  it('只有实时点时，跨度攒满 15 分钟同样算完整覆盖', () => {
    const window = sparklineWindow(
      [{ ts: TO - SPARKLINE_WINDOW_SEC, value: 0.1 }, { ts: TO, value: 0.2 }],
      { ...fullHistory(), intervalSec: null },
    )

    expect(window.coveredSec).toBe(SPARKLINE_WINDOW_SEC)
    expect(sparklineWindowComplete(window)).toBe(true)
  })

  it('实时尾巴 + 迟到首桶：首点按精确时刻计，不套桶宽容差（840 秒不得说成 15 分钟）', () => {
    // 历史刚启用：首桶在 TO-300；首点 TO-840 是从上一轮序列保留的实时尾巴（距 from 仅 60 秒，
    // 若套桶宽容差会被当成 from，实际只覆盖 840 秒）
    const window = sparklineWindow(
      [
        { ts: TO - 840, value: 0.1 },
        { ts: TO - 300, value: 0.2 },
        { ts: TO - 60, value: 0.3 },
      ],
      { ...fullHistory(), firstBucketTs: TO - 300 },
    )

    expect(window.coveredSec).toBe(SPARKLINE_WINDOW_SEC - 60)
    expect(sparklineWindowComplete(window)).toBe(false)
  })

  it('截断 + 补拉保留的更早点把覆盖拉满：窗口算完整（#145：截断注不得与满窗标注意义重叠）', () => {
    // 上一轮序列在历史覆盖段之前保留了更早的点（实时尾巴或记录间隔变更后残留的旧桶），
    // 合并后跨度正好 15 分钟。截断只说明历史段被 max_points 前移，不代表可见曲线不满窗。
    const window = sparklineWindow(
      [{ ts: TO - SPARKLINE_WINDOW_SEC, value: 0.1 }, { ts: TO - 5, value: 0.2 }],
      { from: TO - 295, to: TO, truncated: true, intervalSec: 5, firstBucketTs: TO - 295 },
    )

    expect(window).toEqual({ coveredSec: SPARKLINE_WINDOW_SEC, truncated: true, stale: false })
    expect(sparklineWindowComplete(window)).toBe(true)
  })

  it('stale 原样透传：保留上一轮序列时标题可提示未刷新', () => {
    const window = sparklineWindow([{ ts: TO - 60, value: 0.1 }, { ts: TO, value: 0.2 }], fullHistory(), true)

    expect(window.stale).toBe(true)
    expect(window.truncated).toBe(false)
  })

  it('无点（组件不渲染）：coveredSec 为 null、不算完整覆盖', () => {
    expect(sparklineWindow([], null)).toEqual({ coveredSec: null, truncated: false, stale: false })
    expect(sparklineWindowComplete(sparklineWindow([], null))).toBe(false)
  })
})

describe('sparklineTitle / sparklineAriaLabel（#142：窗口文案如实）', () => {
  const COMPLETE: SparklineWindow = { coveredSec: SPARKLINE_WINDOW_SEC, truncated: false, stale: false }
  const TRUNCATED: SparklineWindow = { coveredSec: 300, truncated: true, stale: false }
  const SHORT: SparklineWindow = { coveredSec: 45, truncated: false, stale: false }

  it('完整覆盖：沿用「最近 15 分钟」文案（与 #127 一致）', () => {
    expect(sparklineTitle('CPU 使用率', COMPLETE)).toBe('CPU 使用率 · 最近 15 分钟（点击查看历史）')
  })

  it('实际覆盖不足 15 分钟：标注实际时长，不出现「最近 15 分钟」', () => {
    const title = sparklineTitle('CPU 使用率', SHORT)

    expect(title).toContain('最近 45 秒')
    expect(title).not.toContain('最近 15 分钟')
    expect(title).toContain('点击查看历史')
  })

  it('覆盖 14 分 59 秒仍不得说成 15 分钟（分钟向下取整，899 秒 → 14 分钟）', () => {
    for (const coveredSec of [870, 899]) {
      const title = sparklineTitle('CPU 使用率', { coveredSec, truncated: true, stale: false })

      expect(title).toContain('最近 14 分钟')
      expect(title).not.toContain('最近 15 分钟')
    }
  })

  it('无点（组件不渲染）时文案是占位而不是编一个时长', () => {
    const empty = sparklineWindow([], null)

    expect(sparklineWindowText(empty)).toBe('时间窗未知')
    expect(sparklineTitle('CPU 使用率', empty)).toContain('时间窗未知')
  })

  it('点数上限截断：与历史视图同口径（实际覆盖 + 仅显示最近一段）', () => {
    const title = sparklineTitle('CPU 使用率', TRUNCATED)

    expect(title).toContain('最近 5 分钟')
    expect(title).toContain('点数超限，仅显示最近一段')
    expect(title).not.toContain('最近 15 分钟')
  })

  it('满窗时抑制截断注（#145：满窗陈述优先，不同时出现两个窗口定性）', () => {
    // 补拉合并保留的更早点把实际覆盖拉满：曲线确实覆盖整窗，「仅显示最近一段」不再成立，
    // 与「最近 15 分钟」并存会自相矛盾
    const truncatedComplete: SparklineWindow = {
      coveredSec: SPARKLINE_WINDOW_SEC,
      truncated: true,
      stale: false,
    }

    expect(sparklineTitle('CPU 使用率', truncatedComplete)).toBe('CPU 使用率 · 最近 15 分钟（点击查看历史）')
    expect(sparklineAriaLabel('CPU 使用率', truncatedComplete))
      .toBe('CPU 使用率 最近 15 分钟趋势，查看历史')
  })

  it('截断注只在未满窗时出现；未刷新与满窗无关，始终保留（#145 组合口径）', () => {
    const titleComplete = sparklineTitle('CPU 使用率', {
      coveredSec: SPARKLINE_WINDOW_SEC,
      truncated: true,
      stale: false,
    })
    const titleShort = sparklineTitle('CPU 使用率', {
      coveredSec: SPARKLINE_WINDOW_SEC - 1,
      truncated: true,
      stale: false,
    })
    const titleCompleteStale = sparklineTitle('CPU 使用率', {
      coveredSec: SPARKLINE_WINDOW_SEC,
      truncated: true,
      stale: true,
    })

    expect(titleComplete).not.toContain('点数超限')
    expect(titleShort).toContain('点数超限，仅显示最近一段')
    expect(titleCompleteStale).toContain('历史未刷新')
    expect(titleCompleteStale).not.toContain('点数超限')
  })

  it('历史未刷新：title 与 aria-label 都带限定说明', () => {
    const stale: SparklineWindow = { ...COMPLETE, stale: true }

    expect(sparklineTitle('内存使用率', stale)).toContain('历史未刷新')
    expect(sparklineAriaLabel('内存使用率', stale)).toContain('历史未刷新')
  })

  it('aria-label 保留指标名与趋势语义（无障碍文案不含视觉细节）', () => {
    expect(sparklineAriaLabel('内存使用率', COMPLETE)).toBe('内存使用率 最近 15 分钟趋势，查看历史')
  })
})

describe('mergeSparklinePoints（#142：补拉与上一轮实时序列合并）', () => {
  it('历史范围内无桶：保留上一轮实时序列（历史刚启用时不清空已攒下的趋势）', () => {
    const previous = [
      { ts: 1_790_000_000, value: 0.1 },
      { ts: 1_790_000_015, value: 0.2 },
    ]

    expect(mergeSparklinePoints([], previous)).toEqual(previous)
  })

  it('历史覆盖的时段以历史为准：只保留历史末桶之后的实时点', () => {
    const history = [
      { ts: 1_790_000_000, value: 0.5 },
      { ts: 1_790_000_060, value: 0.6 },
    ]
    const previous = [
      { ts: 1_790_000_030, value: 0.99 },
      { ts: 1_790_000_075, value: 0.7 },
    ]

    expect(mergeSparklinePoints(history, previous)).toEqual([
      { ts: 1_790_000_000, value: 0.5 },
      { ts: 1_790_000_060, value: 0.6 },
      { ts: 1_790_000_075, value: 0.7 },
    ])
  })

  it('历史只覆盖窗口后半段：前段实时尾巴与后段实时点都保留（补拉不缩短曲线）', () => {
    const history = [
      { ts: 1_790_000_600, value: 0.5 },
      { ts: 1_790_000_660, value: 0.6 },
    ]
    const previous = [
      { ts: 1_790_000_000, value: 0.1 },  // 早于历史首桶：实时尾巴，保留
      { ts: 1_790_000_300, value: 0.2 },  // 早于历史首桶：实时尾巴，保留
      { ts: 1_790_000_630, value: 0.99 }, // 落在历史覆盖段内：以历史为准，丢弃
      { ts: 1_790_000_720, value: 0.7 },  // 晚于历史末桶：实时点，保留
    ]

    expect(mergeSparklinePoints(history, previous)).toEqual([
      { ts: 1_790_000_000, value: 0.1 },
      { ts: 1_790_000_300, value: 0.2 },
      { ts: 1_790_000_600, value: 0.5 },
      { ts: 1_790_000_660, value: 0.6 },
      { ts: 1_790_000_720, value: 0.7 },
    ])
  })

  it('点数超过上限时丢最旧的点（与实时推进同一收敛口径）', () => {
    const history: SparklinePoint[] = Array.from({ length: 60 }, (_, index) => ({
      ts: 1_790_000_000 + index * 10,
      value: index,
    }))
    const live: SparklinePoint[] = Array.from({ length: 20 }, (_, index) => ({
      ts: 1_790_000_600 + index * 10,
      value: 100 + index,
    }))

    const merged = mergeSparklinePoints(history, live)

    expect(merged).toHaveLength(SPARKLINE_MAX_POINTS)
    // 丢掉的是最旧的 20 个历史点（ts 1_790_000_000–1_790_000_190），首点为原第 20 个历史点
    expect(merged[0]).toEqual(history[20])
    expect(merged[merged.length - 1]).toEqual(live[live.length - 1])
  })

  it('纯函数：不修改入参', () => {
    const history = [{ ts: 1_790_000_000, value: 0.5 }]
    const previous = [{ ts: 1_790_000_060, value: 0.6 }]
    mergeSparklinePoints(history, previous)

    expect(history).toEqual([{ ts: 1_790_000_000, value: 0.5 }])
    expect(previous).toEqual([{ ts: 1_790_000_060, value: 0.6 }])
  })
})
