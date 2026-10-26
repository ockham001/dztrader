import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonHistoryStore } from '@/stores/sysmonHistory'
import { useSysmonStore } from '@/stores/sysmon'
import { SYSMON_TREND_METRIC_KEYS } from '@/stores/sysmonTrend'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'
import type { SysmonHistoryAvailable } from '@/types/api'

// #127：内联趋势 / 卡片折叠 / 响应式布局。
// 本文件覆盖「实时视图的密度与跳转行为」；历史视图三态见 MonitorHistoryView.spec.ts，
// 数据渲染见 MonitorView.spec.ts（后者挂载后自动展开维度明细）。

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn(), getHistory: vi.fn() },
}))

// 图表组件在 jsdom 里无 canvas：用桩替换；本文件只验证「跳到了历史视图并预选指标」
const stubs = {
  Icon: { template: '<span class="icon" />' },
  SysmonHistoryChart: { template: '<div class="chart-stub" data-test="monitor-history-chart" />' },
}

/** 夹具快照的固定时刻（`makeSysmonSnapshot` 的 timestamp）；墙钟对齐到它，趋势点才落在同一条时间轴上 */
const FIXTURE_SEC = Math.floor(Date.UTC(2026, 9, 21, 15, 0, 0) / 1000)
const NOW_MS = (FIXTURE_SEC + 3) * 1000

/** 历史序列：按 metric 回显两条桶（最后一条桶时间戳落在实时点之前） */
function makeHistory(metric: string): SysmonHistoryAvailable {
  const base = FIXTURE_SEC - 120
  return {
    available: true,
    level: 'raw',
    metric,
    from: base,
    to: base + 120,
    interval_sec: 60,
    truncated: false,
    series: [{
      dimension: '',
      points: [
        { ts: base, min: 0.1, max: 0.1, avg: 0.1, last: 0.1, n: 1 },
        { ts: base + 60, min: 0.2, max: 0.2, avg: 0.2, last: 0.2, n: 1 },
      ],
    }],
    marks: [],
    gaps: [],
  }
}

function mockHistoryAvailable(): void {
  vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) =>
    Promise.resolve(makeHistory(params.metric ?? 'cpu.usage_ratio')))
}

async function mountView(
  snapshot = makeSysmonSnapshot({ cpuUsageRatio: 0.187, resourceData: true, sensorData: true }),
): Promise<VueWrapper> {
  vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return wrapper
}

/** 点击折叠按钮（#127 维度明细默认收起） */
async function toggle(wrapper: VueWrapper, testId: string): Promise<void> {
  await wrapper.find(`[data-test="${testId}"]`).trigger('click')
  await wrapper.vm.$nextTick()
}

