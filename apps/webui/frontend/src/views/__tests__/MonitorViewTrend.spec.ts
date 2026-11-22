import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { enableAutoUnmount, mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import MonitorView from '../MonitorView.vue'
import { parseCssFile, tokenDeclarationsOf } from '@/test-utils/cssTheme'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonHistoryStore } from '@/stores/sysmonHistory'
import { useSysmonStore } from '@/stores/sysmon'
import { SYSMON_TREND_METRIC_KEYS } from '@/stores/sysmonTrend'
import { SYSMON_CLOCK_LIVE, makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'
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

/** #370：每核条形从 CPU 宽卡移入台账 cpu.cores 行的展开区，需先点开才能断言逐核读数 */
async function openCores(wrapper: VueWrapper): Promise<void> {
  await toggle(wrapper, 'ledger-expand-cpu.cores')
}

/** assets/css 下某个文件的绝对路径（从本 spec 文件位置推导，不依赖 cwd） */
function cssPath(name: string): string {
  return join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'assets', 'css', name)
}
const themePath = (): string => cssPath('theme.css')
const designTokensPath = (): string => cssPath('design-tokens.css')

// #371：MonitorView 挂了 1 秒 ticker，用例必须卸载 —— 不卸载时未清理的定时器会在
// 后续用例的 flush 里插入无主更新，实测会让无关用例偶发假红。
enableAutoUnmount(afterEach)

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

  it('5 个标量指标各有一条趋势，维度指标（温度 / 每核 / 每盘 / 每网卡）没有', async () => {
    const wrapper = await mountView()

    // #362：CPU 使用率的趋势升级为宽卡内的「刻度列 + 折线」（不再是内联 sparkline 组件），
    // 其余四个标量指标仍是内联 sparkline。5 条趋势的数据源与时间窗口径不变。
    expect(wrapper.findAll('.sysmon-sparkline')).toHaveLength(4)
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').exists()).toBe(true)
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
    expect(wrapper.find('[data-test="monitor-tab-history"]').attributes('aria-pressed')).toBe('true')
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

describe('MonitorView 台账维度明细展开（#127 → #363）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('每核条形收在台账 cpu.cores 行展开区（#370），每核频率明细默认收起、点开展开后显示全部', async () => {
    const wrapper = await mountView()

    // #370：每核使用率条形从 CPU 宽卡移入台账 cpu.cores 行的展开区，默认收起
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-cpu-cores"]').exists()).toBe(false)
    // 每核频率同属维度明细，仍默认收起
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('false')

    await openCores(wrapper)
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    const summary = wrapper.find('[data-test="monitor-cpu-detail-summary"]')
    expect(summary.exists()).toBe(true)
    // 夹具每核使用率 0.05 + index*0.031，最大在 15 号核：0.515
    expect(summary.text()).toBe('16 核 · 最高 51.5%')

    // 频率明细展开后出现；条形数量不随折叠变化
    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(16)
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('true')

    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)

    // 收起 cpu.cores 行后条形回到折叠态（展开状态不持久化、由按钮控制）
    await openCores(wrapper)
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

  it('每盘七项明细默认收起：行本身平铺可见，「展开明细」给出完整七项', async () => {
    const wrapper = await mountView()

    // #363：盘行平铺（不点开就能看见盘名 / 忙碌占比 / 关键明细），收起的是**完整明细**
    const rows = wrapper.findAll('[data-test="monitor-disk-io-row"]')
    expect(rows).toHaveLength(2)
    // 夹具两块盘忙碌占比 0.35 / 0.72：行内直接给读数（不是摘要里的「最高」）
    expect(rows.map(row => row.find('.monitor-view__ledger-value').text())).toEqual(['72.0%', '35.0%'])
    expect(wrapper.findAll('[data-test="ledger-fields"]')).toHaveLength(0)

    await toggle(wrapper, 'ledger-expand-disk:nvme0n1')
    expect(wrapper.findAll('[data-test="ledger-fields"]')).toHaveLength(1)
    const labels = rows[0].findAll('[data-test="ledger-field"]').map(field => field.attributes('data-label'))
    expect(labels).toEqual(['读吞吐', '写吞吐', '读 IOPS', '写 IOPS', '平均延迟', '队列深度', '忙碌占比'])
  })

  it('每个展开按钮的可访问名各不相同，且都以可见文本开头（#363 C6：WCAG 2.5.3 + 4.1.2）', async () => {
    const wrapper = await mountView()

    const buttons = wrapper.findAll('[data-test^="ledger-expand-"]')
    expect(buttons.length).toBeGreaterThan(1)
    const names = buttons.map(button => button.attributes('aria-label') ?? '')
    // 唯一：原先 15 个按钮的可访问名全是「明细」，读屏里无法区分展开的是哪一行
    expect(new Set(names).size).toBe(names.length)
    for (const button of buttons) {
      const label = button.attributes('aria-label') ?? ''
      // 以可见文本开头（Label in Name）：可见文字就是「明细」/「收起」
      expect(label, button.attributes('data-test')).toMatch(/^(明细|收起) · .+/)
      expect(label, button.attributes('data-test')).toContain(button.text())
    }

    // 收起时不指向不存在的元素（aria-controls 条件绑定），展开后指向真实存在的字段表 id。
    // cpu.cores 行的展开区是逐核条形（data-test 为 monitor-cpu-cores，不是字段表），
    // 因此这里取一个渲染字段表的按钮来验这条绑定。
    const fieldsButton = buttons.find(button => !(button.attributes('data-test') ?? '').includes('cpu.cores'))!
    const rowId = (fieldsButton.attributes('data-test') ?? '').replace('ledger-expand-', '')
    expect(fieldsButton.attributes('aria-controls')).toBeUndefined()
    await toggle(wrapper, `ledger-expand-${rowId}`)
    expect(fieldsButton.attributes('aria-controls')).toBe(`ledger-fields-${rowId}`)
    const fields = wrapper.find(`[data-test="ledger-fields"]`)
    expect(fields.attributes('id')).toBe(`ledger-fields-${rowId}`)
  })

  it('时钟行的展开按钮同样带唯一的可访问名（#363 C6）', async () => {
    const wrapper = await mountView(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))

    const toggleBtn = wrapper.find('[data-test="monitor-clock-detail-toggle"]')
    expect(toggleBtn.exists()).toBe(true)
    const label = toggleBtn.attributes('aria-label') ?? ''
    expect(label).toContain('展开服务器明细')
    // 与其他展开按钮不重名：带上了行名（时钟偏差）
    expect(label).toContain('时钟偏差')
  })

  it('每网卡十项明细默认收起：收发各占一行平铺，展开给出完整八项', async () => {
    const wrapper = await mountView()

    // 一张网卡 = 接收 + 发送两行，都平铺可见
    const rows = wrapper.findAll('[data-test="monitor-network-row"]')
    expect(rows).toHaveLength(2)
    expect(rows.map(row => row.attributes('data-direction'))).toEqual(['rx', 'tx'])
    expect(wrapper.findAll('[data-test="ledger-fields"]')).toHaveLength(0)

    await toggle(wrapper, 'ledger-expand-nic:eth0:rx')
    const labels = rows[0].findAll('[data-test="ledger-field"]').map(field => field.attributes('data-label'))
    expect(labels).toEqual([
      '接收速率',
      '接收错误速率',
      '接收丢弃速率',
      '接收字节（累计）',
      '接收包数（累计）',
      '接收错误（累计）',
      '接收丢弃（累计）',
      '虚拟网卡',
    ])
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

    // 不需要展开就能看到「不可用」空态与原因（no_permission 由共享映射给出可读文案）。
    // #370：每核条形移入台账 cpu.cores 行的展开区，但**可用性事实**落在行本身的明细列，
    // 不藏在折叠后面（展开区里另有逐核读数的空态）。
    expect(wrapper.find('[data-row-id="cpu.cores"] [data-test="row-state"]').text())
      .toContain('权限不足，无法读取该指标')
    // #363：磁盘 I/O 与网卡的不可用是台账行上的说明文字，行本身平铺可见
    expect(wrapper.find('[data-row-id="disk_io"] [data-test="row-state"]').text())
      .toContain('磁盘性能计数器不可用')
    expect(wrapper.find('[data-row-id="network"] [data-test="row-state"]').text()).toContain('采集失败')
    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    // 不可用的行没有完整明细可展开（没有 fields 就不渲染展开按钮）
    expect(wrapper.find('[data-test="ledger-expand-disk_io"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="ledger-expand-network"]').exists()).toBe(false)
  })

  it('台账行明细展开状态不持久化：重新挂载（等价刷新）回到收起', async () => {
    const wrapper = await mountView()
    // 展开内存行明细
    await toggle(wrapper, 'ledger-expand-memory')
    expect(wrapper.find('[data-test="ledger-fields"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="ledger-expand-memory"]').attributes('aria-expanded')).toBe('true')

    // 重新挂载 = 刷新的组件生命周期：不读写 localStorage / sessionStorage，必须回到默认收起
    wrapper.unmount()
    const refreshed = await mountView()

    expect(refreshed.find('[data-test="ledger-fields"]').exists()).toBe(false)
    expect(refreshed.find('[data-test="ledger-expand-memory"]').attributes('aria-expanded')).toBe('false')
    // 也没有把展开态写进任何持久化存储
    expect(Object.keys(localStorage).filter(key => key.includes('ledger'))).toEqual([])
  })

  it('折叠状态不持久化：重新挂载（等价刷新）回到默认收起', async () => {
    const wrapper = await mountView()
    await toggle(wrapper, 'monitor-cpu-detail-toggle')
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(16)

    // 重新挂载 = 刷新的组件生命周期：不读写任何持久化存储，必须回到产品认为重要的默认视图
    wrapper.unmount()
    const refreshed = await mountView()

    expect(refreshed.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    // #370：每核条形默认收起 → 点开后数量与摘要照旧（折叠状态不持久化）
    expect(refreshed.findAll('[data-test="monitor-core"]')).toHaveLength(0)
    await openCores(refreshed)
    expect(refreshed.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(refreshed.find('[data-test="monitor-cpu-detail-summary"]').exists()).toBe(true)
    expect(refreshed.find('[data-test="monitor-cpu-detail-toggle"]').attributes('aria-expanded')).toBe('false')
  })
})

describe('MonitorView 响应式布局（#127：列数断点 + 无容器查询）', () => {
  const testDir = dirname(fileURLToPath(import.meta.url))
  const source = readFileSync(join(testDir, '..', 'MonitorView.vue'), 'utf8')

  /**
   * 取某选择器的**基础**规则内容（样式表内首次以「行首选择器 + {」出现的那个）。
   * 必须锚定行首 + `{`：裸 `indexOf(selector)` 会命中模板里的 `class="…"` 或复合选择器里的
   * 子串（如 `.monitor-view__ledger-row--empty .monitor-view__ledger-value`），
   * 断言的会是一条完全无关的规则（且恒真）。
   */
  function ruleBody(selector: string): string {
    const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
    const match = new RegExp(`^${escaped}\\s*\\{`, 'm').exec(source)
    expect(match, `缺少 ${selector} 基础规则`).not.toBeNull()
    const open = (match as RegExpExecArray).index + (match as RegExpExecArray)[0].length - 1
    const close = source.indexOf('}', open)
    expect(close).toBeGreaterThan(open)
    return source.slice(open, close)
  }

  /**
   * 在同一位宽档的**全部**媒体块里找包含指定选择器的那个，返回其内容。
   * 不能只取该档第一个媒体块：1280 档有两个（页面网格与 CPU 网格），取第一个会拿错。
   * 按选择器定位也避免「块内加注释 / 调整规则顺序」导致断言假红。
   */
  /** 取媒体块内某选择器的**单条规则**内容（mediaRuleBody 返回整块，断言会误配同块其它规则）。 */
  function ruleInMedia(header: string, selector: string): string {
    const block = mediaRuleBody(header, selector)
    const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
    const match = new RegExp(`${escaped}\\s*\\{([^}]*)\\}`).exec(block)
    expect(match, `媒体块 ${header} 内缺少 ${selector} 规则`).not.toBeNull()
    return (match as RegExpExecArray)[1]
  }

  function mediaRuleBody(header: string, selector: string): string {
    let from = 0
    for (;;) {
      const start = source.indexOf(header, from)
      if (start < 0) {
        expect(false, `媒体块 ${header} 内找不到 ${selector}`).toBe(true)
        return ''
      }
      const open = source.indexOf('{', start)
      const close = source.indexOf('\n}', open)
      expect(close).toBeGreaterThan(open)
      const body = source.slice(open, close)
      if (body.includes(selector)) return body
      from = close
    }
  }

  it('监控页网格是单列纵向：宽卡与台账跨满整行，没有任何媒体规则为网格改列数（#363）', () => {
    // 基础规则即单列。必须是 minmax(0, 1fr)——裸 1fr 的最小轨道是 auto，
    // 列会被卡片内最长不可断行内容顶宽（#321 的 320 宽溢出就复现于此形态）
    const grid = ruleBody('.monitor-view__grid')
    expect(grid).toContain('grid-template-columns: minmax(0, 1fr)')
    expect(grid).not.toContain('repeat(')
    // #325：卡片按内容高度排布（默认 stretch 会把同行卡片拉到最高者，造成大片卡内留白）
    expect(grid).toContain('align-items: start')

    // 网格里的子元素跨满整行：宽卡与台账由 CSS 声明跨列（#430 后主机条是顶栏下方的
    // 独立一条，不是网格项——它不在网格里断言，见下一条）。
    expect(ruleBody('.monitor-view__card--wide')).toContain('grid-column: 1 / -1')
    expect(source).toMatch(/class="[^"]*monitor-view__card--wide[^"]*"[^>]*data-test="monitor-ledger"/)
    // 主机条不在网格里（#430：顶栏下方的独立一条）：它的规则不得再声明跨列
    // （否则是死规则，会误导下一个人以为它在网格内）
    expect(ruleBody('.monitor-view__host'), '主机条不在网格内，不得残留网格跨列规则').not.toContain('grid-column')

    // 不存在为 `.monitor-view__grid` 单独改列数的媒体规则（列数不是版面变量了）。
    // 媒体块按「行首 `}` 收尾」切分，与上面的 mediaRuleBody 同一假设。
    const mediaBlocks = source.match(/@media[^{]*\{[\s\S]*?\n\}/g) ?? []
    expect(mediaBlocks.length).toBeGreaterThan(0)
    for (const block of mediaBlocks) {
      if (!block.includes('.monitor-view__grid')) continue
      expect(block, '媒体规则不得为页面网格改列数').not.toContain('grid-template-columns')
    }
  })

  it('每核条形容器自适应：一条 auto-fit 规则覆盖 4–128 核，不写断点、不用 JS 测量（#362）', () => {
    // #362：CPU 卡内不再是「多面板网格」，而是三块纵向排布（四数字 / 趋势 / 每核条形），
    // 因此原来「面板数 == 列数」的断言失去意义，改为断言每核条形这一条规则的容器自适应口径。
    const cores = ruleBody('.monitor-view__cores')
    expect(cores).toContain('repeat(auto-fit, minmax(var(--core-cell-min-width), 1fr))')
    // 不写断点：全文件里没有任何规则为每核网格单独改列数（一条规则覆盖 4–128 核）
    expect(source).not.toMatch(/\.monitor-view__cores[^{]*\{[^}]*grid-template-columns:\s*repeat\(\d/)
    // 不用 JS 测量：模板里不出现对每核网格的 clientWidth / getBoundingClientRect 计算
    expect(source).not.toMatch(/clientWidth|getBoundingClientRect/)
  })

  it('长 token 在台账里按列收口：名称 / 明细 / 展开值可断行，读数列不折（#321 / #363）', () => {
    // 挂载点与契约键可任意长度：名称列开 anywhere（它同时收窄 min-content，避免窄屏横向溢出）
    expect(ruleBody('.monitor-view__ledger-label')).toContain('overflow-wrap: anywhere')
    expect(ruleBody('.monitor-view__ledger-key')).toContain('overflow-wrap: anywhere')
    // 明细列的**文字**允许断行（列本身是 flex：文字 + 右端按钮，按钮不压缩）
    expect(ruleBody('.monitor-view__ledger-detail-text')).toContain('overflow-wrap: anywhere')
    expect(ruleBody('.monitor-view__ledger-detail')).toContain('display: flex')
    expect(ruleBody('.monitor-view__ledger-expand-btn')).toContain('flex: none')
    expect(ruleBody('.monitor-view__ledger-field-value')).toContain('overflow-wrap: anywhere')
    // 读数与阈值列**不折**：百分比 / 容量在窄列里折成两行等于读错（实测 320 下 68.8% 被折断）
    for (const selector of ['.monitor-view__ledger-value', '.monitor-view__ledger-threshold',
      '.monitor-view__ledger-headroom']) {
      const body = ruleBody(selector)
      expect(body, selector).toContain('white-space: nowrap')
      expect(body, selector).not.toContain('overflow-wrap')
    }
  })

  it('不使用容器查询（既有白名单门禁只扫 @media，引入 @container 会绕过防野断点检查）', () => {
    expect(source).not.toContain('@container')
  })

  it('台账七列共用一条列基线：一条 grid-template-columns 定义七列，三档只改列宽不改列数', () => {
    // 基础规则（≥1280）：七列，状态点 / 名称 / 轨道 / 当前值 / 明细 / 阈值 / 余量
    const base = ruleBody('.monitor-view__ledger-row')
    expect(base).toContain('display: grid')
    const columns = base.match(/grid-template-columns:\s*([^;]+);/)
    expect(columns, '台账行必须是网格').not.toBeNull()
    // 七列：按括号配平把每个轨道折成一个占位符再计数（minmax 里还嵌着 var(...)，
    // 只按正则截到第一个 ')' 会把一条轨道切成两半，让「七列」这条断言失去意义）
    const tracks: string[] = []
    let depth = 0
    let current = ''
    for (const char of columns![1]) {
      if (char === '(') depth += 1
      if (char === ')') depth -= 1
      if (/\s/.test(char) && depth === 0) {
        if (current.trim().length > 0) tracks.push(current.trim())
        current = ''
        continue
      }
      current += char
    }
    if (current.trim().length > 0) tracks.push(current.trim())
    expect(tracks).toHaveLength(7)
    // 列宽全部来自 token（列宽是版面几何量，直接写字面量会被 dz/token-literal 拦下；
    // 这里再断言一次，避免有人用 var(--spacer-*) 冒充列宽）
    for (const token of ['--ledger-col-state', '--ledger-col-name', '--ledger-col-track',
      '--ledger-col-value', '--ledger-col-detail', '--ledger-col-threshold', '--ledger-col-headroom']) {
      expect(columns![1], `列宽缺 ${token}`).toContain(`var(${token})`)
    }

    // 中档（1024–1279）：仍是七列，只换成 md 档的收缩宽度（并同时收起列间距）
    const md = ruleInMedia('@media (min-width: 1024px) and (max-width: 1279.98px)', '.monitor-view__ledger-row')
    expect(md).toContain('var(--ledger-col-name-md)')
    expect(md).toContain('var(--ledger-col-track-md)')
    expect(md).toContain('var(--ledger-col-detail-md)')
    expect(md).toContain('column-gap: var(--spacer-8)')

    // 窄屏（<1024，即侧栏常驻后容器不再够分七列）：三行堆叠，用 grid-template-areas 重排
    const narrow = ruleInMedia('@media (max-width: 1023.98px)', '.monitor-view__ledger-row')
    expect(narrow).toContain('grid-template-areas')
    for (const area of ['track track', 'detail detail', 'threshold headroom']) {
      expect(narrow, `窄屏缺少 ${area} 行`).toContain(area)
    }
    // 阈值与余量离开列位置后必须自带列名（否则「90%」与「12.8 GiB」是两个无主数字）
    expect(ruleInMedia('@media (max-width: 1023.98px)', '.monitor-view__ledger-col-label'))
      .toContain('display: inline')
    // 桌面档列名隐藏（七列各有固定位置，列名是噪声）
    expect(ruleBody('.monitor-view__ledger-col-label')).toContain('display: none')
  })

  it('阈值 / 余量两列按契约全空间定宽（#382：数字列只由内容定宽，不随版面收窄）', () => {
    // 实测（11px 等宽数字，台账真实单元格）：
    // 阈值最宽 = 默认档 `±100 / ±1000 ms` 84.86px（秒档最宽 80.47px、分钟档 64.06px）；
    // 余量最宽 = 字节余量 `已超 1023.9 MiB` 82.09px（时钟余量换秒后 ≤ 58px、点数余量 66.8px）。
    // 两列在两个位宽档都必须容得下这两串——#382 的教训正是「按当前机器实测值定列宽」。
    const tokens = tokenDeclarationsOf(parseCssFile(designTokensPath()), ':root', false)
    const REQUIRED: Record<string, number> = {
      '--ledger-col-threshold': 88,
      '--ledger-col-headroom': 88,
      '--ledger-col-threshold-md': 88,
      '--ledger-col-headroom-md': 88,
    }
    for (const [token, min] of Object.entries(REQUIRED)) {
      const value = tokens.get(token)
      expect(value, `design-tokens.css 缺少 ${token}`).toBeDefined()
      const px = Number.parseFloat(String(value))
      expect(px, `${token}=${value} 容不下契约上限文案（需 ≥ ${min}px）`).toBeGreaterThanOrEqual(min)
    }
    // 两档的阈值 / 余量列宽必须一致：同一串文本在两个位宽档不能一档放得下一档放不下
    expect(tokens.get('--ledger-col-threshold')).toBe(tokens.get('--ledger-col-threshold-md'))
    expect(tokens.get('--ledger-col-headroom')).toBe(tokens.get('--ledger-col-headroom-md'))
  })

  it('数值列右对齐（DESIGN.md:354）：阈值 / 余量 / 值槽；值槽是 flex 列，还要给 alignItems', () => {
    // 表格口径：数值列右对齐 + 等宽数字。阈值 / 余量是纯文字格 → text-align: right
    for (const selector of ['.monitor-view__ledger-threshold', '.monitor-view__ledger-headroom']) {
      const body = ruleBody(selector)
      expect(body, selector).toContain('text-align: right')
      expect(body, selector).toContain('font-variant-numeric: tabular-nums')
    }
    // 值槽是纵向 flex（值 + sparkline 两层）：`text-align` 管不到 flex 项，必须 align-items
    const value = ruleBody('.monitor-view__ledger-value')
    expect(value).toContain('align-items: flex-end')
    expect(value).toContain('text-align: right')
    // 窄屏堆叠时阈值格回到左对齐（它此时在「阈值 headroom」一行的左端，右对齐会贴住余量）
    expect(ruleInMedia('@media (max-width: 1023.98px)', '.monitor-view__ledger-threshold'))
      .toContain('text-align: start')
  })

  it('台账元信息文字用 --text-secondary（tertiary 在两套主题下都够不到 11px 正文的 4.5:1）', () => {
    // 承载**信息**的小字（不是装饰）：状态文字 / 契约键 / 明细列 / 组注 / 窄屏列名
    for (const selector of ['.monitor-view__ledger-state-text', '.monitor-view__ledger-key',
      '.monitor-view__ledger-detail-text', '.monitor-view__ledger-group-note',
      '.monitor-view__ledger-col-label']) {
      expect(ruleBody(selector), selector).toContain('color: var(--text-secondary)')
    }
    // 等级文字与等级数值走**配对档** -on-surface（Paired-Step Rule，DESIGN.md:229）；
    // 点 / 轨道填充仍保持实体 -default —— 这条重启只约束文字。
    const tokens = [
      ['.monitor-view__ledger-row[data-level=\'warn\'] .monitor-view__ledger-state-text',
        'var(--status-warning-on-surface)'],
      ['.monitor-view__ledger-row[data-level=\'danger\'] .monitor-view__ledger-state-text',
        'var(--status-error-on-surface)'],
      ['.monitor-view__ledger-row[data-level=\'warn\'] .monitor-view__ledger-value',
        'var(--status-warning-on-surface)'],
      ['.monitor-view__ledger-row[data-level=\'danger\'] .monitor-view__ledger-value',
        'var(--status-error-on-surface)'],
    ] as const
    for (const [selector, token] of tokens) {
      expect(ruleBody(selector), selector).toContain(`color: ${token}`)
    }
    // 点与轨道填充不动（它们是 -default 的正当归属：实体色只给点/条/图标）
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'warn\'] .monitor-view__ledger-dot'))
      .toContain('background: var(--status-alert-default)')
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'danger\'] .monitor-view__ledger-track-fill'))
      .toContain('background: var(--status-error-default)')
  })

  it('#432 存活信号着色：在流动=success 绿、停推=warning 黄；文字与标签仍中性（与指标绿分工见 DESIGN.md）', () => {
    // 脉冲点是**连接/进程事实**（数据在推），#432 起着色；#371 的「中性色」是旧方向口径。
    const dot = ruleBody('.monitor-view__host-dot')
    expect(dot).toContain('background: var(--status-success-default)')
    expect(dot).toContain('animation: monitor-view-host-pulse')
    // 状态槽文字与标签保持中性色：颜色只是冗余信号，停推事实由文案承担（不靠色相才可读）
    for (const selector of ['.monitor-view__host-status', '.monitor-view__host-label']) {
      const body = ruleBody(selector)
      expect(body, `${selector} 不得占用存活/状态色`).not.toContain('--status-success-default')
      expect(body, `${selector} 不得借用警示色`).not.toContain('--status-warning-default')
      // 中性色只允许两档（tertiary 在深色主机条底上只有 3.19，够不到正文 4.5:1）
      expect(body, `${selector} 应用 --text-secondary`).toContain('var(--text-secondary)')
    }
    // 停推：脉冲点转 warning 黄并停跳；状态槽只加字重，不加背景 / 彩色 / 动画
    const staleDot = ruleBody('.monitor-view__host--stale .monitor-view__host-dot')
    expect(staleDot).toContain('background: var(--status-warning-default)')
    expect(staleDot).toContain('animation: none')
    const stale = ruleBody('.monitor-view__host--stale .monitor-view__host-status')
    expect(stale).toContain('font-weight: var(--body-xs-strong-font-weight)')
    expect(stale).not.toContain('background')
    expect(stale).not.toContain('animation')
    // 主机条单条内容不换行（换行只发生在条与条之间，#430）：值 nowrap + 省略号
    const value = ruleBody('.monitor-view__host-value')
    expect(value).toContain('white-space: nowrap')
    expect(value).toContain('text-overflow: ellipsis')
    expect(value).toContain('overflow: hidden')
    // 容器允许整体换行：390 下靠这一条消化（单条内容另由 nowrap 锁住），
    // 这是 AC3/AC4 的核心机制，删掉它必须有用例变红
    expect(ruleBody('.monitor-view__host')).toContain('flex-wrap: wrap')
    // spacer 吃掉剩余宽度 → 状态槽恒在所在行最右（含换行后的第二行）
    expect(ruleBody('.monitor-view__host-spacer')).toContain('flex: 1 1 auto')
    expect(ruleBody('.monitor-view__host-status')).toContain('flex: none')
  })

  it('#371 中性色与绿色在**解析后**确实不同：token 名匹配之外，颜色值也要分开', () => {
    // 上一条只 grep token 名（源码文本匹配），挡不住「token 被重新定义成绿色」这种改动。
    // 这里把三个 token 的名字解析成实际字面值，断言存活信号用的中性色与状态绿**不同值**。
    // 解析口径复用 tokenContrast.spec.ts 的同一套（theme.css 三段结构 → 每段的 token 表），
    // 不在本文件另写一份，否则两份会漂移（review-checklist §2.4）。
    // 两处来源都要读：状态色（--status-*）定义在 design-tokens.css 的 :root 里，
    // 主题色（--text-secondary）在 theme.css 的两段里。合并顺序照 src/main.ts 的导入顺序
    // （design-tokens 在前、theme 在后，同名由主题段胜），与 tokenContrast.spec.ts 同一口径。
    const designTokens = tokenDeclarationsOf(parseCssFile(designTokensPath()), ':root', false)
    const themeRoot = parseCssFile(themePath())
    const scopes = {
      light: new Map([...designTokens, ...tokenDeclarationsOf(themeRoot, ':root', false)]),
      dark: new Map([...designTokens, ...tokenDeclarationsOf(themeRoot, '.dark', false)]),
    }
    for (const [theme, scope] of Object.entries(scopes)) {
      const neutral = scope.get('--text-secondary')
      const success = scope.get('--status-success-default')
      expect(neutral, `${theme} 段缺 --text-secondary`).toBeDefined()
      expect(success, `${theme} 段缺 --status-success-default`).toBeDefined()
      // 直白但有效的断言：两者不是同一个字面值。若有人把 --text-secondary 重新定义成
      // 状态绿（或反过来），存活信号与指标状态就又会同色，One Job Rule 当场失效。
      expect(neutral, `${theme} 段的中性色与状态绿同值，存活信号会与指标状态撞色`)
        .not.toBe(success)
    }
  })

  it('#371 台账的 normal 状态点与轨道填充仍然保持绿色（绿只属于指标状态）', () => {
    // 与上一条成对：存活信号撤出之后，绿色必须仍然在它唯一正当的位置
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'normal\'] .monitor-view__ledger-dot'))
      .toContain('background: var(--status-success-default)')
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'normal\'] .monitor-view__ledger-track-fill'))
      .toContain('background: var(--status-success-default)')
  })

  it('组成带的「已用」与「缓存」分得开：阶梯整体往「虚」侧挪一格（#363 C7）', () => {
    // 已用：保持最实的中性色 + 一圈底色内描边（用既有 token，不引入新 token / 新纹理）
    const used = ruleBody('.monitor-view__ledger-band-seg--used')
    expect(used).toContain('background: var(--text-default)')
    expect(used).toContain('box-shadow: inset 0 0 0 var(--border-width-default) var(--bg-layout-1)')
    // 缓存 / 缓冲各接过下一档：整条阶梯往「虚」侧挪一格，让弱边界从「已用|缓存」
    // （压力 / 可回收的分界，必须看清）挪到「缓存|缓冲」（两者都属可回收）。
    expect(ruleBody('.monitor-view__ledger-band-seg--cached'))
      .toContain('background: var(--text-tertiary)')
    expect(ruleBody('.monitor-view__ledger-band-seg--buffers'))
      .toContain('background: var(--text-disabled)')
    // 空闲仍是最虚的叠色阶
    expect(ruleBody('.monitor-view__ledger-band-seg--free'))
      .toContain('background: var(--bg-overlay-l3)')
  })

  it('值按钮在触摸设备上有 44px 下限（.ds-btn 的 P5-T3 规则管不到自绘按钮，#363 C8）', () => {
    // 必须在 `@media (pointer: coarse)` 里（桌面 pointer: fine 保持台账紧凑密度）
    const coarse = source.slice(source.indexOf('@media (pointer: coarse)'))
    expect(coarse, '缺少 @media (pointer: coarse) 块').not.toBe('')
    expect(coarse).toContain('.monitor-view__value-btn')
    // 44px 走 token（--spacer-40），不写裸 px
    expect(coarse).toMatch(/\.monitor-view__value-btn\s*\{[^}]*min-height:\s*var\(--spacer-40\)/)
  })

  it('轨道缺口只画在有契约阈值的行上：CSS 用属性选择器不做判断，判断在纯函数层', () => {
    // 缺口是展示分档（70 / 90），位置由 JS 注入；CSS 侧只定义位置与形状
    expect(ruleBody('.monitor-view__ledger-gap')).toContain('left: var(--ledger-gap, 0%)')
    // 轨道填充同理：逐行不同，只能运行时注入（未注入时退回 0%，不闪一条满格轨道）
    expect(ruleBody('.monitor-view__ledger-track-fill')).toContain('width: var(--ledger-fill, 0%)')
    // 组成带各段宽度按各自占比注入
    expect(ruleBody('.monitor-view__ledger-band-seg')).toContain('var(--ledger-seg, 0%)')
  })

  it('「立即查询」按钮按最长冷却文案预留宽度，倒计时不引起跳动（#326）', () => {
    // 文案在「立即查询」/「N 秒后可再次查询」之间切换（原实测 87.1→138.5px）。
    // `manual_min_interval_sec` 上限 3600 → 最长文案实测 151.7px；预留 158px（含跨平台字体
    // 回退余量）使按钮宽度保持常量，内容居中，不再有左缘位移。
    expect(ruleBody('.monitor-view__clock-probe .ds-btn')).toMatch(/min-width:\s*158px/)
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


  it('768–1279 页头收紧：单行、标题组可收缩、副标题省略号（#328）', () => {
    // 单行由基础规则保证（topbar nowrap）；可收缩的是作为 flex item 的 header
    expect(ruleBody('.monitor-view__topbar')).toContain('flex-wrap: nowrap')
    expect(ruleBody('.monitor-view__header')).toContain('min-width: 0')
    // 间距分档：有主机条时页头与它读成一组（12），其余视图 / 等待态保持 24（#430）
    expect(ruleBody('.monitor-view__topbar')).toContain('margin-bottom: var(--spacer-24)')
    expect(ruleBody('.monitor-view__topbar--host-follows')).toContain('margin-bottom: var(--spacer-12)')
    // 中档只收紧副标题：省略号而不是把 Tab 顶到下一行
    const subtitle = ruleInMedia('@media (min-width: 768px) and (max-width: 1279.98px)', '.monitor-view__subtitle')
    expect(subtitle).toContain('white-space: nowrap')
    expect(subtitle).toContain('overflow: hidden')
    expect(subtitle).toContain('text-overflow: ellipsis')
  })

  it('窄屏只钉视图切换行，不钉整块 header（#328）', () => {
    const narrow = '@media (max-width: 767.98px)'
    // 窄屏滚动容器是 document（dashboard.css ≤1024 放开高度）：.monitor-view 必须让出 overflow:auto，
    // 否则它成为最近 scrollport、sticky 被困在内部（实测滚动时行直接跟着消失）
    expect(ruleInMedia(narrow, '.monitor-view')).toContain('overflow: visible')
    // 拆 box：让视图切换行成为滚动容器直接子级，sticky 才能跨内容区生效
    expect(ruleInMedia(narrow, '.monitor-view__topbar')).toContain('display: contents')
    const switchRule = ruleInMedia(narrow, '.monitor-view__view-switch')
    expect(switchRule).toContain('position: sticky')
    // 贴在全局 .topbar（layout.css 固定 32px）下方，内容不从缝隙透出
    expect(switchRule).toContain('top: 32px')
    expect(switchRule).toMatch(/z-index:\s*2/)
    expect(switchRule).toContain('background: var(--bg-base-default)')
    // 只钉 Tab 行：header 本身不得 sticky（否则 390×844 上吃掉 90–110px 视口）
    expect(ruleInMedia(narrow, '.monitor-view__header')).not.toContain('position: sticky')
  })
})

