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
    expect(wrapper.find('[data-test="monitor-memory-meta"]').text()).toBe('16.0 GiB / 32.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-available"]').text()).toBe('16.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-cached"]').text()).toBe('8.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-buffers"]').text()).toBe('256.0 MiB')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('/data')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('25.0%')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('eth0')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('收 1.0 MiB/s')
    expect(wrapper.text()).not.toContain('不可用')
  })

  it('部分不可用：不可用指标显示明确空态（不得显示 0），其余真实数据照常渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: {
        'cpu.total': { available: false, reason: 'unsupported' },
        memory: { available: false, reason: 'disabled_by_config' },
        swap: { available: false, reason: 'disabled_by_config' },
        volumes: { available: false, reason: 'not_implemented' },
      },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('不可用')
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('unsupported')
    // 内存 / 交换不可用：明确空态 + 原因，不渲染任何数值
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').text()).toContain('disabled_by_config')
    expect(wrapper.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').text()).toContain('disabled_by_config')
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('该指标将在后续版本接入')
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

describe('MonitorView 温度与每核频率（#73，可用性驱动）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('全部可用：温度卡片与每核频率面板渲染真实数据（45.5 °C / 3600 MHz）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ sensorData: true }))
    const wrapper = await mountView()

    const temperatureCard = wrapper.find('[data-test="monitor-temperature"]')
    expect(temperatureCard.exists()).toBe(true)
    expect(temperatureCard.text()).toContain('coretemp#hwmon3:Package id 0')
    expect(temperatureCard.text()).toContain('45.5 °C')
    expect(temperatureCard.text()).toContain('41.5 °C')
    const sensors = wrapper.findAll('[data-test="monitor-temperature-sensor"]')
    expect(sensors).toHaveLength(2)
    expect(sensors[0].text()).toContain('45.5 °C')

    // 每核频率面板位于 CPU 卡片内（宽面板，复用每核样式）
    expect(wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-frequency"]').exists()).toBe(true)
    const frequencies = wrapper.findAll('[data-test="monitor-frequency-core"]')
    expect(frequencies).toHaveLength(16)
    expect(frequencies[0].attributes('data-core-index')).toBe('0')
    expect(frequencies[0].text()).toContain('核 0')
    expect(frequencies[0].text()).toContain('3600 MHz')
    expect(frequencies[15].text()).toContain('5100 MHz')
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('不可用')
  })

  it('不可用：温度与每核频率区块整块不渲染（不显示 0 / 不可用占位）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-temperature-empty"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-frequency-empty"]').exists()).toBe(false)
    // 不可用不是 0：不出现温度 / 频率数字
    expect(wrapper.text()).not.toContain('°C')
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('MHz')
  })

  it('Windows 式快照（unsupported、数据为 null）：温度与每核频率区块整块不渲染', async () => {
    const snapshot = makeSysmonSnapshot({
      metrics: {
        temperature: { available: false, reason: 'unsupported' },
        'cpu.frequency': { available: false, reason: 'unsupported' },
      },
    })
    snapshot.platform = {
      family: 'windows',
      version: 'Windows 10 Pro',
      kernel: '10.0.19045',
      arch: 'x86_64',
      build: 19045,
    }
    snapshot.availability.platform_description = 'Windows 10 Pro (build 19045)'
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    // 区块可见性由 availability 决定，不按 platform.family 分支
    expect(wrapper.find('[data-test="monitor-subtitle"]').text()).toContain('Windows 10 Pro (build 19045)')
    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(false)
  })

  it('可用但本帧无数据（null / 空数组）：温度与每核频率区块显示「暂无数据」空态，不显示 0', async () => {
    // 能力粘性可用、本帧值为 null（契约 sysmon §2.5）：保留区块，显示「暂无数据」而不是「不可用」
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      sensorData: true,
      temperatures: null,
      coreFrequencies: null,
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(true)
    const temperatureEmpty = wrapper.find('[data-test="monitor-temperature-empty"]')
    expect(temperatureEmpty.exists()).toBe(true)
    expect(temperatureEmpty.text()).toContain('暂无数据')
    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)

    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(true)
    const frequencyEmpty = wrapper.find('[data-test="monitor-frequency-empty"]')
    expect(frequencyEmpty.exists()).toBe(true)
    expect(frequencyEmpty.text()).toContain('暂无数据')
    expect(wrapper.findAll('[data-test="monitor-frequency-core"]')).toHaveLength(0)
    expect(frequencyEmpty.text()).not.toContain('0 MHz')

    // 空数组同样走空态（温度允许空数组；消费方不得据此隐藏区块）
    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      sensorData: true,
      temperatures: [],
      coreFrequencies: [],
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-temperature-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-frequency-empty"]').exists()).toBe(true)
  })

  it('非法值过滤：空 sensor / 非有限温度 / 非法 index 或 mhz 不渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      sensorData: true,
      temperatures: [
        { sensor: '', celsius: 30 },
        { sensor: 'coretemp:Core 0', celsius: Number.NaN },
        { sensor: 'coretemp:Valid', celsius: 45.5 },
      ],
      coreFrequencies: [
        { index: -1, mhz: 3600 },
        { index: 2, mhz: 0 },
        { index: 3, mhz: 3600 },
      ],
    }))
    const wrapper = await mountView()

    const sensors = wrapper.findAll('[data-test="monitor-temperature-sensor"]')
    expect(sensors).toHaveLength(1)
    expect(sensors[0].text()).toContain('coretemp:Valid')
    expect(sensors[0].text()).toContain('45.5 °C')

    const frequencies = wrapper.findAll('[data-test="monitor-frequency-core"]')
    expect(frequencies).toHaveLength(1)
    expect(frequencies[0].attributes('data-core-index')).toBe('3')
    expect(frequencies[0].text()).toContain('3600 MHz')
  })

  it('组合可用性：仅温度可用时只渲染温度卡，仅频率可用时只渲染频率面板', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      sensorData: true,
      metrics: { 'cpu.frequency': { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(false)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      sensorData: true,
      metrics: { temperature: { available: false, reason: 'no_sensor' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(true)
  })
})

describe('MonitorView 内存与交换（#74，可用性驱动）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照：内存使用率 / 已用/总量 / 可用 / 缓存 / 缓冲与交换区块均渲染真实数据', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-memory-meta"]').text()).toBe('16.0 GiB / 32.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-available"]').text()).toBe('16.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-cached"]').text()).toBe('8.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-buffers"]').text()).toBe('256.0 MiB')

    const swap = wrapper.find('[data-test="monitor-memory-swap"]')
    expect(swap.exists()).toBe(true)
    expect(swap.text()).toContain('25.0%')
    expect(swap.text()).toContain('2.0 GiB / 8.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-swap-none"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').exists()).toBe(false)
  })

  it('Windows 式内存（buffers_bytes=null）：不渲染缓冲行，缓存与可用行保留', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 32 * GIB,
        available_bytes: 16 * GIB,
        used_bytes: 16 * GIB,
        used_ratio: 0.5,
        cached_bytes: 4 * GIB,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-memory-cached"]').text()).toBe('4.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-buffers"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-available"]').exists()).toBe(true)
  })

  it('缓存 / 缓冲均不可得（null）：两行都不渲染，不用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 32 * GIB,
        available_bytes: 16 * GIB,
        used_bytes: 16 * GIB,
        used_ratio: 0.5,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-memory-cached"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-buffers"]').exists()).toBe(false)
  })

  it('无交换分区（swap 可用且 total_bytes=0）：显示「无交换分区」，不显示 0%', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
    }))
    const wrapper = await mountView()

    const none = wrapper.find('[data-test="monitor-memory-swap-none"]')
    expect(none.exists()).toBe(true)
    expect(none.text()).toBe('无交换分区')
    expect(wrapper.find('[data-test="monitor-memory-swap"]').exists()).toBe(false)
    // 交换子区块不出现任何百分比数字
    expect(wrapper.find('[data-test="monitor-swap"]').text()).not.toContain('%')
  })

  it('交换不可用（无交换分区 → unsupported）：显示「不可用」+ 明确无交换文案，不渲染 total=0 对象', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
      metrics: { swap: { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-test="monitor-memory-swap-unavailable"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.text()).toContain('不可用')
    // 无交换是平台事实而非故障：给出明确文案，而不是把裸 reason 丢给用户
    expect(unavailable.text()).toContain('本机无交换分区/页面文件')
    expect(wrapper.find('[data-test="monitor-memory-swap"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-swap-none"]').exists()).toBe(false)
  })

  it('交换可用但本帧对象缺失（能力粘性 / 未采集）：显示「暂无数据」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ swap: null }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-test="monitor-memory-swap-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-swap"]').exists()).toBe(false)
  })

  it('内存可用但本帧对象缺失：显示「暂无数据」空态（不是不可用），交换仍独立渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ memory: null }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-test="monitor-memory-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').exists()).toBe(false)
    // 交换可用性独立于内存：仍渲染真实数值
    expect(wrapper.find('[data-test="monitor-memory-swap"]').exists()).toBe(true)
  })

  it('内存不可用但 payload 残留数据：按可用性显示不可用，不渲染陈旧数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-test="monitor-memory-unavailable"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.text()).toContain('采集失败，暂不可用')
    expect(wrapper.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory"]').text()).not.toContain('50.0%')
    expect(wrapper.find('[data-test="monitor-memory"]').text()).not.toContain('16.0 GiB')
  })

  it('内存比率由 used/total 派生（不信任 payload 的 used_ratio，保证字节与百分比一致）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 32 * GIB,
        available_bytes: 16 * GIB,
        used_bytes: 16 * GIB,
        used_ratio: null,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountView()

    // 可用且字节有效时按唯一口径派生 50%，不因发送端比率缺失显示「不可用」
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-memory-meta"]').text()).toBe('16.0 GiB / 32.0 GiB')
  })
})