describe('MonitorView 内联趋势（#127：sparkline 覆盖标量指标）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('5 个标量指标各有一条 sparkline，维度指标（温度 / 每核 / 每盘 / 每网卡）没有', async () => {
    const wrapper = await mountView()

    expect(wrapper.findAll('.sysmon-sparkline')).toHaveLength(5)
    for (const metric of SYSMON_TREND_METRIC_KEYS) {
      expect(vi.mocked(sysmonApi.getHistory).mock.calls.some(([params]) => params?.metric === metric)).toBe(true)
    }
    // 维度指标第一版不做 sparkline：温度卡片 / 磁盘 I/O / 网络卡片内都没有
    expect(wrapper.find('[data-test="monitor-temperature"] .sysmon-sparkline').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-disk-io"] .sysmon-sparkline').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-network"] .sysmon-sparkline').exists()).toBe(false)
  })

  it('sparkline 末点与旁边的实时数字同源同刷新时刻（末点 == 当前值）', async () => {
    const wrapper = await mountView()

    // 数据层：末点就是 store 的当前值（不是另一次采样）
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('data-last-value')).toBe('0.187')
    expect(wrapper.find('[data-test="monitor-sparkline-load"]').attributes('data-last-value')).toBe('0.42')
    expect(wrapper.find('[data-test="monitor-sparkline-memory"]').attributes('data-last-value')).toBe('0.5')
    expect(wrapper.find('[data-test="monitor-sparkline-swap"]').attributes('data-last-value')).toBe('0.25')
    expect(wrapper.find('[data-test="monitor-sparkline-shm"]').attributes('data-last-value')).toBe(String(720 * 1024 * 1024))
    // 展示层：旁边的数字是同一个值的格式化结果（18.7% ↔ 0.187、50.0% ↔ 0.5）
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
  })

  it('同一次快照推送后，sparkline 末点与旁边数字在同一 tick 内同步更新', async () => {
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('data-last-value')).toBe('0.187')

    // WS 推送到达：末点与数字必须同时变到新值（不是等下一次推送/两次采样）
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.93, resourceData: true, sensorData: true }))
    await wrapper.vm.$nextTick()

    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('93.0%')
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('data-last-value')).toBe('0.93')
  })

  it('历史库不可用 / 历史被关闭时 sparkline 静默不渲染，数值照常显示（不是错误提示）', async () => {
    vi.mocked(sysmonApi.getHistory).mockResolvedValue({ available: false, reason: 'not_found' })
    const wrapper = await mountView()

    expect(wrapper.findAll('.sysmon-sparkline')).toHaveLength(0)
    // 数值照常
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    // 不是错误提示：页面没有「历史不可用」之类的降级文案
    expect(wrapper.text()).not.toContain('历史不可用')
  })

  it('历史查询失败同样静默降级（HTTP 失败不弹错、不渲染）', async () => {
    vi.mocked(sysmonApi.getHistory).mockRejectedValue(new Error('HTTP 500'))
    const wrapper = await mountView()

    expect(wrapper.findAll('.sysmon-sparkline')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
  })

  it('历史可用但当前值不可用（首次采样）时不渲染 sparkline，也不画 0', async () => {
    const wrapper = await mountView(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'collect_failed' } },
    }))

    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').exists()).toBe(false)
  })
})

describe('MonitorView 内联趋势窗口口径（#142：按实际覆盖标注，不冒充 15 分钟）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('历史响应截断（record_interval < 15s）：标题标注生效覆盖 + 仅显示最近一段', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const to = Math.floor(NOW_MS / 1000)
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric: params.metric ?? 'cpu.usage_ratio',
        from: to - 300,
        to,
        interval_sec: 5,
        truncated: true,
        series: [{
          dimension: '',
          points: [
            { ts: to - 295, min: 0.2, max: 0.2, avg: 0.2, last: 0.2, n: 1 },
            { ts: to - 5, min: 0.3, max: 0.3, avg: 0.3, last: 0.3, n: 1 },
          ],
        }],
        marks: [],
        gaps: [],
      } satisfies SysmonHistoryAvailable)
    })
    const wrapper = await mountView()

    const root = wrapper.find('[data-test="monitor-sparkline-cpu"]')
    expect(root.attributes('title')).toContain('最近 5 分钟')
    expect(root.attributes('title')).toContain('点数超限，仅显示最近一段')
    expect(root.attributes('title')).not.toContain('最近 15 分钟')
    expect(root.attributes('data-window-truncated')).toBe('true')
  })

  it('历史可用但范围内无桶：两条实时快照后按实际跨度标注', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const from = Math.floor(NOW_MS / 1000) - 15 * 60
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric: params.metric ?? 'cpu.usage_ratio',
        from,
        to: Math.floor(NOW_MS / 1000),
        interval_sec: null,
        truncated: false,
        series: [{ dimension: '', points: [] }],
        marks: [],
        gaps: [],
      } satisfies SysmonHistoryAvailable)
    })
    const wrapper = await mountView()

    useSysmonStore().applySnapshot({
      ...makeSysmonSnapshot({ cpuUsageRatio: 0.42, resourceData: true, sensorData: true }),
      timestamp: (FIXTURE_SEC + 15) * 1000,
    })
    await wrapper.vm.$nextTick()

    const title = wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('title')
    expect(title).toContain('最近 15 秒')
    expect(title).not.toContain('最近 15 分钟')
  })

  it('单指标补拉降级：重新进入页面时保留上一轮曲线并标记历史未刷新', async () => {
    mockHistoryAvailable()
    const first = await mountView()
    expect(first.find('[data-test="monitor-sparkline-cpu"]').exists()).toBe(true)
    first.unmount()

    vi.mocked(sysmonApi.getHistory).mockRejectedValue(new Error('HTTP 500'))
    const second = await mountView()

    const root = second.find('[data-test="monitor-sparkline-cpu"]')
    expect(root.exists()).toBe(true)
    expect(root.attributes('title')).toContain('历史未刷新')
    expect(root.attributes('data-window-stale')).toBe('true')
  })
})

