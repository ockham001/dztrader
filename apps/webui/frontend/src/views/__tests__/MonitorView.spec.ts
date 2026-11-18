import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useAuthStore } from '@/stores/auth'
import { sysmonPending, useSysmonStore } from '@/stores/sysmon'
import { useToastStore } from '@/stores/toast'
import { makeSysmonConfig, makeSysmonCpuCores, makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn(), getHistory: vi.fn() },
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
}

/** 维度明细的折叠按钮（#127 默认收起；本文件聚焦数据渲染，挂载后统一展开） */
const DETAIL_TOGGLES = [
  'monitor-cpu-detail-toggle',
  'monitor-temperature-detail-toggle',
  'monitor-disk-io-detail-toggle',
  'monitor-network-detail-toggle',
]

async function mountView() {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  // #127 起每核 / 每传感器 / 每盘七项指标 / 每网卡十项指标默认收起；本文件验证的是「有数据时
  // 渲染什么」，故挂载后展开明细。折叠默认态、切换与摘要由 MonitorViewTrend.spec.ts 覆盖。
  for (const toggleTest of DETAIL_TOGGLES) {
    const toggle = wrapper.find(`[data-test="${toggleTest}"]`)
    if (toggle.exists()) await toggle.trigger('click')
  }
  await wrapper.vm.$nextTick()
  return wrapper
}

/** 「显示全部网卡」开关的 admin 判定走 auth store 的 isAdmin（role === 'admin'） */
function setRole(role: 'admin' | 'user'): void {
  useAuthStore().user = {
    id: 1,
    username: role,
    display_name: role,
    email: `${role}@x.com`,
    role,
    status: 'online',
    created_at: '2026-08-01',
  }
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

    // 主机条（#362：取代原「系统信息」卡）接真实数据
    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.text()).toContain('trade-01')
    expect(host.text()).toContain('AMD Ryzen 9 7950X 16-Core Processor')
    expect(host.text()).toContain('01:01:01')  // 运行时长
    // CPU 总使用率接真实数据
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    // 内存 / 磁盘 / 网络按可用性渲染真实数据
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-memory-meta"]').text()).toBe('16.0 GiB / 32.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-available"]').text()).toBe('16.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-cached"]').text()).toBe('8.0 GiB')
    expect(wrapper.find('[data-test="monitor-memory-buffers"]').text()).toBe('256.0 MiB')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('/data')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('25.0%')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('eth0')
    expect(wrapper.find('[data-test="monitor-network-row"] [data-test="monitor-network-rx-rate"]').text()).toBe('1.0 MiB/s')
    expect(wrapper.find('[data-test="monitor-network-row"] [data-test="monitor-network-tx-rate"]').text()).toBe('2.0 MiB/s')
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
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').text()).toContain('已在配置中关闭')
    expect(wrapper.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-swap-unavailable"]').text()).toContain('已在配置中关闭')
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('该指标将在后续版本接入')
    // 主机条仍可用（#362：原「系统信息」卡 → 主机条）
    expect(wrapper.find('[data-test="monitor-host"]').text()).toContain('trade-01')
    // 不得用 0 冒充 CPU 数值
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('0.0%')
  })

  it('快照尚未到达：整页等待空态，不渲染任何指标数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-waiting"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-waiting"]').text()).toContain('等待系统监控数据')
    expect(wrapper.find('[data-test="monitor-host"]').exists()).toBe(false)
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
    // 夹具刻意**不**携带 cpu.load_average 的 semantic_hints：真实发送方只在
    // semantic=processor_queue（Windows 代理量）时携带该提示（apps/sysmon/snapshot_builder.cpp），
    // Linux 原生负载不携带。此前的 'linux load average from /proc/loadavg' 夹具与契约不符。
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
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
    // 无提示时不得凭空造提示（#207 B2 的映射只在发送方真的携带 key 时生效）
    expect(wrapper.find('[data-test="monitor-load-hint"]').exists()).toBe(false)

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
    // 文案映射已收口到 composables/monitorLedger.unavailableReasonText（与台账共用）：
    // no_permission 现在给可读文案，不再是裸 reason
    expect(panel.text()).toContain('权限不足，无法读取该指标')
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
    // #207 B2：契约 key 命中 → 展示中文口径；发送方英文原文保留在 title（tooltip）里可对照
    const hint = wrapper.find('[data-test="monitor-load-hint"]')
    expect(hint.text()).toContain('语义提示：')
    expect(hint.text()).toContain('处理器队列长度')
    expect(hint.text()).not.toContain('processor queue length is not load average')
    expect(hint.attributes('title')).toBe('processor queue length is not load average')
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

    // #362：温度不再是一张独立卡片，而是 CPU 宽卡内「最高温度」那格 + 可展开的明细区
    const temperatureCell = wrapper.find('[data-test="monitor-temperature"]')
    expect(temperatureCell.exists()).toBe(true)
    expect(wrapper.find('.ds-card [data-test="monitor-temperature"]').exists()).toBe(true)
    // #126：芯片名做友好映射（coretemp → CPU），组内逐通道显示 label 与温度
    const groups = wrapper.findAll('[data-test="monitor-temperature-group"]')
    expect(groups).toHaveLength(1)
    expect(groups[0].attributes('data-chip')).toBe('coretemp')
    expect(wrapper.find('[data-test="monitor-temperature-group-name"]').text()).toBe('CPU')
    // 主数值（最高温）在那格里，逐传感器读数在展开的明细区里
    expect(temperatureCell.text()).toContain('45.5 °C')
    const detail = wrapper.find('[data-test="monitor-temperature-detail"]')
    expect(detail.exists()).toBe(true)
    expect(detail.text()).toContain('45.5 °C')
    expect(detail.text()).toContain('41.5 °C')
    const sensors = wrapper.findAll('[data-test="monitor-temperature-sensor"]')
    expect(sensors).toHaveLength(2)
    // 行内展示的是 label（组内按 label 字典序），组的友好名取代了原始标识；
    // 原始 sensor 标识降级到 title（tooltip），仍可查到。
    const rows = sensors.map(sensor => sensor.text())
    expect(rows).toEqual(['Core 041.5 °C', 'Package id 045.5 °C'])
    expect(detail.text()).not.toContain('coretemp#hwmon3')
    expect(sensors[0].attributes('title')).toBe('coretemp#hwmon3:Core 0')
    expect(sensors[1].attributes('title')).toBe('coretemp#hwmon3:Package id 0')

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

  it('#126 温度按芯片分组：友好名分组、CPU 在前 ACPI 在后、不过滤任何传感器', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      sensorData: true,
      temperatures: [
        // 刻意打乱发送顺序：ACPI 在前、CPU 在后，验证排序不依赖发送端顺序
        { sensor: 'acpitz_0#hwmon0:temp1', chip: 'acpitz_0', label: '', celsius: 27.8 },
        { sensor: 'amdgpu#hwmon4:edge', chip: 'amdgpu', label: 'edge', celsius: 52.0 },
        { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
        { sensor: 'iwlwifi_1_2#hwmon2:temp1', chip: 'iwlwifi_1_2', label: '', celsius: 38.0 },
      ],
    }))
    const wrapper = await mountView()

    // 芯片排序：CPU → GPU → 无线网卡 → 主板 ACPI
    const groups = wrapper.findAll('[data-test="monitor-temperature-group"]')
    expect(groups.map(group => group.attributes('data-chip')))
      .toEqual(['coretemp', 'amdgpu', 'iwlwifi_1_2', 'acpitz_0'])
    // 友好映射：coretemp → CPU、amdgpu → GPU、iwlwifi* → 无线网卡、acpitz* → 主板 ACPI
    expect(wrapper.findAll('[data-test="monitor-temperature-group-name"]').map(name => name.text()))
      .toEqual(['CPU', 'GPU', '无线网卡', '主板 ACPI'])

    // 不过滤任何传感器：四条读数全部渲染（ACPI 只是排在后面）
    const sensors = wrapper.findAll('[data-test="monitor-temperature-sensor"]')
    expect(sensors).toHaveLength(4)

    // label 缺失（空串）时回退通道名，不退化为空行
    const iwlwifiRow = sensors.find(row => row.attributes('data-sensor') === 'iwlwifi_1_2#hwmon2:temp1')
    expect(iwlwifiRow?.text()).toContain('temp1')
    expect(iwlwifiRow?.text()).toContain('38.0 °C')

    // 原始 sensor 标识在 tooltip（title）里仍可查到
    expect(sensors.map(row => row.attributes('title'))).toEqual([
      'coretemp#hwmon3:Package id 0',
      'amdgpu#hwmon4:edge',
      'iwlwifi_1_2#hwmon2:temp1',
      'acpitz_0#hwmon0:temp1',
    ])
  })

  it('#126 旧版本发送方（无 chip/label）：从 sensor 回退解析，仍分组且标签非空', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      sensorData: true,
      // 模拟旧版本 dzsysmon：只发 sensor + celsius
      temperatures: [
        { sensor: 'coretemp#hwmon3:Package id 0', celsius: 45.5 },
        { sensor: 'acpitz#hwmon0:temp1', celsius: 27.8 },
      ],
    }))
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-temperature-group"]').map(g => g.attributes('data-chip')))
      .toEqual(['coretemp', 'acpitz'])
    const sensors = wrapper.findAll('[data-test="monitor-temperature-sensor"]')
    expect(sensors).toHaveLength(2)
    expect(sensors[0].text()).toContain('Package id 0')
    expect(sensors[1].text()).toContain('temp1')  // 无 label 时回退通道名
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
    // 展示的是通道标签，原始标识在 title 里（#126）
    expect(sensors[0].attributes('title')).toBe('coretemp:Valid')
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

    // #120 收口：内存 / 交换使用率色标与卷行、顶栏共享同一组常量（默认 50% / 25% → normal）
    expect(wrapper.find('[data-test="monitor-memory"] .monitor-view__metric-row').attributes('data-level'))
      .toBe('normal')
    expect(swap.find('.monitor-view__list-row').attributes('data-level')).toBe('normal')
  })

  it('内存 / 交换使用率色标：0.70 → warn、0.90 → danger（边界含，数值照常渲染）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * GIB,
        available_bytes: 10 * GIB,
        used_bytes: 90 * GIB,
        used_ratio: 0.9,
        cached_bytes: null,
        buffers_bytes: null,
      },
      swap: { total_bytes: 100 * GIB, used_bytes: 70 * GIB, used_ratio: 0.7 },
    }))
    const wrapper = await mountView()

    const memoryRow = wrapper.find('[data-test="monitor-memory"] .monitor-view__metric-row')
    expect(memoryRow.attributes('data-level')).toBe('danger')
    // 色标只影响颜色，不改变数值展示
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('90.0%')

    const swapRow = wrapper.find('[data-test="monitor-memory-swap"] .monitor-view__list-row')
    expect(swapRow.attributes('data-level')).toBe('warn')
    expect(wrapper.find('[data-test="monitor-memory-swap-usage"]').text()).toBe('70.0%')
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

