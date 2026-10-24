import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  SYSMON_HISTORY_LEVELS,
  SYSMON_HISTORY_METRICS,
  SYSMON_HISTORY_RANGES,
  historyUnavailableText,
  useSysmonHistoryStore,
} from '../sysmonHistory'
import { sysmonApi } from '@/api/sysmon'
import type { SysmonHistoryAvailable, SysmonHistoryResponse } from '@/types/api'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getHistory: vi.fn() },
}))

/** 构造一条有 n 个点的可用响应（min/max/avg/last 各不相同便于断言包络） */
function makeAvailable(overrides: Partial<SysmonHistoryAvailable> = {}): SysmonHistoryAvailable {
  return {
    available: true,
    level: 'raw',
    metric: 'cpu.usage_ratio',
    from: 1_790_000_000,
    to: 1_790_000_600,
    interval_sec: 60,
    truncated: false,
    series: [
      {
        dimension: '',
        points: [
          { ts: 1_790_000_000, min: 0.1, max: 0.5, avg: 0.3, last: 0.2, n: 5 },
          { ts: 1_790_000_060, min: 0.2, max: 0.9, avg: 0.4, last: 0.6, n: 5 },
        ],
      },
    ],
    marks: [],
    gaps: [],
    ...overrides,
  }
}

function makeUnavailable(
  reason: 'not_found' | 'schema_mismatch' | 'open_failed',
): SysmonHistoryResponse {
  return { available: false, reason }
}

describe('useSysmonHistoryStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('load 发起查询并进入 ready（有数据）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const store = useSysmonHistoryStore()

    await store.load()
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(1)
    expect(store.state).toBe('ready')
    expect(store.available).toBe(true)
    expect(store.points).toHaveLength(2)
    expect(store.effectiveLevel).toBe('raw')
    expect(store.intervalSec).toBe(60)
    expect(store.truncated).toBe(false)
  })

  it('空序列进入 empty 而不是 ready（「无数据」与「不可用」是不同事实）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(
      makeAvailable({ series: [{ dimension: '', points: [] }], interval_sec: null }),
    )
    const store = useSysmonHistoryStore()
    await store.load()
    expect(store.state).toBe('empty')
    expect(store.available).toBe(true)
    expect(store.points).toHaveLength(0)
    expect(store.intervalSec).toBeNull()
  })

  it('可用但 series 缺失也按 empty 处理（防御异常响应）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(
      makeAvailable({ series: [] }),
    )
    const store = useSysmonHistoryStore()
    await store.load()
    expect(store.state).toBe('empty')
  })

  it.each(['not_found', 'schema_mismatch', 'open_failed'] as const)(
    '降级为 unavailable 并保留原因：%s',
    async (reason) => {
      vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeUnavailable(reason))
      const store = useSysmonHistoryStore()
      await store.load()
      // 降级不是错误：state 是 unavailable，error 保持 null
      expect(store.state).toBe('unavailable')
      expect(store.available).toBe(false)
      expect(store.unavailableReason).toBe(reason)
      expect(store.error).toBeNull()
      // 不可用时绝不产出伪造数据
      expect(store.points).toHaveLength(0)
      expect(store.marks).toHaveLength(0)
      expect(store.gaps).toHaveLength(0)
      expect(store.intervalSec).toBeNull()
      expect(store.effectiveRange).toBeNull()
    },
  )

  it('HTTP 失败记入 error 并回到 idle（可重试）', async () => {
    vi.mocked(sysmonApi.getHistory).mockRejectedValue(new Error('HTTP 500'))
    const store = useSysmonHistoryStore()
    await store.load()
    expect(store.error).toBe('HTTP 500')
    expect(store.state).toBe('idle')
    expect(store.available).toBe(false)
  })

  it('查询参数携带默认范围与粒度（to = now，from = now - 6h）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const store = useSysmonHistoryStore()
    const before = Math.floor(Date.now() / 1000)
    await store.load()
    const after = Math.floor(Date.now() / 1000)

    const params = vi.mocked(sysmonApi.getHistory).mock.calls[0][0]
    expect(params?.metric).toBe(SYSMON_HISTORY_METRICS[0].key)
    expect(params?.level).toBe('auto')
    expect(params?.to).toBeGreaterThanOrEqual(before)
    expect(params?.to).toBeLessThanOrEqual(after)
    expect(params?.from).toBe((params?.to ?? 0) - SYSMON_HISTORY_RANGES[2].seconds)
    // 标量指标不带 dimension（后端会回 400）
    expect(params?.dimension).toBeUndefined()
  })

  it('setMetric / setRange / setLevel 变更后重新查询', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const store = useSysmonHistoryStore()
    await store.load()
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(1)

    await store.setMetric(SYSMON_HISTORY_METRICS[1].key)
    expect(store.metricKey).toBe(SYSMON_HISTORY_METRICS[1].key)
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(2)

    await store.setRange(SYSMON_HISTORY_RANGES[0].seconds)
    expect(store.rangeSeconds).toBe(SYSMON_HISTORY_RANGES[0].seconds)
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(3)

    await store.setLevel('minute')
    expect(store.level).toBe('minute')
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(4)
  })

  it('值未变化时不重复查询（避免无意义请求）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const store = useSysmonHistoryStore()
    await store.load()
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(1)

    await store.setMetric(store.metricKey)
    await store.setRange(store.rangeSeconds)
    await store.setLevel(store.level)
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(1)
  })

  it('丢弃过期响应（快速切换时前一次请求后返回不覆盖新结果）', async () => {
    let resolveFirst: ((value: SysmonHistoryResponse) => void) | undefined
    const first = new Promise<SysmonHistoryResponse>((resolve) => { resolveFirst = resolve })
    vi.mocked(sysmonApi.getHistory)
      .mockReturnValueOnce(first)
      .mockResolvedValueOnce(makeAvailable({ metric: 'memory.used_ratio' }))

    const store = useSysmonHistoryStore()
    const firstLoad = store.load()
    await store.setMetric('memory.used_ratio')  // 第二次先完成
    expect(store.state).toBe('ready')

    // 第一次（慢）此时才返回：不得覆盖已生效的新结果
    resolveFirst?.(makeAvailable({ series: [{ dimension: '', points: [] }] }))
    await firstLoad
    expect(store.state).toBe('ready')
    expect(store.points).toHaveLength(2)
  })

  it('标记与缺口透传，截断标志透传', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(
      makeAvailable({
        truncated: true,
        marks: [{ ts: 1_790_000_120, kind: 'restart', detail: 'recover' }],
        gaps: [{ from: 1_790_000_120, to: 1_790_000_600, kind: 'restart' }],
      }),
    )
    const store = useSysmonHistoryStore()
    await store.load()
    expect(store.truncated).toBe(true)
    expect(store.marks).toHaveLength(1)
    expect(store.marks[0].kind).toBe('restart')
    expect(store.gaps).toHaveLength(1)
    expect(store.gaps[0].kind).toBe('restart')
  })

  it('历史 store 不订阅 WS：切换查询不影响实时快照（结构保证）', async () => {
    // 历史 store 只经 REST 取数；它没有任何 WS handler 注册（结构断言：
    // 该 store 的 API 面只有 load/set* 与只读视图，没有 apply* 之类的 WS 入口）。
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const store = useSysmonHistoryStore()
    const wsEntryPoints = Object.keys(store).filter(key =>
      key.startsWith('apply') || key.startsWith('on') || key.includes('snapshot'))
    expect(wsEntryPoints).toEqual([])
  })
})

