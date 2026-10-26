import { describe, it, expect, vi, beforeEach } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'
import {
  SYSMON_TREND_METRIC_KEYS,
  trendMetricLabel,
  useSysmonTrendStore,
} from '../sysmonTrend'
import { SYSMON_HISTORY_METRICS } from '../sysmonHistory'
import { useSysmonStore } from '../sysmon'
import { sysmonApi } from '@/api/sysmon'
import { SPARKLINE_BUCKET_SEC, SPARKLINE_MAX_POINTS, SPARKLINE_WINDOW_SEC } from '@/composables/sysmonSparkline'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'
import type { SysmonHistoryAvailable, SysmonHistoryResponse } from '@/types/api'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getConfig: vi.fn(), getHistory: vi.fn() },
}))

/** 夹具快照的固定时刻（`makeSysmonSnapshot` 的 timestamp）；测试把墙钟对齐到它，避免「历史点比快照新」 */
const FIXTURE_SEC = Math.floor(Date.UTC(2026, 9, 21, 15, 0, 0) / 1000)
/** 当前墙钟秒（比夹具快照晚 3 秒：历史最后一点 = NOW - 60，实时点 = FIXTURE，仍在历史之后） */
const NOW_SEC = FIXTURE_SEC + 3

/** 序列末点的值（none 时为 undefined） */
function lastValue(points: { value: number }[]): number | undefined {
  return points.length > 0 ? points[points.length - 1].value : undefined
}

/** 历史可用响应：一条序列，按 metric 回显（点值 avg 各不相同便于断言） */
function makeAvailable(metric: string, averages: number[] = [0.2, 0.3]): SysmonHistoryAvailable {
  const base = NOW_SEC - averages.length * 60
  return {
    available: true,
    level: 'raw',
    metric,
    from: base,
    to: base + averages.length * 60,
    interval_sec: 60,
    truncated: false,
    series: [{
      dimension: '',
      points: averages.map((avg, index) => ({
        ts: base + index * 60, min: avg, max: avg, avg, last: avg, n: 1,
      })),
    }],
    marks: [],
    gaps: [],
  }
}

/** 所有历史查询都回可用（按请求 metric 回显，2 个点） */
function mockAllAvailable(): void {
  vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) =>
    Promise.resolve(makeAvailable(params.metric ?? 'cpu.usage_ratio')))
}

async function initTrend(): Promise<ReturnType<typeof useSysmonTrendStore>> {
  const trend = useSysmonTrendStore()
  await trend.init()
  return trend
}