describe('MonitorView 跳转历史（#127：点击 sparkline / 数值预选指标）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('点击数值切到历史视图并预选该指标（查询由历史视图挂载时发起）', async () => {
    const wrapper = await mountView()
    const history = useSysmonHistoryStore()
    const before = vi.mocked(sysmonApi.getHistory).mock.calls.length

    await wrapper.find('[data-test="monitor-memory-value"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="sysmon-history"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-tab-history"]').attributes('aria-selected')).toBe('true')
    // 预选生效：store 指标键切到内存使用率，历史视图据此查询
    expect(history.metricKey).toBe('memory.used_ratio')
    const calls = vi.mocked(sysmonApi.getHistory).mock.calls
    expect(calls.length).toBe(before + 1)
    expect(calls[calls.length - 1][0]?.metric).toBe('memory.used_ratio')
  })

  it('点击 sparkline 同样跳转历史并预选（交换使用率）', async () => {
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-sparkline-swap"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="sysmon-history"]').exists()).toBe(true)
    expect(useSysmonHistoryStore().metricKey).toBe('swap.used_ratio')
  })
})

describe('MonitorView 卡片折叠（#127：维度明细默认收起）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('每核明细默认收起：只显示摘要（核数 + 最高使用率），点开展开后显示全部', async () => {
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(0)
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    const summary = wrapper.find('[data-test="monitor-cpu-detail-summary"]')
    expect(summary.exists()).toBe(true)
    // 夹具每核使用率 0.05 + index*0.031，最大在 15 号核：0.515
    expect(summary.text()).toBe('16 核 · 最高 51.5%')
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('false')

    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(16)
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('true')

    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(0)
  })

  it('每传感器明细默认收起：卡片显示最高温作为主数值，展开后按芯片分组显示全部', async () => {
    const wrapper = await mountView(makeSysmonSnapshot({
      sensorData: true,
      temperatures: [
        { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
        { sensor: 'coretemp#hwmon3:Core 0', chip: 'coretemp', label: 'Core 0', celsius: 41.5 },
        { sensor: 'acpitz_0#hwmon0:temp1', chip: 'acpitz_0', label: '', celsius: 27.8 },
      ],
    }))

    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)
    // 主数值 = 最高温度；关键数字 = 传感器 / 芯片数量（不伪造 0 °C）
    expect(wrapper.find('[data-test="monitor-temperature-max"]').text()).toBe('45.5 °C')
    // 主数值带归属（哪颗芯片 / 哪个通道），不丢可读命名口径
    expect(wrapper.find('[data-test="monitor-temperature-summary"]').text())
      .toBe('最高温度 · CPU · Package id 0 · 3 个传感器 · 2 个芯片')

    await toggle(wrapper, 'monitor-temperature-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(3)
    expect(wrapper.findAll('[data-test="monitor-temperature-group"]')).toHaveLength(2)
  })

  it('每盘七项指标默认收起：只显示盘数与最高利用率', async () => {
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(0)
    const summary = wrapper.find('[data-test="monitor-disk-io-summary"]')
    // 夹具两块盘利用率 0.35 / 0.72：摘要给出盘数与最高值
    expect(summary.text()).toBe('2 块盘 · 最高利用率 72.0%')

    await toggle(wrapper, 'monitor-disk-io-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(2)
  })

  it('每网卡十项指标默认收起：只显示网卡数', async () => {
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-network-summary"]').text()).toContain('1 块网卡')

    await toggle(wrapper, 'monitor-network-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(1)
  })

  it('可用性空态不藏在折叠后面（不可用 / 无数据事实照常可见）', async () => {
    const wrapper = await mountView(makeSysmonSnapshot({
      metrics: {
        'cpu.cores': { available: false, reason: 'no_permission' },
        disk_io: { available: false, reason: 'no_permission' },
        network: { available: false, reason: 'collect_failed' },
        temperature: { available: false, reason: 'no_sensor' },
      },
      diskIo: [],
      network: [],
    }))

    // 不需要展开就能看到「不可用」空态与原因
    expect(wrapper.find('[data-test="monitor-cores-empty"]').text()).toContain('no_permission')
    // 磁盘 I/O 的 no_permission 给出可操作文案（不是裸 reason，与既有行为一致）
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').text()).toContain('磁盘性能计数器不可用')
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    // 没有数据的区块不出现折叠按钮（没有可展开的明细）
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-disk-io-detail-toggle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-network-detail-toggle"]').exists()).toBe(false)
  })

  it('折叠状态不持久化：重新挂载（等价刷新）回到默认收起', async () => {
    const wrapper = await mountView()
    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)

    // 重新挂载 = 刷新的组件生命周期：不读写任何持久化存储，必须回到产品认为重要的默认视图
    wrapper.unmount()
    const refreshed = await mountView()

    expect(refreshed.findAll('[data-test="monitor-core"]')).toHaveLength(0)
    expect(refreshed.find('[data-test="monitor-cpu-detail-summary"]').exists()).toBe(true)
    expect(refreshed.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('false')
  })
})

describe('MonitorView 响应式布局（#127：列数断点 + 无容器查询）', () => {
  const testDir = dirname(fileURLToPath(import.meta.url))
  const source = readFileSync(join(testDir, '..', 'MonitorView.vue'), 'utf8')

  /** 取某个 @media 块的内容（媒体块外层 `}` 顶格，内层规则缩进，故用 `\n}` 定位块尾） */
  function mediaBlock(header: string): string {
    const start = source.indexOf(header)
    expect(start, `缺少媒体查询：${header}`).toBeGreaterThanOrEqual(0)
    const open = source.indexOf('{', start)
    const close = source.indexOf('\n}', open)
    expect(close).toBeGreaterThan(open)
    return source.slice(open, close)
  }

  /** 取第一条 `.monitor-view__grid` 规则的内容（基础规则在样式中先出现，即 md–xl 档） */
  function baseGridRule(): string {
    const start = source.indexOf('.monitor-view__grid')
    expect(start, '缺少 .monitor-view__grid 基础规则').toBeGreaterThanOrEqual(0)
    const open = source.indexOf('{', start)
    const close = source.indexOf('}', open)
    expect(close).toBeGreaterThan(open)
    return source.slice(open, close)
  }

  it('监控页列数：<md 单列、md–xl 双列、≥xl 三列', () => {
    // 基础规则（md–xl）：双列（只认基础块本身，避免别处残留的 2 列让断言假绿）
    expect(baseGridRule()).toContain('grid-template-columns: repeat(2, minmax(0, 1fr))')
    // ≥xl（1280）：三列
    expect(mediaBlock('@media (min-width: 1280px)'))
      .toMatch(/\.monitor-view__grid\s*\{[^}]*grid-template-columns:\s*repeat\(3,\s*minmax\(0,\s*1fr\)\)/)
    // <md（767.98）：单列
    expect(mediaBlock('@media (max-width: 767.98px)'))
      .toMatch(/\.monitor-view__grid\s*\{[^}]*grid-template-columns:\s*1fr/)
  })

  it('不使用容器查询（既有白名单门禁只扫 @media，引入 @container 会绕过防野断点检查）', () => {
    expect(source).not.toContain('@container')
  })

  it('sparkline 用容器自适应：viewBox 拉伸 + 宽度 100%，不靠 JS 断点计算', () => {
    const sparkline = readFileSync(
      join(testDir, '..', '..', 'components', 'sysmon', 'SysmonSparkline.vue'),
      'utf8',
    )
    expect(sparkline).toContain('viewBox="0 0 100 24"')
    expect(sparkline).toContain('preserveAspectRatio="none"')
    expect(sparkline).toMatch(/\.sysmon-sparkline\s*\{[^}]*width:\s*100%/)
  })
})
