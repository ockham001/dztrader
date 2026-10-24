import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonStore } from '@/stores/sysmon'
import { useSysmonHistoryStore } from '@/stores/sysmonHistory'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'
import type { SysmonHistoryAvailable, SysmonHistoryResponse } from '@/types/api'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getHistory: vi.fn() },
}))

// 图表组件在 jsdom 里无 canvas：用桩替换，测试聚焦「三态渲染」与「控件/摘要」，
// 图表内部的时间轴/包络属于 ECharts 的渲染逻辑，不在组件测试范围内。
const stubs = {
  Icon: { template: '<span class="icon" />' },
  SysmonHistoryChart: { template: '<div class="chart-stub" data-test="monitor-history-chart" />' },
}

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

async function mountView(): Promise<ReturnType<typeof mount>> {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return wrapper
}

/** 切到历史视图 */
async function openHistory(wrapper: ReturnType<typeof mount>): Promise<void> {
  await wrapper.find('[data-test="monitor-tab-history"]').trigger('click')
  await flushPromises()
}

describe('MonitorView 历史视图（#79 三态）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
  })

  it('有数据：渲染图表、摘要与控件', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
    const wrapper = await mountView()
    await openHistory(wrapper)

    expect(wrapper.find('[data-test="sysmon-history"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-history-chart"]').exists()).toBe(true)
    // 摘要如实回显生效粒度 / 桶宽 / 点数（不让用户猜 auto 选了哪一层）
    const summary = wrapper.find('[data-test="sysmon-history-summary"]').text()
    expect(summary).toContain('原始记录')
    expect(summary).toContain('60 秒')
    expect(summary).toContain('2 个点')
    // 三个查询控件都在
    expect(wrapper.find('[data-test="sysmon-history-metric"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-range"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-level"]').exists()).toBe(true)
    // 未截断、无异常时不显示那两个提示
    expect(wrapper.find('[data-test="sysmon-history-truncated"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-history-anomaly"]').exists()).toBe(false)
  })

  it('有数据但被点数上限截断：显示「仅显示最近一段」提示', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable({ truncated: true }))
    const wrapper = await mountView()
    await openHistory(wrapper)
    expect(wrapper.find('[data-test="sysmon-history-truncated"]').exists()).toBe(true)
  })

  it('有数据带缺口与时钟跳变：摘要显示异常计数', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(
      makeAvailable({
        gaps: [{ from: 1_790_000_120, to: 1_790_000_600, kind: 'restart' }],
        marks: [
          { ts: 1_790_000_120, kind: 'restart', detail: 'recover' },
          { ts: 1_790_000_300, kind: 'clock_jump', detail: '1500' },
        ],
      }),
    )
    const wrapper = await mountView()
    await openHistory(wrapper)
    const anomaly = wrapper.find('[data-test="sysmon-history-anomaly"]').text()
    expect(anomaly).toContain('1 处采集中断')
    expect(anomaly).toContain('1 处时钟跳变')
  })

  it('无数据：显示「该时间范围无数据」而不是「历史不可用」', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(
      makeAvailable({ series: [{ dimension: '', points: [] }], interval_sec: null }),
    )
    const wrapper = await mountView()
    await openHistory(wrapper)

    expect(wrapper.find('[data-test="sysmon-history-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-history-chart"]').exists()).toBe(false)
    // 明确写出「历史库可用」以区别于降级
    expect(wrapper.find('[data-test="sysmon-history-empty"]').text()).toContain('历史库可用')
  })

  it.each([
    ['not_found', '历史记录未启用或尚无数据'],
    ['schema_mismatch', '历史库版本与当前程序不符'],
    ['open_failed', '历史库无法读取'],
  ] as const)('历史不可用（%s）：显示降级文案，不渲染任何数据', async (reason, text) => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue({ available: false, reason })
    const wrapper = await mountView()
    await openHistory(wrapper)

    expect(wrapper.find('[data-test="sysmon-history-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-unavailable-reason"]').text()).toContain(text)
    // 降级时不渲染图表、也不显示「无数据」或截断提示（不伪造任何结论）
    expect(wrapper.find('[data-test="monitor-history-chart"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-history-empty"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-history-truncated"]').exists()).toBe(false)
  })

  it('HTTP 失败：显示可重试的错误态', async () => {
    vi.mocked(sysmonApi.getHistory).mockRejectedValue(new Error('HTTP 500'))
    const wrapper = await mountView()
    await openHistory(wrapper)
    expect(wrapper.find('[data-test="sysmon-history-error"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-error"]').text()).toContain('HTTP 500')
  })

  it('加载中：显示加载态（不显示任何结论）', async () => {
    let resolve: ((v: SysmonHistoryResponse) => void) | undefined
    vi.mocked(sysmonApi.getHistory).mockReturnValue(
      new Promise<SysmonHistoryResponse>((r) => { resolve = r }),
    )
    const wrapper = await mountView()
    await openHistory(wrapper)

    expect(wrapper.find('[data-test="sysmon-history-loading"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-history-empty"]').exists()).toBe(false)

    resolve?.(makeAvailable())
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-history-chart"]').exists()).toBe(true)
  })
})