describe('useSysmonTrendStore：历史补齐（最近 15 分钟 / max_points=60）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    // 墙钟与夹具快照对齐（夹具 timestamp 是固定时刻，真实 Date.now 会晚几天 → 实时点会被判成旧点）
    vi.spyOn(Date, 'now').mockReturnValue(NOW_SEC * 1000)
  })

  it('趋势指标键都是历史视图已注册的标量键（子集守卫：打错键会在契约一致性上暴露）', () => {
    const registered = new Set(SYSMON_HISTORY_METRICS.map(metric => metric.key))
    for (const key of SYSMON_TREND_METRIC_KEYS) {
      expect(registered.has(key), `趋势指标键未在历史视图注册: ${key}`).toBe(true)
    }
  })

  it('sparkline 只覆盖 5 个标量指标（维度指标第一版不做）', () => {
    expect([...SYSMON_TREND_METRIC_KEYS]).toEqual([
      'cpu.usage_ratio',
      'cpu.load_average.one',
      'memory.used_ratio',
      'swap.used_ratio',
      'shm_dir.total_bytes',
    ])
  })

  it('init 为每个标量指标发一次历史查询：最近 15 分钟 + max_points=60，不新增后端接口', async () => {
    mockAllAvailable()
    await initTrend()

    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(SYSMON_TREND_METRIC_KEYS.length)
    for (const key of SYSMON_TREND_METRIC_KEYS) {
      const params = vi.mocked(sysmonApi.getHistory).mock.calls
        .map(([arg]) => arg)
        .find(arg => arg?.metric === key)
      expect(params).toBeDefined()
      // 时间窗：最近 15 分钟（to 为当前墙钟秒）
      expect((params?.to ?? 0) - (params?.from ?? 0)).toBe(SPARKLINE_WINDOW_SEC)
      expect(Math.abs((params?.to ?? 0) - NOW_SEC)).toBeLessThanOrEqual(1)
      expect(params?.max_points).toBe(SPARKLINE_MAX_POINTS)
    }
  })

  it('历史可用：序列以历史桶均值补齐（不渲染任何错误提示）', async () => {
    mockAllAvailable()
    const trend = await initTrend()

    expect(trend.seriesOf('cpu.usage_ratio').map(point => point.value)).toEqual([0.2, 0.3])
  })

  it('再次 init 重新补拉（历史库可能在运行期才建立，一次失败不应让整个会话失去 sparkline）', async () => {
    mockAllAvailable()
    const trend = await initTrend()
    await trend.init()

    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(SYSMON_TREND_METRIC_KEYS.length * 2)
  })

  it('并发 init 共享同一 in-flight 请求：第二个调用真正等待而不是提前 resolve', async () => {
    // 5 条历史请求都挂起，验证两个调用拿到同一个 promise，且它在请求完成前不会 resolve
    let resolveHistory: ((value: SysmonHistoryResponse) => void) | undefined
    vi.mocked(sysmonApi.getHistory).mockReturnValue(
      new Promise<SysmonHistoryResponse>((resolve) => { resolveHistory = resolve }),
    )
    const trend = useSysmonTrendStore()
    const first = trend.init()
    const second = trend.init()
    let secondSettled = false
    void second.then(() => { secondSettled = true })

    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(SYSMON_TREND_METRIC_KEYS.length)
    // 请求尚未返回：第二个调用不得提前 resolve（复用的是同一次请求，不是空 promise）
    await Promise.resolve()
    expect(secondSettled).toBe(false)

    resolveHistory?.(makeAvailable('cpu.usage_ratio'))
    await Promise.all([first, second])
    expect(secondSettled).toBe(true)
    expect(trend.seriesOf('cpu.usage_ratio')).toHaveLength(2)
  })

  it('未注册的指标键回退显示键本身（不静默空标题）', () => {
    expect(trendMetricLabel('memory.used_ratio')).toBe('内存使用率')
    expect(trendMetricLabel('not.registered.metric')).toBe('not.registered.metric')
  })
})

describe('useSysmonTrendStore：历史不可用时静默降级', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_SEC * 1000)
  })

  it('available=false（历史被关闭 / 未启用）：不产生序列，后续快照也不追加', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue({ available: false, reason: 'not_found' })
    const trend = await initTrend()

    expect(trend.seriesOf('cpu.usage_ratio')).toEqual([])

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.87 }))
    expect(trend.seriesOf('cpu.usage_ratio')).toEqual([])
  })

  it('HTTP 失败：静默（不抛错、不保留错误状态），数值照常显示', async () => {
    vi.mocked(sysmonApi.getHistory).mockRejectedValue(new Error('HTTP 500'))
    const trend = await initTrend()

    expect(trend.seriesOf('memory.used_ratio')).toEqual([])
  })
})