describe('MonitorView 磁盘（#75：卷容量 / 共享内存目录，可用性驱动）', () => {
  const GIB = 1024 ** 3
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照：3 个卷按固定顺序（ADR 0048）排列，根挂载点在最前', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(3)
    // ADR 0048：挂载深度升序 → 同深度按挂载点升序（/ 深度 0；/data、/home 深度 1）
    expect(rows[0].attributes('data-mount')).toBe('/')
    expect(rows[0].text()).toContain('40.0%')
    expect(rows[1].attributes('data-mount')).toBe('/data')
    expect(rows[1].text()).toContain('25.0%')

    expect(rows[2].attributes('data-mount')).toBe('/home')
    expect(rows[2].text()).toContain('62.0%')
    // #126：1 TiB 定标到 TiB 档，不再显示为 1024.0 GiB
    expect(rows[1].text()).toContain('256.0 GiB / 1.0 TiB')

    // 使用率均 < 70%：色标 normal（与顶栏卷徽章同源）
    expect(rows.map(row => row.attributes('data-level'))).toEqual(['normal', 'normal', 'normal'])

    // 卷集合与数量完全由快照驱动，无硬编码盘符
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-disk-zero-volumes"]').exists()).toBe(false)
  })

  it('卷使用率色标：0.69 → normal、0.70 → warn、0.90 → danger（边界含，百分比文本照常）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/low', available: true, total_bytes: 100 * GIB, used_bytes: 69 * GIB, used_ratio: 0.69 },
        { mount: '/warn', available: true, total_bytes: 100 * GIB, used_bytes: 70 * GIB, used_ratio: 0.7 },
        { mount: '/danger', available: true, total_bytes: 100 * GIB, used_bytes: 90 * GIB, used_ratio: 0.9 },
      ],
    }))
    const wrapper = await mountView()

    // ADR 0048：固定顺序（同深度按挂载点升序 /danger → /low → /warn），与使用率无关
    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows.map(row => row.attributes('data-mount'))).toEqual(['/danger', '/low', '/warn'])
    expect(rows.map(row => row.attributes('data-level'))).toEqual(['danger', 'normal', 'warn'])
    // 着色不改变数值展示：百分比 / 已用与总量照常渲染
    expect(rows[0].find('.monitor-view__list-value').text()).toBe('90.0%')
    expect(rows[1].find('.monitor-view__list-value').text()).toBe('69.0%')
    expect(rows[2].find('.monitor-view__list-value').text()).toBe('70.0%')
    expect(rows[0].find('.monitor-view__list-meta').text()).toBe('90.0 GiB / 100.0 GiB')
  })

  it('#122 无容量挂载：正文列表只留可读容量的卷，其余折叠进「无容量挂载（N）」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/run/user/1000/cliprdr-abc', available: true, total_bytes: 0, used_bytes: 0, used_ratio: null },
        { mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountView()

    // 正文只有容量可读且总量 > 0 的卷
    const rows = wrapper.findAll('[data-test="monitor-disk-volume"]')
    expect(rows).toHaveLength(1)
    expect(rows[0].attributes('data-mount')).toBe('/')

    // 折叠组给出数量，默认收起（展开与否是纯显示状态）
    const group = wrapper.find('[data-test="monitor-disk-zero-volumes"]')
    expect(group.exists()).toBe(true)
    expect(group.find('[data-test="monitor-disk-zero-toggle"]').text()).toContain('无容量挂载（2）')
    expect(group.find('[data-test="monitor-disk-zero-toggle"]').attributes('aria-expanded')).toBe('false')
    expect(wrapper.findAll('[data-test="monitor-disk-zero-volume"]')).toHaveLength(0)
  })

  it('#122 无容量挂载展开：逐条显示挂载点、占位与不可用原因', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/run/user/1000/cliprdr-abc', available: true, total_bytes: 0, used_bytes: 0, used_ratio: null },
        { mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-disk-zero-toggle"]').trigger('click')
    await wrapper.vm.$nextTick()

    const items = wrapper.findAll('[data-test="monitor-disk-zero-volume"]')
    expect(items).toHaveLength(2)
    // 按挂载点升序（比率为 null 的一律同优先级，见 store 排序）
    expect(items.map(item => item.attributes('data-mount'))).toEqual(['/backup', '/run/user/1000/cliprdr-abc'])

    // 不可读 → 值列「不可用」+ 原因文案；可读但总量为 0 → 值列「—」+ 总量说明。
    // 两行的值列都不含字节数或百分比（这正是折叠前 `0 B / 0 B` 要消失的噪声）。
    const values = items.map(item => item.find('.monitor-view__list-value').text())
    expect(values).toEqual(['不可用', '—'])
    expect(items[0].find('[data-test="monitor-disk-zero-reason"]').text()).toBe('无访问权限')
    expect(items[1].find('[data-test="monitor-disk-zero-reason"]').text()).toBe('总容量为 0')

    const group = wrapper.find('[data-test="monitor-disk-zero-volumes"]')
    // 展开后按钮语义同步
    expect(group.find('[data-test="monitor-disk-zero-toggle"]').attributes('aria-expanded')).toBe('true')
  })

  it('卷可读性脏数据（NaN/负数）：折叠进无容量分组并显示「采集失败」，不显示 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/dirty', available: true, total_bytes: Number.NaN, used_bytes: -1, used_ratio: 0 },
      ],
    }))
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(0)
    await wrapper.find('[data-test="monitor-disk-zero-toggle"]').trigger('click')
    await wrapper.vm.$nextTick()

    const row = wrapper.find('[data-test="monitor-disk-zero-volume"]')
    expect(row.find('.monitor-view__list-value').text()).toBe('不可用')
    expect(row.find('[data-test="monitor-disk-zero-reason"]').text()).toBe('采集失败')
  })

  it('volumes 不可用：显示不可用 + 原因空态，不渲染卷条目', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { volumes: { available: false, reason: 'disabled_by_config' } },
    }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-test="monitor-disk-unavailable"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('不可用')
    // 空态沿用既有 unavailableDesc：disabled_by_config 映射为「已在配置中关闭」
    expect(empty.text()).toContain('已在配置中关闭')
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(0)
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
    // 同上：no_permission 由共享映射给出可读文案
    expect(unavailable.text()).toContain('权限不足，无法读取该指标')
    expect(wrapper.find('[data-test="monitor-shm-path"]').exists()).toBe(false)

    // 能力粘性可用但本帧对象缺失 → 「暂无数据」而不是「不可用」
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ shmDir: null }))
    await wrapper.vm.$nextTick()
    const empty = wrapper.find('[data-test="monitor-shm-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-shm-unavailable"]').exists()).toBe(false)
  })

  it('可用但 total_bytes=0 的特殊卷：折叠进无容量分组并显示「—」，不占正文也不报不可用', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/special', available: true, total_bytes: 0, used_bytes: 0, used_ratio: null },
      ],
    }))
    const wrapper = await mountView()

    // 正文没有可读容量的卷：不显示「不可用」空态（volumes 指标本身可用），只剩折叠分组
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(false)
    const group = wrapper.find('[data-test="monitor-disk-zero-volumes"]')
    expect(group.exists()).toBe(true)

    await group.find('[data-test="monitor-disk-zero-toggle"]').trigger('click')
    await wrapper.vm.$nextTick()
    const row = wrapper.find('[data-test="monitor-disk-zero-volume"]')
    expect(row.attributes('data-mount')).toBe('/special')
    // 比率为 null 但容量确实为 0：值列是「—」（不是 0%），原因列说明总量为 0
    expect(row.find('.monitor-view__list-value').text()).toBe('—')
    expect(row.find('[data-test="monitor-disk-zero-reason"]').text()).toBe('总容量为 0')
  })

  it('不按 platform.family 分支：Windows 形状卷（C: / D:）照常按数据渲染', async () => {
    const snapshot = makeSysmonSnapshot({
      volumes: [
        { mount: 'C:', available: true, total_bytes: 500 * GIB, used_bytes: 250 * GIB, used_ratio: 0.5 },
        { mount: 'D:', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
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
    expect(rows).toHaveLength(1)
    expect(rows[0].attributes('data-mount')).toBe('C:')
    expect(rows[0].text()).toContain('50.0%')
    // 不可读的 D: 折叠进无容量分组（#122），正文只剩 C:
    const group = wrapper.find('[data-test="monitor-disk-zero-volumes"]')
    expect(group.find('[data-test="monitor-disk-zero-toggle"]').text()).toContain('无容量挂载（1）')
    await group.find('[data-test="monitor-disk-zero-toggle"]').trigger('click')
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-zero-volume"]').attributes('data-mount')).toBe('D:')
    expect(wrapper.find('[data-test="monitor-shm-path"]').text()).toBe('C:\\dztrader\\shm')
    expect(wrapper.find('[data-test="monitor-shm-oldest-age"]').text()).toBe('30 秒')
  })

  it('WS 推送更新：卷数量随快照刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(3)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/only', available: true, total_bytes: 10 * GIB, used_bytes: 9 * GIB, used_ratio: 0.9 },
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

    // #207 B2：disk_io 是契约定义的稳定 key → 展示中文口径；原文只作为 tooltip 保留
    const hint = wrapper.find('[data-test="monitor-disk-io-hint"]')
    expect(hint.exists()).toBe(true)
    expect(hint.text()).toContain('语义提示：')
    expect(hint.text()).toContain('队列深度')
    expect(hint.text()).not.toContain('windows counters are per physical disk')
    expect(hint.attributes('title')).toBe('windows counters are per physical disk')

    // 防御：payload 缺省 semantic_hints（前向兼容/发送方未提供）时不渲染提示行
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ semanticHints: {} }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-hint"]').exists()).toBe(false)

    // 默认夹具（#76 后端形状）自带 disk_io 口径提示：恢复后应重新显示
    useSysmonStore().applySnapshot(makeSysmonSnapshot())
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-disk-io-hint"]').exists()).toBe(true)
  })
})