describe('MonitorView CPU 自适应趋势（#362：纵轴随序列变化 + 三档刻度 + 如实时间窗）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('三档刻度标签 + 纵轴范围说明：19%→21% 的波动不被压成一条直线', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const to = Math.floor(NOW_MS / 1000)
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric: params.metric ?? 'cpu.usage_ratio',
        from: to - 120,
        to,
        interval_sec: 60,
        truncated: false,
        series: [{
          dimension: '',
          points: [
            { ts: to - 120, min: 0.19, max: 0.19, avg: 0.19, last: 0.19, n: 1 },
            { ts: to - 60, min: 0.21, max: 0.21, avg: 0.21, last: 0.21, n: 1 },
          ],
        }],
        marks: [],
        gaps: [],
      } satisfies SysmonHistoryAvailable)
    })
    const wrapper = await mountView()

    const ticks = wrapper.findAll('.monitor-view__chart-tick')
    expect(ticks).toHaveLength(3)
    // 三档整数百分点标签，自上而下递减（上界 / 中点 / 下界）
    const labels = ticks.map(tick => Number(tick.text().replace('%', '')))
    expect(labels.every(value => Number.isInteger(value))).toBe(true)
    expect(labels[0]).toBeGreaterThan(labels[1])
    expect(labels[1]).toBeGreaterThan(labels[2])
    // 轴范围随 19–21% 的序列收窄：绝不是 0–100 的固定轴
    expect(labels[0] - labels[2]).toBeLessThanOrEqual(10)
    expect(labels[0]).toBeGreaterThanOrEqual(20)
    // 说明行必须点明「自适应，不是 0–100」——刻度不是从 0 起的轴
    const caption = wrapper.find('[data-test="monitor-cpu-trend-caption"]').text()
    expect(caption).toMatch(/纵轴 \d+–\d+%（自适应，不是 0–100）/)
    // 折线在 100px 高度内用满绘图区（不是一条平线）
    const points = wrapper.find('[data-test="monitor-sparkline-cpu"] polyline').attributes('points') ?? ''
    const ys = points.split(' ').map(pair => Number(pair.split(',')[1]))
    expect(Math.max(...ys) - Math.min(...ys)).toBeGreaterThan(15)
  })

  it('时间窗按实际覆盖标注：未满 15 分钟不写「最近 15 分钟」（默认夹具只覆盖 2 分钟）', async () => {
    const wrapper = await mountView()

    const caption = wrapper.find('[data-test="monitor-cpu-trend-caption"]').text()
    // 夹具历史覆盖 2 分钟（两条 60 秒桶）+ 实时点，实际跨度 ≈ 2 分钟
    expect(caption).toContain('最近 2 分钟')
    expect(caption).not.toContain('最近 15 分钟')
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('data-window-complete')).toBe('false')
  })

  it('覆盖满 15 分钟才写「最近 15 分钟」', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const to = Math.floor(NOW_MS / 1000)
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric: params.metric ?? 'cpu.usage_ratio',
        from: to - 15 * 60,
        to,
        interval_sec: 60,
        truncated: false,
        series: [{
          dimension: '',
          points: [
            { ts: to - 15 * 60, min: 0.1, max: 0.1, avg: 0.1, last: 0.1, n: 1 },
            { ts: to, min: 0.2, max: 0.2, avg: 0.2, last: 0.2, n: 1 },
          ],
        }],
        marks: [],
        gaps: [],
      } satisfies SysmonHistoryAvailable)
    })
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-trend-caption"]').text()).toContain('最近 15 分钟')
    expect(wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('data-window-complete')).toBe('true')
  })

  it('序列不足两个点时不渲染趋势（一个点画不出趋势），数值与每核条形照常', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) => {
      const to = Math.floor(NOW_MS / 1000)
      return Promise.resolve({
        available: true,
        level: 'raw',
        metric: params.metric ?? 'cpu.usage_ratio',
        from: to - 15 * 60,
        to,
        interval_sec: null,
        truncated: false,
        series: [{ dimension: '', points: [] }],
        marks: [],
        gaps: [],
      } satisfies SysmonHistoryAvailable)
    })
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-trend"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    // #370：每核条形在台账 cpu.cores 行的展开区，点开后照常可见
    await openCores(wrapper)
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
  })
})