describe('MonitorView 实时 / 历史视图切换（不影响 WS 与实时数据）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getHistory).mockResolvedValue(makeAvailable())
  })

  it('默认显示实时视图，历史视图不渲染', async () => {
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-cpu"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="sysmon-history"]').exists()).toBe(false)
  })

  it('切到历史视图再切回实时：实时快照数据仍在（store 未被卸载/重置）', async () => {
    const wrapper = await mountView()
    const sysmon = useSysmonStore()
    const cpuBefore = sysmon.cpuUsageRatio

    await openHistory(wrapper)
    expect(wrapper.find('[data-test="monitor-cpu"]').exists()).toBe(false)

    // 历史视图期间应到一条实时推送：applySnapshot 仍然有效（store 独立于视图挂载）
    sysmon.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.42 }))
    await flushPromises()

    await wrapper.find('[data-test="monitor-tab-realtime"]').trigger('click')
    await flushPromises()
    // 切回实时立刻是最新快照（不是切走前的旧值）——切换不影响实时数据流
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('42.0%')
    expect(cpuBefore).not.toBe(0.42)  // 切走前是另一份快照，确认读到的是新推送
  })

  it('切到历史不重建实时 store（同一 store 实例、已加载状态保持）', async () => {
    const wrapper = await mountView()
    const sysmon = useSysmonStore()
    expect(sysmon.loaded).toBe(true)

    await openHistory(wrapper)
    expect(sysmon.loaded).toBe(true)
    // 历史查询不触发实时快照的重新拉取（REST 只被调用一次，来自 onMounted 的 init）
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })

  it('切到历史时只发历史查询，不额外发实时请求', async () => {
    const wrapper = await mountView()
    expect(sysmonApi.getHistory).not.toHaveBeenCalled()

    await openHistory(wrapper)
    expect(sysmonApi.getHistory).toHaveBeenCalledTimes(1)
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })

  it('历史视图的实时徽章隐藏（避免「实时快照」标签出现在历史数据上）', async () => {
    const wrapper = await mountView()
    await openHistory(wrapper)
    expect(wrapper.text()).not.toContain('实时快照')
    expect(wrapper.find('[data-test="monitor-tab-history"]').attributes('aria-selected')).toBe('true')
  })

  it('历史 store 与实时 store 互不影响（实时推送不改变历史结果，反之亦然）', async () => {
    const wrapper = await mountView()
    await openHistory(wrapper)
    const sysmon = useSysmonStore()
    const history = useSysmonHistoryStore()
    const historyPointsBefore = history.points.map(p => p.max)

    // 实时推送到达：历史查询结果必须原样不变（两者数据源独立）
    sysmon.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.7 }))
    await flushPromises()
    expect(history.points.map(p => p.max)).toEqual(historyPointsBefore)
    expect(sysmon.cpuUsageRatio).toBe(0.7)

    // 反向：重新查历史不改变实时快照
    await history.load()
    await flushPromises()
    expect(sysmon.cpuUsageRatio).toBe(0.7)
  })
})