describe('MonitorView 网络区块（#77 网络与虚拟网卡过滤）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    localStorage.setItem('jwt_token', 'test-token')
  })

  it('默认快照（单块物理网卡）：每网卡渲染收发速率 / 累计字节包 / 错误丢弃，且不显示虚拟标记', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-network-row"]')
    expect(rows).toHaveLength(1)
    expect(rows[0].attributes('data-interface')).toBe('eth0')
    expect(rows[0].find('[data-test="monitor-network-rx-rate"]').text()).toBe('1.0 MiB/s')
    expect(rows[0].find('[data-test="monitor-network-tx-rate"]').text()).toBe('2.0 MiB/s')
    expect(rows[0].find('[data-test="monitor-network-rx-bytes"]').text()).toBe('8.0 GiB')
    expect(rows[0].find('[data-test="monitor-network-tx-bytes"]').text()).toBe('4.0 GiB')
    expect(rows[0].find('[data-test="monitor-network-rx-packets"]').text()).toBe('12,000,000')
    expect(rows[0].find('[data-test="monitor-network-tx-packets"]').text()).toBe('9,000,000')
    // 速率 0 是合法真实值（本窗口确实没有增量），显示 0.0/s 而不是「不可用」；
    // 错误/丢弃单元格并列累计量（累计计数器没有"不可用"语义）
    expect(rows[0].find('[data-test="monitor-network-rx-errors"]').text()).toBe('0.0/s（累计 3）')
    expect(rows[0].find('[data-test="monitor-network-tx-drops"]').text()).toBe('0.0/s（累计 2）')
    // 物理网卡不带「虚拟」标记
    expect(rows[0].find('[data-test="monitor-network-virtual"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-network-hidden-all"]').exists()).toBe(false)
  })

  it('全部可用时按 interface 升序排序，不依赖发送端顺序', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ virtualNetwork: true }))
    const wrapper = await mountView()

    // 夹具发送顺序为 veth1234、docker0、lo、eth0；前端按 interface localeCompare 重排
    expect(wrapper.findAll('[data-test="monitor-network-row"]').map(row => row.attributes('data-interface')))
      .toEqual(['docker0', 'eth0', 'lo', 'veth1234'])
  })

  it('虚拟网卡显式标注「虚拟」；前端不自行过滤（过滤是发送方义务，契约 §2.4）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ virtualNetwork: true }))
    const wrapper = await mountView()

    const tags = wrapper.findAll('[data-test="monitor-network-virtual"]')
    expect(tags).toHaveLength(3)
    expect(tags.every(tag => tag.text() === '虚拟')).toBe(true)
    const rowsWithTag = wrapper.findAll('[data-test="monitor-network-row"]')
      .filter(row => row.find('[data-test="monitor-network-virtual"]').exists())
      .map(row => row.attributes('data-interface'))
    expect(rowsWithTag).toEqual(['docker0', 'lo', 'veth1234'])
  })

  it('速率 null（首次采样/计数器重置）逐项显示「不可用」，不得用 0 冒充', async () => {
    const eth0 = makeSysmonSnapshot({ resourceData: true }).network![0]!
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      network: [{
        ...eth0,
        rx_bytes_per_sec: null,
        tx_bytes_per_sec: null,
        rx_errors_per_sec: null,
        tx_errors_per_sec: null,
        rx_drops_per_sec: null,
        tx_drops_per_sec: null,
      }],
    }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-test="monitor-network-row"]')
    // 吞吐速率没有累计量可并列：null 就是「不可用」
    for (const field of ['rx-rate', 'tx-rate']) {
      expect(row.find(`[data-test="monitor-network-${field}"]`).text()).toBe('不可用')
    }
    // 错误/丢弃速率 null 显示「不可用」，但不隐瞒累计量（累计计数器没有"不可用"语义）
    for (const field of ['rx-errors', 'tx-errors', 'rx-drops', 'tx-drops']) {
      expect(row.find(`[data-test="monitor-network-${field}"]`).text()).toContain('不可用')
      expect(row.find(`[data-test="monitor-network-${field}"]`).text()).toContain('累计')
    }
    // 速率不可用时累计量仍照常显示（累计计数器没有"不可用"语义）
    expect(row.find('[data-test="monitor-network-rx-bytes"]').text()).toBe('8.0 GiB')
    expect(row.text()).not.toContain('0 B/s')
    expect(row.text()).not.toContain('0.0/s')
  })

  it('全被 hide_virtual 滤掉（空数组 + available=true）：显示「已隐藏全部虚拟网卡」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: [] }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-network-hidden-all"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-hidden-all"]').text()).toContain('已隐藏全部虚拟网卡')
    // 契约 §2.5 明文：全部被过滤 ≠ 不可用，也不是「暂无数据」的空态
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-network-empty"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
  })

  it('hide_virtual=false 时空数组 = 本机确实无网卡：显示「暂无数据」而不是「已隐藏」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: [] }))
    const wrapper = await mountView()
    // 生效配置显示全部网卡（hide_virtual=false）：空数组不能再解释成"被过滤"
    useSysmonStore().applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    await wrapper.vm.$nextTick()

    expect(wrapper.find('[data-test="monitor-network-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-hidden-all"]').exists()).toBe(false)
  })

  it('能力可用但本帧未采集（network=null）：同样走「暂无数据」，不改成「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: null }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-network-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
  })

  it('能力不可用（disabled_by_config / collect_failed）：整块「不可用」+ 原因，不渲染网卡行与开关', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { network: { available: false, reason: 'disabled_by_config' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-test="monitor-network-unavailable"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.text()).toContain('不可用')
    expect(unavailable.text()).toContain('已在配置中关闭')
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
    // 能力不可用时开关无意义，不渲染
    expect(wrapper.find('[data-test="monitor-network-show-all"]').exists()).toBe(false)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { network: { available: false, reason: 'collect_failed' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').text()).toContain('采集失败，暂不可用')
  })
})

