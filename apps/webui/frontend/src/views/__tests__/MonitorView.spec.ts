import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonStore } from '@/stores/sysmon'
import { makeSysmonCpuCores, makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn() },
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
}

async function mountView() {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return wrapper
}

describe('MonitorView 资源监控页（三种快照状态）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('全部可用：系统信息与 CPU 渲染真实数据，资源区块不显示不可用', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuUsageRatio: 0.187, resourceData: true }),
    )
    const wrapper = await mountView()

    // 系统信息接真实数据
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('trade-01')
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('AMD Ryzen 9 7950X 16-Core Processor')
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('01:01:01')  // 运行时长
    // CPU 总使用率接真实数据（含进度条）
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[role="progressbar"]').attributes('aria-valuenow')).toBe('19')
    // 内存 / 磁盘 / 网络按可用性渲染真实数据
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('/data')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('25.0%')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('eth0')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('收 1.0 MiB/s')
    expect(wrapper.text()).not.toContain('不可用')
  })

  it('部分不可用：不可用指标显示明确空态（不得显示 0），其余真实数据照常渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('不可用')
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('unsupported')
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').text()).toContain('该指标将在后续版本接入')
    // 系统信息仍可用
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('trade-01')
    // 不得用 0 冒充 CPU 数值
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('0.0%')
  })

  it('快照尚未到达：整页等待空态，不渲染任何指标数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-waiting"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-waiting"]').text()).toContain('等待系统监控数据')
    expect(wrapper.find('[data-test="monitor-system"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu"]').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('%')
  })
})

describe('MonitorView CPU 区块完整化（#72）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('全部可用：16 核、负载三项、上下文切换/中断速率均渲染，不出现不可用', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      resourceData: true,
      semanticHints: { 'cpu.load_average': 'linux load average from /proc/loadavg' },
    }))
    const wrapper = await mountView()

    // CPU 卡片整行宽（总量 + 每核 + 负载 + 速率）
    expect(wrapper.find('[data-test="monitor-cpu"]').classes()).toContain('monitor-view__card--wide')

    const cores = wrapper.findAll('[data-test="monitor-core"]')
    expect(cores).toHaveLength(16)
    expect(cores[0].attributes('data-core-index')).toBe('0')
    expect(cores[15].attributes('data-core-index')).toBe('15')
    expect(cores[0].text()).toContain('核 0')
    expect(cores[0].text()).toContain('5.0%')

    expect(wrapper.find('[data-test="monitor-load-one"]').text()).toBe('0.42')
    expect(wrapper.find('[data-test="monitor-load-five"]').text()).toBe('0.51')
    expect(wrapper.find('[data-test="monitor-load-fifteen"]').text()).toBe('0.63')
    expect(wrapper.find('[data-test="monitor-load-hint"]').text()).toContain('linux load average from /proc/loadavg')

    expect(wrapper.find('[data-test="monitor-context-switches"]').text()).toBe('1234/s')
    expect(wrapper.find('[data-test="monitor-interrupts"]').text()).toBe('567/s')
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('不可用')
  })

  it('饱和核：仅饱和核带「饱和」标记与警示样式', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ saturatedCores: [2, 5] }))
    const wrapper = await mountView()

    const tags = wrapper.findAll('[data-test="monitor-core-saturated"]')
    expect(tags).toHaveLength(2)
    expect(tags.every(tag => tag.text() === '饱和')).toBe(true)

    const coreItems = wrapper.findAll('[data-test="monitor-core"]')
    const saturatedIndices = coreItems
      .filter(core => core.find('[data-test="monitor-core-saturated"]').exists())
      .map(core => core.attributes('data-core-index'))
    expect(saturatedIndices).toEqual(['2', '5'])
    expect(coreItems[2].classes()).toContain('monitor-view__core--saturated')
    expect(coreItems[0].classes()).not.toContain('monitor-view__core--saturated')
  })

  it('部分不可用：单核 null 显示不可用、负载空态、速率 null 显示不可用', async () => {
    const cores = makeSysmonCpuCores(4)
    cores[1].usage_ratio = null
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuCores: cores,
      metrics: { 'cpu.load_average': { available: false, reason: 'unsupported' } },
      contextSwitchesPerSec: null,
      interruptsPerSec: null,
    }))
    const wrapper = await mountView()

    const coreItems = wrapper.findAll('[data-test="monitor-core"]')
    expect(coreItems).toHaveLength(4)
    expect(coreItems[1].text()).toContain('不可用')
    expect(coreItems[0].text()).not.toContain('不可用')

    const loadEmpty = wrapper.find('[data-test="monitor-load-unavailable"]')
    expect(loadEmpty.exists()).toBe(true)
    expect(loadEmpty.text()).toContain('unsupported')
    expect(wrapper.find('[data-test="monitor-load-hint"]').exists()).toBe(false)

    expect(wrapper.find('[data-test="monitor-context-switches"]').text()).toBe('不可用')
    expect(wrapper.find('[data-test="monitor-interrupts"]').text()).toBe('不可用')
  })

  it('cpu.cores 不可用：每核子区块显示不可用原因空态，不渲染核条目', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.cores': { available: false, reason: 'no_permission' } },
    }))
    const wrapper = await mountView()

    const panel = wrapper.find('[data-test="monitor-cpu-cores"]')
    expect(panel.text()).toContain('不可用')
    expect(panel.text()).toContain('no_permission')
    expect(wrapper.find('[data-test="monitor-cores-empty"]').exists()).toBe(true)
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(0)
  })

  it('cpu.cores 可用但数组为空：显示「暂无每核数据」，不显示 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuCores: [] }))
    const wrapper = await mountView()

    const panel = wrapper.find('[data-test="monitor-cpu-cores"]')
    expect(panel.text()).toContain('暂无每核数据')
    expect(panel.text()).not.toContain('不可用')
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(0)
  })

  it('核数变化：新快照按数组长度重新渲染（16 → 8），逻辑核数同步', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(16) }),
    )
    const wrapper = await mountView()
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).toContain('16 逻辑核')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(8) }))
    await wrapper.vm.$nextTick()

    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(8)
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).toContain('8 逻辑核')
  })

  it('semantic=processor_queue：只展示「处理器队列长度」单值并带语义提示', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuLoadAverage: { one: 3.25, five: null, fifteen: null, semantic: 'processor_queue' },
      semanticHints: { 'cpu.load_average': 'processor queue length is not load average' },
    }))
    const wrapper = await mountView()

    const loadPanel = wrapper.find('[data-test="monitor-load"]')
    expect(loadPanel.text()).toContain('处理器队列长度')
    expect(loadPanel.text()).toContain('3.25')
    expect(loadPanel.text()).not.toContain('1 分钟')
    expect(wrapper.find('[data-test="monitor-load-one"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-load-hint"]').text())
      .toContain('processor queue length is not load average')
  })

  it('首批计数未就绪（total null）：仍走总使用率不可用空态，其余 CPU 子区块独立渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      contextSwitchesPerSec: null,
      interruptsPerSec: null,
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-value"]').exists()).toBe(false)
    const empty = wrapper.find('[data-test="monitor-cpu-unavailable"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无可用采样（首次采样未完成或计数器重置）')

    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    expect(wrapper.find('[data-test="monitor-context-switches"]').text()).toBe('不可用')
    expect(wrapper.find('[data-test="monitor-interrupts"]').text()).toBe('不可用')
  })
})
