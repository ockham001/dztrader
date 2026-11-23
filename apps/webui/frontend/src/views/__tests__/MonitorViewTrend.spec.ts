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

  it('2 个趋势指标各有一条趋势（CPU 块宽图 + 共享内存目录内联 sparkline），负载 / 内存 / 交换没有', async () => {
    const wrapper = await mountView()

    // #435：CPU 使用率的趋势是 CPU 块内的固定 0–100% 宽图（不是内联 sparkline 组件）；
    // 负载的迷你趋势已按用户拍板移除；#445 内存卡照主机面板插件做成 Meter，内存 / 交换也没有趋势。
    expect(wrapper.findAll('.sysmon-sparkline')).toHaveLength(1)
    expect(wrapper.find('[data-test="monitor-sparkline-shm"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-sparkline-load"]').exists()).toBe(false)
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
    expect(wrapper.find('[data-test="monitor-sparkline-shm"]').attributes('data-last-value')).toBe(String(720 * 1024 * 1024))
    // 展示层：旁边的数字是同一个值的格式化结果（18.7% ↔ 0.187；#445 内存百分比在内存卡里）
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[data-test="monitor-memory-percent"]').text()).toBe('50.0%')
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
    expect(wrapper.find('[data-test="monitor-memory-percent"]').text()).toBe('50.0%')
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

    await wrapper.find('[data-test="monitor-memory-percent"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="sysmon-history"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-tab-history"]').attributes('aria-pressed')).toBe('true')
    // 预选生效：store 指标键切到内存使用率，历史视图据此查询
    expect(history.metricKey).toBe('memory.used_ratio')
    const calls = vi.mocked(sysmonApi.getHistory).mock.calls
    expect(calls.length).toBe(before + 1)
    expect(calls[calls.length - 1][0]?.metric).toBe('memory.used_ratio')
  })

  it('点击内存卡里的交换百分比同样跳转历史并预选（交换使用率）', async () => {
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-swap-percent"]').trigger('click')
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

  it('每核条形常驻 CPU 块（#435）：不在折叠区；台账 CPU 组与每核频率明细入口均已删除', async () => {
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(wrapper.find('[data-test="monitor-cpu-cores"]').exists()).toBe(true)
    // 台账不再有 CPU 组：cpu.cores / cpu.rates 两行都不存在
    expect(wrapper.find('[data-row-id="cpu.cores"]').exists()).toBe(false)
    expect(wrapper.find('[data-row-id="cpu.rates"]').exists()).toBe(false)
    // 每核频率明细入口已删除（频率进条形 tooltip）
    expect(wrapper.find('[data-test="monitor-cpu-detail-toggle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-core"]').attributes('title')).toContain('GHz')
    // 条形不属于任何折叠内容（CPU 块已没有折叠区）：始终全部渲染
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
  })

  it('温度明细不进页面 DOM：全部传感器通过 title / aria 可达，四数字行没有折叠按钮（#435 验收收敛）', async () => {
    const wrapper = await mountView(makeSysmonSnapshot({
      sensorData: true,
      temperatures: [
        { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
        { sensor: 'coretemp#hwmon3:Core 0', chip: 'coretemp', label: 'Core 0', celsius: 41.5 },
        { sensor: 'acpitz_0#hwmon0:temp1', chip: 'acpitz_0', label: '', celsius: 27.8 },
      ],
    }))

    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-temperature-detail-toggle"]').exists()).toBe(false)
    // 主数值 = 最高温度；归属 = 该读数的芯片与通道
    expect(wrapper.find('[data-test="monitor-temperature-max"]').text()).toBe('45.5 °C')
    expect(wrapper.find('[data-test="monitor-temperature-summary"]').text()).toBe('CPU · Package id 0')
    // 全部传感器在 title 里（悬停可看；读屏用同一份 aria-label）
    const title = wrapper.find('[data-test="monitor-temperature-max"]').attributes('title') ?? ''
    expect(title).toContain('CPU · Package id 0 45.5 °C')
    expect(title).toContain('CPU · Core 0 41.5 °C')
    expect(title).toContain('主板 ACPI · temp1 27.8 °C')
    expect(wrapper.find('[data-test="monitor-temperature"]').attributes('aria-label')).toBe(`温度 45.5 °C；${title}`)
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
    const fieldsButton = buttons[0]
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
    // #435：每核条形在 CPU 块里常驻；不可用事实直接显示在条形区的空态上，台账不再有 CPU 行。
    expect(wrapper.find('[data-test="monitor-cores-empty"]').text())
      .toContain('权限不足，无法读取该指标')
    expect(wrapper.find('[data-row-id="cpu.cores"]').exists()).toBe(false)
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
    // 展开盘行明细（#445 后内存行已移出台账，用磁盘 I/O 行验证同一机制）
    await toggle(wrapper, 'ledger-expand-disk:nvme0n1')
    expect(wrapper.find('[data-test="ledger-fields"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="ledger-expand-disk:nvme0n1"]').attributes('aria-expanded')).toBe('true')

    // 重新挂载 = 刷新的组件生命周期：不读写 localStorage / sessionStorage，必须回到默认收起
    wrapper.unmount()
    const refreshed = await mountView()

    expect(refreshed.find('[data-test="ledger-fields"]').exists()).toBe(false)
    expect(refreshed.find('[data-test="ledger-expand-disk:nvme0n1"]').attributes('aria-expanded')).toBe('false')
    // 也没有把展开态写进任何持久化存储
    expect(Object.keys(localStorage).filter(key => key.includes('ledger'))).toEqual([])
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

  it('监控页网格：<lg 单列、≥lg 两列；CPU 块与台账跨满整行，内存卡占半宽（#445）', () => {
    // 基础规则即单列。必须是 minmax(0, 1fr)——裸 1fr 的最小轨道是 auto，
    // 列会被卡片内最长不可断行内容顶宽（#321 的 320 宽溢出就复现于此形态）
    const grid = ruleBody('.monitor-view__grid')
    expect(grid).toContain('grid-template-columns: minmax(0, 1fr)')
    // #325：卡片按内容高度排布（默认 stretch 会把同行卡片拉到最高者，造成大片卡内留白）
    expect(grid).toContain('align-items: start')

    // #445：≥lg 两列（主机面板插件的两列网格同形），内存卡占一列；宽块仍跨满整行
    const lg = ruleInMedia('@media (min-width: 1024px)', '.monitor-view__grid')
    expect(lg).toContain('grid-template-columns: repeat(2, minmax(0, 1fr))')
    const memoryCard = source.match(/class="[^"]*monitor-view__memory[^"]*"/)?.[0] ?? ''
    expect(memoryCard).not.toContain('monitor-view__card--wide')

    // 网格里的子元素跨满整行：CPU 块与台账由 CSS 声明跨列（#430 后主机条是顶栏下方的
    // 独立一条，不是网格项——它不在网格里断言，见下一条）。
    expect(ruleBody('.monitor-view__card--wide')).toContain('grid-column: 1 / -1')
    expect(source).toMatch(/class="[^"]*monitor-view__card--wide[^"]*"[^>]*data-test="monitor-ledger"/)
    // 主机条不在网格里（#430：顶栏下方的独立一条）：它的规则不得再声明跨列
    // （否则是死规则，会误导下一个人以为它在网格内）
    expect(ruleBody('.monitor-view__host'), '主机条不在网格内，不得残留网格跨列规则').not.toContain('grid-column')
  })

  it('每核条形按容器实测自适应：列数由纯函数算好经 --core-columns 注入，不用 auto-fit（#435）', () => {
    // #435：多行时每行数量尽量相等 → 列数必须由 JS 实测容器宽度后计算（纯函数在
    // composables/monitorCpu.ts）；auto-fit 会把余数甩给末行（16 核可能折成 11 + 5）。
    const cores = ruleBody('.monitor-view__cores')
    expect(cores).toContain('grid-template-columns: repeat(var(--core-columns, 1), minmax(0, 1fr))')
    expect(cores).not.toContain('auto-fit')
    // 不写断点：一条规则覆盖 4–128 核（折行由列数决定，不由媒体查询决定）
    expect(source).not.toMatch(/\.monitor-view__cores[^{]*\{[^}]*repeat\(\d/)
    // 列数来源：视图用 ResizeObserver 实测容器宽度，交给纯函数 coreBarColumns
    // 列数来源与卸载清理由行为用例覆盖（MonitorView.spec：ResizeObserver 桩驱动列数 / 卸载 disconnect），
    // 这里只锁 CSS 契约，不断言私有实现表达式（unit-test-standard §1.2）
    // 条形高度 / 四数字字号对齐参照（主机面板 54px / 19px）：都是几何量，走 token
    const tokens = tokenDeclarationsOf(parseCssFile(designTokensPath()), ':root', false)
    expect(tokens.get('--core-bar-height')).toBe('54px')
    expect(tokens.get('--card-stat-value-font-size')).toBe('19px')
  })

  it('占用率主数值按 resourceLevel 的 70/90 着色，文字用 -on-surface 配对档（#435 决策 7）', () => {
    // 实体 -default 只给点 / 条 / 图标；大字号文本也必须走 -on-surface（浅色 -default 绿 2.79:1）
    const cases = [
      ['normal', 'var(--status-success-on-surface)'],
      ['warn', 'var(--status-warning-on-surface)'],
      ['danger', 'var(--status-error-on-surface)'],
    ] as const
    for (const [level, token] of cases) {
      const selector = `.monitor-view__value-btn.monitor-view__cpu-usage[data-level='${level}']`
      expect(ruleBody(selector), selector).toContain(`color: ${token}`)
    }
    // 温度 / 频率 / 负载的主数值不套色（契约没有对应阈值，不编造判据）
    expect(source).not.toMatch(/monitor-view__(metric|value-btn)[^{]*\[data-level\]/)
    expect(ruleBody('.monitor-view__metric')).toContain('color: var(--text-default)')
  })

  it('频率主值是「大号数字 + 12px 单位」，曲线区有分层底色、网格线更弱（#435 8080 验收）', () => {
    // 频率：数值 19px，单位 12px/secondary（主机面板同款两段排版）
    const withUnit = ruleBody('.monitor-view__metric--with-unit')
    expect(withUnit).toContain('display: inline-flex')
    expect(withUnit).toContain('align-items: baseline')
    const unit = ruleBody('.monitor-view__stat-unit')
    expect(unit).toContain('font-size: var(--body-sm-font-size)')
    expect(unit).toContain('color: var(--text-secondary)')
    // 曲线绘图区底色与卡片分层
    const chart = ruleBody('.monitor-view__chart')
    expect(chart).toContain('background: var(--bg-overlay-l1)')
    expect(chart).toContain('border-radius: var(--radius-6)')
    // 网格线弱化：叠 0.6 不透明度（不改轴色 token）
    expect(ruleBody('.monitor-view__chart-gridline')).toContain('opacity: 0.6')
    // 每核填充按等级着色；「饱和」专属修饰类已删除
    expect(ruleBody(".monitor-view__core-bar-fill[data-level='warn']")).toContain('var(--status-warning-default)')
    expect(ruleBody(".monitor-view__core-bar-fill[data-level='danger']")).toContain('var(--status-error-default)')
    expect(source).not.toContain('core-bar-fill--saturated')
    expect(source).not.toContain('monitor-core-saturated')
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

  it('#432 存活点与指标点共用 success 绿：同色族不同义，语义分工由 DESIGN / GLOSSARY 承担', () => {
    // #371 曾要求「存活信号与状态绿解析后必须不同值」。#432 起这条被反转（用户拍板「绿黄」）：
    // 存活点用 success 绿，与台账 normal 点同族；两件事靠**语义文档**区分（DESIGN.md 的
    // Liveness signals 一句 + GLOSSARY.md「存活信号」），不再靠颜色差异承担。
    // 这里做两件事：① 源码级确认两处都指向同一个 token（不是各造一套绿）；
    // ② 解析后确认 token 在两套主题下都有定义（挡「token 被删/改名」这类改动）。
    expect(ruleBody('.monitor-view__host-dot')).toContain('background: var(--status-success-default)')
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'normal\'] .monitor-view__ledger-dot'))
      .toContain('background: var(--status-success-default)')

    const designTokens = tokenDeclarationsOf(parseCssFile(designTokensPath()), ':root', false)
    const themeRoot = parseCssFile(themePath())
    const scopes = {
      light: new Map([...designTokens, ...tokenDeclarationsOf(themeRoot, ':root', false)]),
      dark: new Map([...designTokens, ...tokenDeclarationsOf(themeRoot, '.dark', false)]),
    }
    for (const [theme, scope] of Object.entries(scopes)) {
      expect(scope.get('--status-success-default'), `${theme} 段缺 --status-success-default`).toBeDefined()
      expect(scope.get('--status-warning-default'), `${theme} 段缺 --status-warning-default`).toBeDefined()
    }
  })

  it('#432 台账的 normal 状态点与轨道填充仍用指标绿（与存活绿同族，语义靠文档区分）', () => {
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'normal\'] .monitor-view__ledger-dot'))
      .toContain('background: var(--status-success-default)')
    expect(ruleBody('.monitor-view__ledger-row[data-level=\'normal\'] .monitor-view__ledger-track-fill'))
      .toContain('background: var(--status-success-default)')
  })

  it('组成带的「已用」与「缓存」分得开：阶梯整体往「虚」侧挪一格（#363 C7）', () => {
    // 已用：保持最实的中性色 + 一圈底色内描边（用既有 token，不引入新 token / 新纹理）
    const used = ruleBody('.monitor-view__memory-band-seg--used')
    expect(used).toContain('background: var(--text-default)')
    expect(used).toContain('box-shadow: inset 0 0 0 var(--border-width-default) var(--bg-layout-1)')
    // 缓存 / 缓冲各接过下一档：整条阶梯往「虚」侧挪一格，让弱边界从「已用|缓存」
    // （压力 / 可回收的分界，必须看清）挪到「缓存|缓冲」（两者都属可回收）。
    expect(ruleBody('.monitor-view__memory-band-seg--cached'))
      .toContain('background: var(--text-tertiary)')
    expect(ruleBody('.monitor-view__memory-band-seg--buffers'))
      .toContain('background: var(--text-disabled)')
    // 空闲仍是最虚的叠色阶
    expect(ruleBody('.monitor-view__memory-band-seg--free'))
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
    // 组成带各段宽度按各自占比注入（#445 迁入内存卡，变量改名）
    expect(ruleBody('.monitor-view__memory-band-seg')).toContain('var(--memory-seg, 0%)')
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

describe('MonitorView CPU 固定 0–100% 趋势（#435：单曲线 + 网格线 + 横轴时刻 + 如实时间窗）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.spyOn(Date, 'now').mockReturnValue(NOW_MS)
    mockHistoryAvailable()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  /** 两条 60 秒桶的 CPU 历史（值由调用方给，用于构造不同的曲线形状） */
  function cpuHistory(values: [number, number]): SysmonHistoryAvailable {
    const to = Math.floor(NOW_MS / 1000)
    return {
      available: true,
      level: 'raw',
      metric: 'cpu.usage_ratio',
      from: to - 120,
      to,
      interval_sec: 60,
      truncated: false,
      series: [{
        dimension: '',
        points: [
          { ts: to - 120, min: values[0], max: values[0], avg: values[0], last: values[0], n: 1 },
          { ts: to - 60, min: values[1], max: values[1], avg: values[1], last: values[1], n: 1 },
        ],
      }],
      marks: [],
      gaps: [],
    }
  }

  it('纵轴固定 0–100%：19%→21% 的波动贴近底边，不再被自适应轴拉伸；无刻度列 / 无轴说明', async () => {
    vi.mocked(sysmonApi.getHistory).mockImplementation((params = {}) =>
      Promise.resolve({ ...cpuHistory([0.19, 0.21]), metric: params.metric ?? 'cpu.usage_ratio' }))
    const wrapper = await mountView()

    // #435 决策 2：删掉左侧刻度标签与说明里的「纵轴 …%」，网格线承担尺度感
    expect(wrapper.findAll('.monitor-view__chart-tick')).toHaveLength(0)
    expect(wrapper.find('.monitor-view__chart-axis').exists()).toBe(false)
    const caption = wrapper.find('[data-test="monitor-cpu-trend-caption"]').text()
    expect(caption).not.toContain('纵轴')
    expect(caption).not.toContain('自适应')

    // 网格线是静态几何：0/25/50/75/100 五条
    expect(wrapper.findAll('.monitor-view__chart-gridline')).toHaveLength(5)
    expect(wrapper.find('.monitor-view__chart-gridline').attributes('vector-effect')).toBe('non-scaling-stroke')

    // 19%–21% 在固定轴上接近底边（y∈(25,27)），而不是像自适应轴那样铺满绘图区
    const points = wrapper.find('[data-test="monitor-sparkline-cpu"] polyline').attributes('points') ?? ''
    const ys = points.split(' ').map(pair => Number(pair.split(',')[1]))
    expect(Math.max(...ys) - Math.min(...ys)).toBeLessThan(1)
    expect(Math.min(...ys)).toBeGreaterThan(25)
  })

  it('横轴两端给真实时刻（与序列首末点同源，不是另行采样）', async () => {
    const wrapper = await mountView()

    const startText = wrapper.find('[data-test="monitor-cpu-trend-start"]').text()
    const endText = wrapper.find('[data-test="monitor-cpu-trend-end"]').text()
    expect(startText).toMatch(/^\d{2}:\d{2}:\d{2}$/)
    expect(endText).toMatch(/^\d{2}:\d{2}:\d{2}$/)
    // 夹具历史覆盖 2 分钟 + 实时点：两端必须不同（同值说明没按首末点取）
    expect(startText).not.toBe(endText)
  })

  it('横轴两端取自曲线的有效首末点，并并入趋势按钮的读屏文案', async () => {
    const wrapper = await mountView()

    const startText = wrapper.find('[data-test="monitor-cpu-trend-start"]').text()
    const endText = wrapper.find('[data-test="monitor-cpu-trend-end"]').text()
    const aria = wrapper.find('[data-test="monitor-sparkline-cpu"]').attributes('aria-label') ?? ''
    // 可见刻度列已删除（拍板 2），但时刻与轴口径必须对读屏用户可达
    expect(aria).toContain('纵轴固定 0–100%')
    expect(aria).toContain(`从 ${startText} 到 ${endText}`)
    // 首末点不同（来自序列两端，不是同一时刻）
    expect(startText).not.toBe(endText)
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
    // #435：每核条形常驻 CPU 块（不需要点开任何折叠）
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
  })
})