describe('MonitorView「显示全部网卡」开关（#77 走配置即时生效）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
    localStorage.setItem('jwt_token', 'test-token')
  })

  it('开关初值来自生效配置镜像：hide_virtual=true → 「显示全部网卡」关闭', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    useSysmonStore().applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()
    const input = wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]')
    expect((input.element as HTMLInputElement).checked).toBe(false)
  })

  it('hide_virtual=false → 开关打开（显示全部网卡）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    useSysmonStore().applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    await wrapper.vm.$nextTick()
    const input = wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]')
    expect((input.element as HTMLInputElement).checked).toBe(true)
  })

  it('admin 打开开关：下发 PUT network.hide_virtual=false，pending 保持到 WS sysmon_config 到达', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true, message: 'dispatched' })
    const wrapper = await mountView()
    setRole('admin')
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()

    await wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]').setValue(true)
    await flushPromises()

    expect(sysmonApi.setConfig).toHaveBeenCalledWith({ network: { hide_virtual: false } })
    // REST 只表示已下发：开关保持 pending，值仍是配置镜像里的旧值（无乐观更新）
    expect(store.hideVirtualPending).toBe(true)
    expect(store.hideVirtual).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-show-all"] input').element)
      .toHaveProperty('disabled', true)

    // WS sysmon_config 到达才是生效信号：镜像更新 + pending 清除
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    await flushPromises()
    expect(store.hideVirtual).toBe(false)
    expect(store.hideVirtualPending).toBe(false)
    expect((wrapper.find('[data-test="monitor-network-show-all"] input').element as HTMLInputElement).checked).toBe(true)
  })

  it('非 admin：开关渲染但 disabled，点击不下发', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()
    setRole('user')
    useSysmonStore().applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()

    const input = wrapper.find('[data-test="monitor-network-show-all"] input')
    expect(input.element).toHaveProperty('disabled', true)
    await input.trigger('change')
    expect(sysmonApi.setConfig).not.toHaveBeenCalled()
  })

  it('HTTP 失败：pending 立即清除，开关回弹到配置镜像的真实值', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 403'))
    const wrapper = await mountView()
    setRole('admin')
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()

    await wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]').setValue(true)
    await flushPromises()

    expect(store.hideVirtualPending).toBe(false)
    expect(store.hideVirtual).toBe(true)
    expect((wrapper.find('[data-test="monitor-network-show-all"] input').element as HTMLInputElement).checked).toBe(false)
  })

  it('HTTP 失败：给出可见的失败提示，不静默失败', async () => {
    // 开关点击后界面无变化且无提示 = 用户无法判断是否生效（静默失败）。
    // 结果契约：false = HTTP 失败/防重入 → view 弹 error toast。
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()
    setRole('admin')
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()
    const toastSpy = vi.spyOn(useToastStore(), 'error')

    await wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]').setValue(true)
    await flushPromises()

    expect(toastSpy).toHaveBeenCalledTimes(1)
    expect(toastSpy.mock.calls[0][0]).toContain('失败')
    expect(store.hideVirtualPending).toBe(false)
  })

  it('防重入：pending 中的重复点击不再下发，也不误报失败', async () => {
    // setHideVirtual 在 pending 中直接返回 false（不重复 HTTP）；此时不应弹失败提示，
    // 否则用户连续点击会看到虚假错误。用永挂起的 Promise 制造 pending 窗口。
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    let release: (() => void) | undefined
    vi.mocked(sysmonApi.setConfig).mockImplementation(
      () => new Promise<void>((resolve) => { release = resolve }) as never,
    )
    const wrapper = await mountView()
    setRole('admin')
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    await wrapper.vm.$nextTick()
    const toastSpy = vi.spyOn(useToastStore(), 'error')

    const input = wrapper.find('[data-test="monitor-network-show-all"] input[type="checkbox"]')
    await input.setValue(true)
    await wrapper.vm.$nextTick()
    expect(store.hideVirtualPending).toBe(true)

    // pending 期间 SyncSwitch 自身 disabled；直接调 setHideVirtual 验证 store 层防重入
    const second = await store.setHideVirtual(false)
    expect(second).toBe(false)
    expect(sysmonApi.setConfig).toHaveBeenCalledTimes(1)
    expect(toastSpy).not.toHaveBeenCalled()

    release?.()
    await flushPromises()
  })
})