describe('MonitorView 磁盘（#75：卷容量 / 关注路径 / 共享内存目录，可用性驱动）', () => {
  const GIB = 1024 ** 3
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照：3 个卷按 watched 优先排序，关注卷高亮并带「关注」标签', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(3)
    // 关注卷 /data 虽在 payload 中排第二，前端按 watched 优先排到首位
    expect(rows[0].attributes('data-mount')).toBe('/data')
    expect(rows[0].classes()).toContain('monitor-view__disk-row--watched')
    expect(rows[0].find('[data-test="monitor-disk-volume-watched"]').exists()).toBe(true)
    expect(rows[0].text()).toContain('25.0%')
    expect(rows[0].text()).toContain('256.0 GiB / 1024.0 GiB')

    expect(rows[1].attributes('data-mount')).toBe('/')
    expect(rows[1].classes()).not.toContain('monitor-view__disk-row--watched')
    expect(rows[1].find('[data-test="monitor-disk-volume-watched"]').exists()).toBe(false)
    expect(rows[1].text()).toContain('40.0%')
    expect(rows[2].attributes('data-mount')).toBe('/home')
    expect(rows[2].text()).toContain('62.0%')

    // 卷集合与数量完全由快照驱动，无硬编码盘符
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(false)
  })

  it('卷 available=false：显示「不可用」+ 原因文案，绝不显示 0% / 0 B', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/backup', watched: false, available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(2)
    const backup = rows[1]
    expect(backup.attributes('data-mount')).toBe('/backup')
    expect(backup.text()).toContain('不可用')
    expect(backup.find('[data-test="monitor-disk-volume-reason"]').text()).toBe('无访问权限')
    // 不得用 0 冒充：不可用卷不出现百分比 / 字节数
    expect(backup.text()).not.toContain('0%')
    expect(backup.text()).not.toContain('0 B')
    expect(backup.text()).not.toContain('GiB')
  })

  it('卷可读性脏数据（NaN/负数）：按不可用渲染，不显示 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/dirty', watched: false, available: true, total_bytes: Number.NaN, used_bytes: -1, used_ratio: 0 },
      ],
    }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-test="monitor-disk-volume"]')
    expect(row.text()).toContain('不可用')
    expect(row.find('[data-test="monitor-disk-volume-reason"]').text()).toBe('采集失败')
    expect(row.text()).not.toContain('0%')
  })

  it('volumes 不可用：显示不可用 + 原因空态，不渲染卷条目', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { volumes: { available: false, reason: 'disabled_by_config' } },
    }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-test="monitor-disk-unavailable"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('不可用')
    // 空态沿用既有 unavailableDesc：未映射的原因按裸 reason 展示
    expect(empty.text()).toContain('disabled_by_config')
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(0)
  })

  it('关注路径：渲染 path + 落卷；解析失败显示「未解析」+ 原因，不猜卷', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      watchPaths: [
        { path: '/srv/dztrader/shm', mount: '/data', available: true },
        { path: '/srv/dztrader/missing', mount: null, available: false, reason: 'path_not_found' },
      ],
    }))
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-watch-path"]')
    expect(rows).toHaveLength(2)
    expect(rows[0].text()).toContain('/srv/dztrader/shm')
    expect(rows[0].text()).toContain('落卷 /data')
    expect(rows[1].text()).toContain('/srv/dztrader/missing')
    expect(rows[1].find('[data-test="monitor-watch-path-unavailable"]').text()).toContain('未解析')
    expect(rows[1].find('[data-test="monitor-watch-path-unavailable"]').text()).toContain('路径不存在或无法解析')
  })

  it('关注路径为空（本帧未采集）：不渲染该子区块', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ watchPaths: null }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-watch-paths"]').exists()).toBe(false)
  })

  it('共享内存目录：渲染路径 / 占用 / 页文件数 / 最旧页文件年龄', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-shm-path"]').text()).toBe('/srv/dztrader/shm')
    expect(wrapper.find('[data-test="monitor-shm-total"]').text()).toBe('720.0 MiB')
    expect(wrapper.find('[data-test="monitor-shm-page-files"]').text()).toBe('12')
    expect(wrapper.find('[data-test="monitor-shm-oldest-age"]').text()).toBe('8 分钟')
    expect(wrapper.find('[data-test="monitor-shm-unavailable"]').exists()).toBe(false)
  })

  it('共享内存目录无页文件（oldest_file_age_sec=null）：不渲染该行，不显示 0 秒', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      shmDir: { path: '/srv/shm', total_bytes: 0, page_files: 0, oldest_file_age_sec: null },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-shm-page-files"]').text()).toBe('0')
    expect(wrapper.find('[data-test="monitor-shm-oldest-age"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-shm-dir"]').text()).not.toContain('0 秒')
  })

  it('共享内存目录不可用：显示不可用 + 原因；可用但本帧缺失显示「暂无数据」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { shm_dir: { available: false, reason: 'no_permission' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-test="monitor-shm-unavailable"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.text()).toContain('不可用')
    expect(unavailable.text()).toContain('no_permission')
    expect(wrapper.find('[data-test="monitor-shm-path"]').exists()).toBe(false)

    // 能力粘性可用但本帧对象缺失 → 「暂无数据」而不是「不可用」
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ shmDir: null }))
    await wrapper.vm.$nextTick()
    const empty = wrapper.find('[data-test="monitor-shm-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-shm-unavailable"]').exists()).toBe(false)
  })

  it('可用但 total_bytes=0 的特殊卷：使用率显示「—」而不是 0%，字节照常渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/special', watched: false, available: true, total_bytes: 0, used_bytes: 0, used_ratio: null },
      ],
    }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-test="monitor-disk-volume"]')
    expect(row.find('.monitor-view__list-value').text()).toBe('—')
    expect(row.text()).not.toContain('0.0%')
    expect(row.text()).not.toContain('不可用')
  })

  it('不按 platform.family 分支：Windows 形状卷（C: / D:）照常按数据渲染', async () => {
    const snapshot = makeSysmonSnapshot({
      volumes: [
        { mount: 'C:', watched: true, available: true, total_bytes: 500 * GIB, used_bytes: 250 * GIB, used_ratio: 0.5 },
        { mount: 'D:', watched: false, available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
      watchPaths: [{ path: 'C:\\dztrader\\shm', mount: 'C:', available: true }],
      shmDir: { path: 'C:\\dztrader\\shm', total_bytes: 128 * MIB, page_files: 4, oldest_file_age_sec: 30 },
    })
    snapshot.platform = {
      family: 'windows',
      version: 'Windows 10 Pro',
      kernel: '10.0.19045',
      arch: 'x86_64',
      build: 19045,
    }
    snapshot.availability.platform_description = 'Windows 10 Pro (build 19045)'
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(2)
    expect(rows[0].attributes('data-mount')).toBe('C:')
    expect(rows[0].classes()).toContain('monitor-view__disk-row--watched')
    expect(rows[0].text()).toContain('50.0%')
    expect(rows[1].attributes('data-mount')).toBe('D:')
    expect(rows[1].text()).toContain('不可用')
    expect(wrapper.find('[data-test="monitor-shm-path"]').text()).toBe('C:\\dztrader\\shm')
    expect(wrapper.find('[data-test="monitor-shm-oldest-age"]').text()).toBe('30 秒')
  })

  it('WS 推送更新：卷数量与关注标记随快照刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(3)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/only', watched: true, available: true, total_bytes: 10 * GIB, used_bytes: 9 * GIB, used_ratio: 0.9 },
      ],
    }))
    await wrapper.vm.$nextTick()

    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(1)
    expect(rows[0].attributes('data-mount')).toBe('/only')
    expect(rows[0].text()).toContain('90.0%')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('MonitorView 磁盘 I/O（#76，可用性驱动）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照（能力可用）：两块盘按 device 升序渲染吞吐 / IOPS / 延迟 / 队列深度 / 利用率', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-io-row"]')
    expect(rows).toHaveLength(2)
    // payload 顺序为 sda、nvme0n1；store 按 device 升序重排
    expect(rows[0].attributes('data-device')).toBe('nvme0n1')
    expect(rows[1].attributes('data-device')).toBe('sda')

    const nvmeValues = rows[0].findAll('.monitor-view__io-value').map(value => value.text())
    expect(nvmeValues).toEqual([
      '64.0 MiB/s',
      '32.0 MiB/s',
      '2048 IOPS',
      '1024 IOPS',
      '0.45 ms',
      '1.25',
      '72.0%',
    ])

    const sdaValues = rows[1].findAll('.monitor-view__io-value').map(value => value.text())
    expect(sdaValues).toEqual([
      '4.0 MiB/s',
      '2.0 MiB/s',
      '128 IOPS',
      '64 IOPS',
      '1.25 ms',
      '2.50',
      '35.0%',
    ])

    expect(wrapper.find('[data-test="monitor-disk-io-empty"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-disk-io"]').text()).not.toContain('不可用')
  })

  it('首帧 / 计数器重置（七项均 null）：逐项显示「不可用」，绝不显示 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      diskIo: [{
        device: 'sda',
        read_bytes_per_sec: null,
        write_bytes_per_sec: null,
        read_iops: null,
        write_iops: null,
        latency_ms: null,
        queue_depth: null,
        utilization: null,
      }],
    }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-test="monitor-disk-io-row"]')
    expect(row.attributes('data-device')).toBe('sda')
    const values = row.findAll('.monitor-view__io-value').map(value => value.text())
    expect(values).toHaveLength(7)
    expect(values.every(value => value === '不可用')).toBe(true)
    // 不得用 0 冒充：不出现 0 B/s、0 IOPS、0.00 ms、0.0%
    expect(row.text()).not.toContain('0 B/s')
    expect(row.text()).not.toContain('0 IOPS')
    expect(row.text()).not.toContain('0.00 ms')
    expect(row.text()).not.toContain('0.0%')
    expect(wrapper.find('[data-test="monitor-disk-io-empty"]').exists()).toBe(false)
  })

  it('能力不可用（no_permission）：整块「不可用」+ 可操作文案，不渲染盘行', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'no_permission' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-test="monitor-disk-io-unavailable"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.text()).toContain('不可用')
    expect(unavailable.text()).toContain('磁盘性能计数器不可用（需管理员权限或性能计数器未开启）')
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(0)
  })

  it('可用但本帧空数组 / null：显示「暂无数据」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ diskIo: [] }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-test="monitor-disk-io-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(0)

    // 能力粘性可用、本帧值为 null（WS 推送）→ 同样走「暂无数据」，不改成「不可用」
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ diskIo: null }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').exists()).toBe(false)
  })

  it('不可用原因文案：disabled_by_config / collect_failed / 未知 reason 分别映射', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'disabled_by_config' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').text()).toContain('已在配置中关闭')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'collect_failed' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').text()).toContain('采集失败，暂不可用')

    // 未知 reason 走既有 unavailableDesc：裸 reason 展示，不吞掉诊断信息
    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'some_new_reason' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-unavailable"]').text()).toContain('some_new_reason')
  })

  it('semantic_hints.disk_io 非空：区块内显示「语义提示：…」；为空时不渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      semanticHints: { disk_io: 'windows counters are per physical disk' },
    }))
    const wrapper = await mountView()

    const hint = wrapper.find('[data-test="monitor-disk-io-hint"]')
    expect(hint.exists()).toBe(true)
    expect(hint.text()).toBe('语义提示：windows counters are per physical disk')

    useSysmonStore().applySnapshot(makeSysmonSnapshot())
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-hint"]').exists()).toBe(false)
  })
})