describe('useSysmonTrendStore：WS 增量推进（末点 = 当前实时值）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_SEC * 1000)
  })

  it('快照推送追加一个点，末点与 store 的当前值相等（与旁边数字同源）', async () => {
    mockAllAvailable()
    const trend = await initTrend()

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.187 }))

    const points = trend.seriesOf('cpu.usage_ratio')
    expect(points.length).toBe(3)
    // 与监控页展示的数字取自同一个 computed：末点 == cpuUsageRatio
    expect(points[points.length - 1].value).toBe(useSysmonStore().cpuUsageRatio)
    expect(points[points.length - 1].value).toBe(0.187)
  })

  it('每个标量指标各取自己的实时值（负载 / 内存 / 交换 / 共享内存目录）', async () => {
    mockAllAvailable()
    const trend = await initTrend()
    const sysmon = useSysmonStore()

    sysmon.applySnapshot(makeSysmonSnapshot())
    sysmon.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.5 }))

    expect(lastValue(trend.seriesOf('cpu.load_average.one'))).toBe(sysmon.cpuLoadAverage?.one)
    expect(lastValue(trend.seriesOf('memory.used_ratio'))).toBe(sysmon.memoryInfo?.used_ratio)
    expect(lastValue(trend.seriesOf('swap.used_ratio'))).toBe(sysmon.swapInfo?.used_ratio)
    expect(lastValue(trend.seriesOf('shm_dir.total_bytes'))).toBe(sysmon.shmDirInfo?.total_bytes)
  })

  it('当前值不可用（null）时不追加，也不用 0 冒充', async () => {
    mockAllAvailable()
    const trend = await initTrend()
    const before = trend.seriesOf('cpu.usage_ratio').length

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'collect_failed' } },
    }))

    expect(trend.seriesOf('cpu.usage_ratio')).toHaveLength(before)
    expect(trend.seriesOf('cpu.usage_ratio').some(point => point.value === 0)).toBe(false)
  })

  it('同一秒内多次快照只替换末点，不堆积（推送间隔 < 1 秒）', async () => {
    mockAllAvailable()
    const trend = await initTrend()
    const snapshot = makeSysmonSnapshot({ cpuUsageRatio: 0.1 })
    const sysmon = useSysmonStore()

    sysmon.applySnapshot(snapshot)
    const firstCount = trend.seriesOf('cpu.usage_ratio').length
    sysmon.applySnapshot({ ...snapshot, cpu: { ...snapshot.cpu, total: { usage_ratio: 0.9 } } })

    expect(trend.seriesOf('cpu.usage_ratio')).toHaveLength(firstCount)
    expect(lastValue(trend.seriesOf('cpu.usage_ratio'))).toBe(0.9)
  })

  it('实时推送按 15 秒桶推进：15 分钟内点数不超过上限，时间窗不被点数上限压缩', async () => {
    mockAllAvailable()
    const trend = await initTrend()
    const sysmon = useSysmonStore()
    const base = makeSysmonSnapshot({ cpuUsageRatio: 0.1 })

    // 模拟 15 分钟、每 5 秒一次快照推送（推送间隔远小于桶宽）
    for (let index = 0; index <= 180; index++) {
      sysmon.applySnapshot({
        ...base,
        timestamp: (FIXTURE_SEC + index * 5) * 1000,
        cpu: { ...base.cpu, total: { usage_ratio: index / 180 } },
      })
    }

    const points = trend.seriesOf('cpu.usage_ratio')
    expect(points.length).toBeLessThanOrEqual(SPARKLINE_MAX_POINTS)
    // 覆盖仍接近最近 15 分钟（末期历史点已被窗口淘汰）：60 个 15 秒桶 = 900 秒
    expect(points[points.length - 1].ts - points[0].ts).toBe(SPARKLINE_WINDOW_SEC - SPARKLINE_BUCKET_SEC)
    // 末点仍等于当前实时值（同桶内只更新值，不改变「末点 == 当前值」）
    expect(points[points.length - 1].value).toBe(1)
  })

  it('历史末桶比实时桶更靠右（桶边界 / 快照时间戳滞后）：末点仍等于当前值', async () => {
    // 历史桶起点 = FIXTURE_SEC + 60，比快照时间戳（FIXTURE_SEC）晚 1 分钟：
    // 若按「更旧的点忽略」，末点会落在历史值 0.3 上、与旁边的当前数字不一致（本票 AC 要求同源）
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const metric = params.metric ?? 'cpu.usage_ratio'
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric,
        from: FIXTURE_SEC,
        to: FIXTURE_SEC + 120,
        interval_sec: 60,
        truncated: false,
        series: [{
          dimension: '',
          points: [
            { ts: FIXTURE_SEC, min: 0.2, max: 0.2, avg: 0.2, last: 0.2, n: 1 },
            { ts: FIXTURE_SEC + 60, min: 0.3, max: 0.3, avg: 0.3, last: 0.3, n: 1 },
          ],
        }],
        marks: [],
        gaps: [],
      })
    })
    const sysmon = useSysmonStore()
    sysmon.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.42 }))

    const trend = await initTrend()

    const points = trend.seriesOf('cpu.usage_ratio')
    expect(points[points.length - 1].value).toBe(0.42)
    expect(points[points.length - 1].value).toBe(sysmon.cpuUsageRatio)
  })

  it('历史补齐完成时立即把当前快照值接到末点（不等下一次推送）', async () => {
    const sysmon = useSysmonStore()
    sysmon.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.42 }))
    // 历史响应在快照之后才返回
    mockAllAvailable()
    const trend = await initTrend()

    expect(lastValue(trend.seriesOf('cpu.usage_ratio'))).toBe(0.42)
  })
})