describe('MonitorView 配置 tab（#81 展示生效配置 + admin 编辑入口）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
    localStorage.setItem('jwt_token', 'test-token')
  })

  it('顶部新增第三个 tab「配置」：默认实时，切到配置后只显示配置视图', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const tab = wrapper.find('[data-test="monitor-tab-config"]')
    expect(tab.exists()).toBe(true)
    expect(tab.text()).toBe('配置')
    expect(tab.attributes('aria-pressed')).toBe('false')

    await tab.trigger('click')
    expect(tab.attributes('aria-pressed')).toBe('true')
    // 配置 tab 用 v-show 常驻（#322：卸载即丢草稿），显隐由根元素的内联 display 表达。
    // 因此直接断内联样式，不用测试辅助的可见性判断（同 TradingDataView.spec.ts 的做法）。
    const configPanel = () => wrapper.find('[data-test="sysmon-config-tab"]').element as HTMLElement
    expect(wrapper.find('[data-test="sysmon-config-tab"]').exists(), '找不到配置面板').toBe(true)
    expect(configPanel().style.display).toBe('')
    // 切换只换渲染来源：实时网格不再渲染（store 未卸载，镜像与 WS 连接保持）
    expect(wrapper.find('[data-test="monitor-host"]').exists()).toBe(false)

    // 切回实时恢复正常渲染：元素仍在 DOM 里，但被 display:none 隐藏
    await wrapper.find('[data-test="monitor-tab-realtime"]').trigger('click')
    expect(configPanel().style.display).toBe('none')
    expect(wrapper.find('[data-test="monitor-host"]').exists()).toBe(true)
  })

  it('init 经 GET /api/sysmon/config 补拉配置镜像：配置 tab 直接展示生效值', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig({ realtime: { push_interval_sec: 10 } }))
    const wrapper = await mountView()

    expect(sysmonApi.getConfig).toHaveBeenCalledTimes(1)
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')
    expect(wrapper.find('[data-test="sysmon-config-value-realtime.push_interval_sec"]').text()).toBe('10 秒')
  })

  it('配置镜像未到达：配置 tab 显示「配置尚未到达」空态（不显示假默认值，也不弹错）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockRejectedValue(new Error('HTTP 503'))
    const errorSpy = vi.spyOn(useToastStore(), 'error')
    const wrapper = await mountView()

    expect(sysmonApi.getConfig).toHaveBeenCalledTimes(1)
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')
    expect(wrapper.find('[data-test="sysmon-config-unavailable"]').text()).toContain('配置尚未到达')
    expect(errorSpy).not.toHaveBeenCalled()
  })

  it('admin 可见编辑入口（保存按钮）；WS sysmon_config 到达后展示值更新', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()
    setRole('admin')
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig())
    await wrapper.vm.$nextTick()
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')

    expect(wrapper.find('[data-test="sysmon-config-save"]').exists()).toBe(true)
    // #207 D1：admin 的只读摘要默认折叠，展开后仍展示生效值并可对照
    await wrapper.find('[data-test="sysmon-config-summary-toggle"]').trigger('click')
    expect(wrapper.find('[data-test="sysmon-config-value-realtime.push_interval_sec"]').text()).toBe('5 秒')

    // WS 全量回推（重连快照域 / SET 成功后的 RTN）：展示跟随镜像更新
    store.applyConfig(makeSysmonConfig({ realtime: { push_interval_sec: 30 } }))
    await flushPromises()
    expect(wrapper.find('[data-test="sysmon-config-value-realtime.push_interval_sec"]').text()).toBe('30 秒')
  })

  it('非 admin：配置 tab 只读（无保存按钮，显示仅管理员可修改）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()
    setRole('user')
    useSysmonStore().applyConfig(makeSysmonConfig())
    await flushPromises()
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')

    expect(wrapper.find('[data-test="sysmon-config-readonly"]').text()).toContain('仅管理员可修改')
    expect(wrapper.find('[data-test="sysmon-config-save"]').exists()).toBe(false)
  })
})