describe('历史视图的选项常量', () => {
  it('指标键与契约（libs/sysmonstore 宽表逻辑键）一致', () => {
    // 后端对未注册的指标键回 400，因此这些键必须是真实存在的宽表逻辑键
    expect(SYSMON_HISTORY_METRICS.map(m => m.key)).toEqual([
      'cpu.usage_ratio',
      'cpu.load_average.one',
      'cpu.load_average.five',
      'cpu.load_average.fifteen',
      'cpu.context_switches_per_sec',
      'cpu.interrupts_per_sec',
      'memory.used_ratio',
      'memory.used_bytes',
      'memory.available_bytes',
      'swap.used_ratio',
      'shm_dir.total_bytes',
      'shm_dir.page_files',
    ])
    // 每个指标都必须声明展示形态（视图据 format 决定百分比/容量/数值）
    for (const metric of SYSMON_HISTORY_METRICS) {
      expect(['ratio', 'bytes', 'number']).toContain(metric.format)
      expect(metric.label.length).toBeGreaterThan(0)
    }
  })

  it('时间范围与粒度选项非空且粒度含 auto/raw/minute', () => {
    expect(SYSMON_HISTORY_RANGES.length).toBeGreaterThanOrEqual(3)
    for (const range of SYSMON_HISTORY_RANGES) {
      expect(range.seconds).toBeGreaterThan(0)
    }
    expect(SYSMON_HISTORY_LEVELS.map(l => l.value)).toEqual(['auto', 'raw', 'minute'])
  })

  it('不可用原因都有对应文案（三种降级路径不出现空文案）', () => {
    for (const reason of ['not_found', 'schema_mismatch', 'open_failed'] as const) {
      expect(historyUnavailableText(reason).length).toBeGreaterThan(0)
    }
  })
})
