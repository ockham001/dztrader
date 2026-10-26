import { describe, expect, it } from 'vitest'
import {
  SPARKLINE_MAX_POINTS,
  SPARKLINE_WINDOW_SEC,
  appendSparklinePoint,
  historySparklinePoints,
  sparklineGeometry,
  upsertLiveSparklinePoint,
  type SparklinePoint,
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