// #322：配置草稿不再随视图切换丢弃（v-show 常驻），但离开路由仍会丢——由路由离开守卫拦一次。
// 守卫必须在 <router-view> 渲染的组件里才生效，因此这一组用例挂真实 router（memory history），
// 而不是复用上面直接 mount(MonitorView) 的 mountView()。
describe('MonitorView 配置草稿守卫（#322）', () => {
  const RouteHost = { template: '<router-view />' }

  // 与其余 describe 同款隔离：本组用例共享 router / store，不能依赖上一个 describe 的残留
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
    localStorage.setItem('jwt_token', 'test-token')
  })

  async function mountWithRouter() {
    const { createRouter, createMemoryHistory } = await import('vue-router')
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/monitor', name: 'monitor', component: MonitorView },
        { path: '/logs', name: 'logs', component: { template: '<div data-test="logs-page" />' } },
        // 登出目标：守卫对会话结束类导航放行（见「登出导航直接放行」用例）
        { path: '/login', name: 'login', component: { template: '<div data-test="login-page" />' } },
      ],
    })
    await router.push('/monitor')
    await router.isReady()
    const wrapper = mount(RouteHost, { global: { plugins: [router], stubs } })
    await flushPromises()
    setRole('admin')
    useSysmonStore().applyConfig(makeSysmonConfig())
    await flushPromises()
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')
    return { wrapper, router }
  }

  it('改了配置后切视图再切回：草稿仍在（v-show 常驻，不重挂载）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper } = await mountWithRouter()

    const input = wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]')
    expect(input.exists()).toBe(true)
    await input.setValue('2000')
    expect((input.element as HTMLInputElement).value).toBe('2000')

    await wrapper.find('[data-test="monitor-tab-realtime"]').trigger('click')
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')

    const after = wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]')
    expect((after.element as HTMLInputElement).value).toBe('2000')
  })

  it('有未保存改动时离开路由：弹确认框；选「留下」留在本页且草稿不变', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('2000')
    await flushPromises()

    // 不 await：守卫挂起导航，等用户在对话框里选择后才 resolve
    void router.push('/logs')
    await flushPromises()

    expect(wrapper.find('.ds-dialog__title').text()).toBe('有未保存的改动')
    const cancel = wrapper.find('[data-test="monitor-leave-cancel"]')
    expect(cancel.exists()).toBe(true)
    await cancel.trigger('click')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/monitor')
    expect(wrapper.find('.ds-dialog').exists()).toBe(false)
    const input = wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]')
    expect((input.element as HTMLInputElement).value).toBe('2000')
  })

  it('有未保存改动时离开路由：选「离开」正常导航', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('2000')
    await flushPromises()

    void router.push('/logs')
    await flushPromises()

    await wrapper.find('[data-test="monitor-leave-confirm"]').trigger('click')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/logs')
    expect(wrapper.find('[data-test="logs-page"]').exists()).toBe(true)
  })

  it('确认框被关闭（X / Esc）等同「留下」：不导航且草稿不变', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('2000')
    await flushPromises()

    void router.push('/logs')
    await flushPromises()
    expect(wrapper.find('.ds-dialog').exists()).toBe(true)

    // Modal 的 @close 走 cancelLeave：遮罩点击 / Esc / header 关闭按钮都是「留下」
    await wrapper.find('.ds-dialog__close').trigger('click')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/monitor')
    expect(wrapper.find('.ds-dialog').exists()).toBe(false)
    const input = wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]')
    expect((input.element as HTMLInputElement).value).toBe('2000')
  })

  it('草稿被生效镜像回退（dirty 变回 false）后离开：不再弹框', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('2000')
    await flushPromises()
    // WS sysmon_config 回推（校验失败时发送方回推旧值）：表单随镜像回退，dirty 归零
    useSysmonStore().applyConfig(makeSysmonConfig())
    await flushPromises()

    await router.push('/logs')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/logs')
    expect(wrapper.find('.ds-dialog').exists()).toBe(false)
  })

  it('登出导航直接放行：会话已清除，不拦也不弹框', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('2000')
    await flushPromises()

    // Sidebar 的登出是「先 auth.logout() 再 push('/login')」：此时本页已在 AppShell 里
    // 且会话已失效，再拦下来会把用户停在已登出的私有页上（选「留下」毫无意义）。
    void router.push('/login')
    await flushPromises()

    expect(router.currentRoute.value.name).toBe('login')
    expect(wrapper.find('.ds-dialog').exists()).toBe(false)
  })

  it('配置镜像未到达（503 / WS 未推送）时离开：不弹框（空表单不得算成有改动）', async () => {
    // 空表单会让组件内的 dirty 判成「有改动」（解析不出数字）；守卫必须只在镜像就绪后才认它，
    // 否则 dzsysmon 未运行期间每次离开监控页都会误报。
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockRejectedValue(new Error('HTTP 503'))
    const { createRouter, createMemoryHistory } = await import('vue-router')
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/monitor', name: 'monitor', component: MonitorView },
        { path: '/logs', name: 'logs', component: { template: '<div data-test="logs-page" />' } },
      ],
    })
    await router.push('/monitor')
    await router.isReady()
    const wrapper = mount(RouteHost, { global: { plugins: [router], stubs } })
    await flushPromises()
    expect(useSysmonStore().config, '本用例要求镜像未到达').toBeNull()
    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')

    void router.push('/logs')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/logs')
  })

  it('无未保存改动时离开路由：不弹确认框，直接导航', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const { wrapper, router } = await mountWithRouter()

    await router.push('/logs')
    await flushPromises()

    expect(router.currentRoute.value.path).toBe('/logs')
    expect(wrapper.find('.ds-dialog').exists()).toBe(false)
  })
})

describe('MonitorView 视图切换语义（#323：分段按钮组，不是 ARIA tabs）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
    localStorage.setItem('jwt_token', 'test-token')
  })

  it('本控件不声明 tabs 角色：无 role=tablist/tab、无 aria-controls，选中态用 aria-pressed', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    // 声明 tabs 语义却不提供 tabpanel / aria-controls / 方向键 / roving tabindex，
    // 比不做更糟（读屏按 tabs 报语义、键盘按按钮走）。退回普通按钮组。
    // absence 断言限定在本控件内：同页其它控件将来合法使用 aria-controls 不该误红。
    const switchEl = wrapper.find('[data-test="monitor-view-switch"]')
    expect(switchEl.exists(), '找不到视图切换控件').toBe(true)
    expect(switchEl.find('[role="tablist"]').exists()).toBe(false)
    expect(switchEl.find('[role="tab"]').exists()).toBe(false)
    expect(switchEl.find('[aria-controls]').exists()).toBe(false)
    expect(switchEl.find('[aria-selected]').exists()).toBe(false)

    // 「可访问名称 + button 角色 + pressed 状态」三件套（不只断 DOM 结构）
    const NAMES: Record<string, string> = { realtime: '实时', history: '历史', config: '配置' }
    for (const mode of ['realtime', 'history', 'config']) {
      const btn = switchEl.find(`[data-test="monitor-tab-${mode}"]`)
      expect(btn.element.tagName).toBe('BUTTON')
      expect(btn.attributes('type')).toBe('button')
      expect(btn.text()).toBe(NAMES[mode])
      expect(btn.attributes('aria-pressed')).toBe(mode === 'realtime' ? 'true' : 'false')
      expect(btn.attributes('tabindex')).toBeUndefined()
    }

    await wrapper.find('[data-test="monitor-tab-history"]').trigger('click')
    expect(wrapper.find('[data-test="monitor-tab-realtime"]').attributes('aria-pressed')).toBe('false')
    expect(wrapper.find('[data-test="monitor-tab-history"]').attributes('aria-pressed')).toBe('true')
  })
})

// #325：实时视图信息呈现——卡片等高留白 / 「实时快照」标签形态 / 可点数值可发现性。
describe('MonitorView 实时视图信息呈现（#325）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('5 处可点标量带 title 与 aria-label（不再只靠 hover 下划线）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    // 独立 oracle：指标名按用户可见口径手抄（不从被测代码取，避免自证）。
    const cases = [
      ['[data-test="monitor-cpu-value"]', 'CPU 使用率'],
      ['[data-test="monitor-load-one"]', '系统负载（1 分钟）'],
      ['[data-test="monitor-memory-value"]', '内存使用率'],
      ['[data-test="monitor-memory-swap-usage"]', '交换使用率'],
      ['[data-test="monitor-shm-total"] button', '共享内存目录占用'],
    ] as const
    for (const [selector, label] of cases) {
      const el = wrapper.find(selector)
      expect(el.exists(), selector).toBe(true)
      expect(el.attributes('title'), selector).toBe(`查看${label}历史变化`)
      const aria = el.attributes('aria-label') ?? ''
      // WCAG 2.5.3 Label in Name：aria-label 必须包含可见文本（按钮内就是当前数值）
      expect(aria, selector).toContain(el.text())
      expect(aria, selector).toContain(`查看${label}历史变化`)
      // 仍是按钮（键盘可达），无障碍属性不引入新元素
      expect(el.element.tagName, selector).toBe('BUTTON')
    }
  })

  it('Windows processor_queue 分支同样带 title 与 aria-label', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuLoadAverage: { one: 3.25, five: null, fifteen: null, semantic: 'processor_queue' },
    }))
    const wrapper = await mountView()

    const el = wrapper.find('[data-test="monitor-load-queue"]')
    expect(el.exists()).toBe(true)
    expect(el.attributes('title')).toBe('查看系统负载（1 分钟）历史变化')
    expect(el.attributes('aria-label')).toContain('3.25')
    expect(el.attributes('aria-label')).toContain('查看系统负载（1 分钟）历史变化')
  })

  it('实时快照状态降级为状态点 + 小字，且不与视图切换按钮同排', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const state = wrapper.find('.monitor-view__heading .monitor-view__snapshot-state')
    expect(state.exists()).toBe(true)
    expect(state.text()).toContain('实时快照')
    // 不再是 ds-tag 药丸族（原形态在 390 宽与三个 Tab 同排、看起来像第 4 个 Tab）
    expect(state.classes()).not.toContain('ds-tag')
    const dot = state.find('.monitor-view__snapshot-dot')
    expect(dot.exists()).toBe(true)
    expect(dot.attributes('aria-hidden')).toBe('true')
    // 结构上不属于 header 直接子级 / 视图切换组 → 不与 Tab 处于同一视觉行
    expect(wrapper.find('.monitor-view__header > .monitor-view__snapshot-state').exists()).toBe(false)
    expect(wrapper.find('.monitor-view__view-switch .monitor-view__snapshot-state').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-subtitle"]').exists()).toBe(true)
  })

  it('等待快照态用警示色状态点，且副标题不重复「等待」语义', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    const state = wrapper.find('.monitor-view__snapshot-state')
    expect(state.exists()).toBe(true)
    expect(state.text()).toContain('等待快照')
    expect(state.classes()).toContain('monitor-view__snapshot-state--waiting')
    expect(state.find('.monitor-view__snapshot-dot').attributes('aria-hidden')).toBe('true')
    // 实时视图的等待语义只由状态行承担（副标题不重复同一句话）
    expect(wrapper.find('[data-test="monitor-subtitle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-waiting"]').exists()).toBe(true)
  })
})

// #328：窄屏页头结构——标题组与 Tab 行分属 topbar 的两个子级（CSS 再决定同行 / 拆行 + 吸顶）。
describe('MonitorView 响应式页头结构（#328）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('视图切换与标题组同属 topbar，但不在 header 内部（拆行由 CSS <md 块决定）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('.monitor-view__topbar [data-test="monitor-view-switch"]').exists()).toBe(true)
    expect(wrapper.find('.monitor-view__header [data-test="monitor-view-switch"]').exists()).toBe(false)
    // 标题组仍在 header 内
    expect(wrapper.find('.monitor-view__header .monitor-view__heading').exists()).toBe(true)
  })
})

describe('MonitorView 主机条与 CPU 宽卡（#362）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('主机条字段与快照一一对应：主机名 / 发行版 / 内核 / 运行时长 / CPU 型号 · 逻辑核数', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.exists()).toBe(true)
    const text = host.text()
    expect(text).toContain('trade-01')
    // 发行版来自平台的 platform_description（夹具 = Ubuntu 24.04.1 LTS (kernel …)）
    expect(text).toContain('Ubuntu 24.04.1 LTS')
    expect(text).toContain('6.8.0-45-generic')   // platform.kernel
    expect(text).toContain('01:01:01')            // uptime_sec = 3661
    expect(text).toContain('AMD Ryzen 9 7950X 16-Core Processor')
    expect(text).toContain('16 逻辑核')
    expect(text).toContain('实时')
    // 系统信息卡已从实时视图消失（其内容并入主机条）
    expect(wrapper.find('[data-test="monitor-system"]').exists()).toBe(false)
    // 温度卡同样消失：温度只在 CPU 宽卡内占一格（本夹具无传感器，故该格整块不渲染）
    expect(wrapper.findAll('.ds-card [data-test="monitor-temperature"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-temperature"]').exists()).toBe(false)
  })

  it('主机条无可显示值的字段显示 — 而不是空白（平台描述 / 内核为空串）', async () => {
    const snapshot = makeSysmonSnapshot({ resourceData: true })
    snapshot.availability.platform_description = ''
    snapshot.platform.kernel = ''
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    const fields = wrapper.findAll('[data-test="monitor-host"] .monitor-view__host-field')
    // 发行版与内核各自退化为 —（不是空串撑出的空白字段）
    expect(fields[0].text()).toBe('—')
    expect(fields[1].text()).toBe('内核 —')
    // 有值的字段照常（不因某一项缺失而整行占位）
    expect(wrapper.find('[data-test="monitor-host"]').text()).toContain('trade-01')
  })

  it('四数字各带一行小字上下文：逻辑核 / 5 分 15 分量程 / 型号标注 / 芯片与传感器数', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ sensorData: true }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[data-test="monitor-cpu-context"]').text()).toBe('16 逻辑核 · 无核饱和')

    expect(wrapper.find('[data-test="monitor-load-context"]').text()).toBe('5 分 0.51 · 15 分 0.63（16 逻辑核量程）')

    // 型号字符串里没有 GHz 标注时退回「—」，不拿当前频率冒充型号标注
    expect(wrapper.find('[data-test="monitor-frequency-context"]').text()).toBe('型号标注 — · 16 核同步')
    expect(wrapper.find('[data-test="monitor-frequency-value"]').text()).toBe('5100 MHz')

    // 温度卡不再是独立卡片：它的主数值与明细都收进 CPU 宽卡内的那一格
    const temperature = wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-temperature"]')
    expect(temperature.exists()).toBe(true)
    expect(temperature.find('[data-test="monitor-temperature-max"]').text()).toBe('45.5 °C')
    expect(temperature.find('[data-test="monitor-temperature-summary"]').text())
      .toContain('2 个传感器 · 1 个芯片')
  })

  it('饱和核计数进入占用率上下文（饱和是快照事实，不在前端重新判定）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ saturatedCores: [2, 5] }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-context"]').text()).toBe('16 逻辑核 · 2 核饱和')
  })

  it('每核条形数量跟随快照（16 → 8 → 4），不受折叠影响', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(16) }))
    const wrapper = await mountView()
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(8) }))
    await wrapper.vm.$nextTick()
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(8)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(4) }))
    await wrapper.vm.$nextTick()
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(4)
  })

  it('饱和核除颜色外还有文字差异（灰度截屏可辨）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ saturatedCores: [3] }))
    const wrapper = await mountView()

    const cores = wrapper.findAll('[data-test="monitor-core"]')
    const saturated = cores.find(core => core.attributes('data-core-index') === '3')
    expect(saturated?.find('[data-test="monitor-core-saturated"]').text()).toBe('饱和')
    // 着色也不只靠颜色：饱和修饰类是两个状态 token 的分支点
    expect(saturated?.find('.monitor-view__core-bar-fill--saturated').exists()).toBe(true)
    expect(cores[0].find('.monitor-view__core-bar-fill--saturated').exists()).toBe(false)
    // 柱高 = 该核占用率（不是固定满格）：用内联 CSS 变量驱动，两核的高度不同
    expect(saturated?.find('.monitor-view__core-bar-fill').attributes('style')).toContain('--core-fill')
    expect(cores[0].find('.monitor-view__core-bar-fill').attributes('style')).toContain('--core-fill')
  })
})
