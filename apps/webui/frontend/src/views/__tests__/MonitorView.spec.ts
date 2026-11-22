import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { enableAutoUnmount, mount, flushPromises, type DOMWrapper } from '@vue/test-utils'
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

// #371：本页现在带一个 1 秒的数据龄定时器（停推时读数要自己走秒）。用例从不卸载 wrapper，
// 未卸载的定时器会在后续用例的渲染 flush 里插入无主的更新，把「apply 之后 nextTick」的断言
// 搅成偶发假红（实测 5 次里 2 次）。让 @vue/test-utils 在每条用例后统一卸载，定时器随
// onUnmounted 一起清。
enableAutoUnmount(afterEach)

async function mountView() {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  // #363 起维度行（卷 / 盘 / 网卡）平铺进台账，不需要先展开就能断言数据；
  // #435 起 CPU 块没有折叠区（温度明细在 title / aria），挂载即最终形态。
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
    // 钉住「此刻」：状态槽的「更新于」按当日/跨日两种形态渲染，不钉就会随真实日期漂移
    vi.spyOn(Date, 'now').mockReturnValue(Date.UTC(2026, 9, 21, 15, 0, 2))
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuUsageRatio: 0.187, resourceData: true }),
    )
    const wrapper = await mountView()
    const memoryRow = wrapper.find('[data-row-id="memory"]')

    // 主机条（#430：页头下方通栏一条）接真实数据；CPU 型号已移出（归 CPU 块）
    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.find('[data-test="monitor-host-name"]').text()).toBe('trade-01')
    expect(host.find('[data-test="monitor-host-system"]').text()).toBe('Ubuntu 24.04.1 LTS')
    expect(host.find('[data-test="monitor-host-kernel"]').text()).toBe('6.8.0-45-generic')
    expect(host.find('[data-test="monitor-host-uptime"]').text()).toBe('1 小时 1 分')
    expect(host.find('[data-test="monitor-host-status"]').text()).toMatch(/^更新于 \d{2}:\d{2}:\d{2}$/)
    // CPU 总使用率接真实数据
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    // #363：内存 / 磁盘 / 网络的行数据全部来自台账（行内七列共用一条列基线）
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(memoryRow.find('[data-test="ledger-detail"]').text()).toBe('16.0 GiB / 32.0 GiB')
    expect(wrapper.find('[data-test="monitor-ledger"]').text()).toContain('/data')
    expect(wrapper.find('[data-test="monitor-ledger"]').text()).toContain('25.0%')
    expect(wrapper.find('[data-test="monitor-ledger"]').text()).toContain('eth0')
    // 每张网卡的收发各占一行（平铺，不点开就能看见）
    const nicRows = wrapper.findAll('[data-test="monitor-network-row"]')
    expect(nicRows.map(row => row.attributes('data-direction'))).toEqual(['rx', 'tx'])
    expect(nicRows[0].find('.monitor-view__ledger-value').text()).toBe('1.0 MiB/s')
    expect(nicRows[1].find('.monitor-view__ledger-value').text()).toBe('2.0 MiB/s')
    expect(wrapper.find('[data-test="monitor-ledger"]').text()).not.toContain('不可用')
    vi.restoreAllMocks()
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
    // #363：内存 / 交换 / 卷 / 网卡不可用 → 各自的台账行带「不可用：原因」，不渲染任何数值
    const memoryRow = wrapper.find('[data-row-id="memory"]')
    expect(memoryRow.attributes('data-state')).toBe('disabled')
    expect(memoryRow.find('[data-test="row-state"]').text()).toContain('已在配置中关闭')
    // 配置关闭是「没有这次读数」而不是「读到了 0」：值列写「—」，且不提供读数槽
    expect(memoryRow.find('.monitor-view__ledger-value').text()).toBe('—')
    expect(memoryRow.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    const swapRow = wrapper.find('[data-row-id="swap"]')
    expect(swapRow.find('[data-test="row-state"]').text()).toContain('已在配置中关闭')
    // not_implemented 不是「配置关闭」：卷 / 网卡按不可用呈现（`disabled_by_config` 才进 disabled 档）
    expect(wrapper.find('[data-row-id="volumes"]').attributes('data-state')).toBe('unavailable')
    expect(wrapper.find('[data-test="monitor-ledger"]').text()).toContain('该指标将在后续版本接入')
    // 默认夹具的 network 是 disabled_by_config（未启用的域）→ disabled 档
    expect(wrapper.find('[data-row-id="network"]').attributes('data-state')).toBe('disabled')
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

  it('全部可用：16 核条形与负载三项照常渲染，不出现不可用（上下文切换/中断已移出实时页）', async () => {
    // 夹具刻意**不**携带 cpu.load_average 的 semantic_hints：真实发送方只在
    // semantic=processor_queue（Windows 代理量）时携带该提示（apps/sysmon/snapshot_builder.cpp），
    // Linux 原生负载不携带。此前的 'linux load average from /proc/loadavg' 夹具与契约不符。
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    // CPU 卡片整行宽（总量 + 每核 + 负载 + 速率）
    expect(wrapper.find('[data-test="monitor-cpu"]').classes()).toContain('monitor-view__card--wide')

    // #435：每核条形不再显示逐核文字，读数在 title / aria（`核 N · X%`，频率可用时再带 MHz）
    const cores = wrapper.findAll('[data-test="monitor-core"]')
    expect(cores).toHaveLength(16)
    expect(cores[0].attributes('data-core-index')).toBe('0')
    expect(cores[15].attributes('data-core-index')).toBe('15')
    expect(cores[0].attributes('title')).toContain('核 0')
    expect(cores[0].attributes('title')).toContain('5.0%')
    expect(cores[0].attributes('aria-label')).toBe(cores[0].attributes('title'))

    expect(wrapper.find('[data-test="monitor-load-one"]').text()).toBe('0.42')
    expect(wrapper.find('[data-test="monitor-load-five"]').text()).toBe('0.51')
    expect(wrapper.find('[data-test="monitor-load-fifteen"]').text()).toBe('0.63')
    // 无提示时不得凭空造提示（#207 B2 的映射只在发送方真的携带 key 时生效）
    expect(wrapper.find('[data-test="monitor-load-hint"]').exists()).toBe(false)

    // #435：上下文切换 / 中断从实时页移除（历史视图两项都可查），台账也不再产出 CPU 组
    expect(wrapper.find('[data-row-id="cpu.rates"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu-rates"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('不可用')
  })

  it('每核填充按占用率等级着色（70 / 90，与顶栏 CPU 徽章同判据），不再有「饱和」标记', async () => {
    const cores = makeSysmonCpuCores(6)
    cores[1].usage_ratio = 0.75
    cores[4].usage_ratio = 0.95
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuCores: cores }))
    const wrapper = await mountView()

    // 8080 验收：「饱和」这类信息不要了——不再有文字标记 / 专属修饰类 / tooltip 里的「饱和」
    expect(wrapper.findAll('[data-test="monitor-core-saturated"]')).toHaveLength(0)
    expect(wrapper.find('.monitor-view__core-bar-fill--saturated').exists()).toBe(false)
    const fills = wrapper.findAll('[data-test="monitor-core"] .monitor-view__core-bar-fill')
    expect(fills[0].attributes('data-level')).toBe('normal')
    expect(fills[1].attributes('data-level')).toBe('warn')
    expect(fills[4].attributes('data-level')).toBe('danger')
    expect(wrapper.findAll('[data-test="monitor-core"]').map(core => core.attributes('title')).join(' '))
      .not.toContain('饱和')
  })

  it('部分不可用：单核 null 的 title 显示不可用、负载空态、台账无 CPU 速率行', async () => {
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
    // #435：读数在 title / aria（不再有逐核文字）；缺失值显示「不可用」而不是 0
    expect(coreItems[1].attributes('title')).toContain('不可用')
    expect(coreItems[0].attributes('title')).not.toContain('不可用')
    expect(coreItems[1].find('.monitor-view__core-bar-fill').attributes('style') ?? '')
      .not.toContain('--core-fill')

    const loadEmpty = wrapper.find('[data-test="monitor-load-unavailable"]')
    expect(loadEmpty.exists()).toBe(true)
    expect(loadEmpty.text()).toContain('unsupported')
    expect(wrapper.find('[data-test="monitor-load-hint"]').exists()).toBe(false)

    // #435：速率读数已移出实时页（历史视图可查），台账也不再产出 CPU 行
    expect(wrapper.find('[data-row-id="cpu.rates"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-context-switches"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-interrupts"]').exists()).toBe(false)
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
    // #435：速率已移出实时页 → 台账中不再有 cpu.rates 行（不是「—」空态行）
    expect(wrapper.find('[data-row-id="cpu.rates"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-context-switches"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-interrupts"]').exists()).toBe(false)
  })
})

describe('MonitorView 温度与每核频率（#73，可用性驱动）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('全部可用：温度卡片与每核频率面板渲染真实数据（45.5 °C / 4.35 GHz）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ sensorData: true }))
    const wrapper = await mountView()

    // #435 验收收敛：温度主值 + 归属留在格里，逐传感器明细进 title / aria（不再有只这一格才有的折叠按钮）
    const temperatureCell = wrapper.find('[data-test="monitor-temperature"]')
    expect(temperatureCell.exists()).toBe(true)
    expect(temperatureCell.find('[data-test="monitor-temperature-max"]').text()).toBe('45.5 °C')
    expect(temperatureCell.find('[data-test="monitor-temperature-summary"]').text()).toBe('CPU · Package id 0')
    const sensorTitle = temperatureCell.find('[data-test="monitor-temperature-max"]').attributes('title') ?? ''
    // 友好名 + 通道 + 温度，全部传感器都在（组内按 label 字典序）
    expect(sensorTitle).toContain('CPU · Package id 0 45.5 °C')
    expect(sensorTitle).toContain('CPU · Core 0 41.5 °C')
    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)
    // 读屏名挂在统计格的 group 上（裸 div 的 aria-label 会被读屏忽略），内容与 title 同源
    expect(temperatureCell.attributes('role')).toBe('group')
    expect(temperatureCell.attributes('aria-label')).toBe(`温度 45.5 °C；${sensorTitle}`)
    // 四数字行里不再有只有温度格才有的按钮（四格同一高度）
    expect(temperatureCell.find('button').exists()).toBe(false)

    // #435：主值是全部有读数核的平均（3600 + index×100 → 平均 4350 MHz = 4.35 GHz，数值与单位分开排版）
    expect(wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-frequency"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-frequency-value"]').text()).toBe('4.35 GHz')
    expect(wrapper.find('[data-test="monitor-frequency-context"]').exists()).toBe(false)
    const coreBars = wrapper.findAll('[data-test="monitor-core"]')
    expect(coreBars[0].attributes('title')).toContain('3.60 GHz')
    expect(coreBars[15].attributes('title')).toContain('5.10 GHz')
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

    // 排在 title 里的顺序 = 芯片排序 CPU → GPU → 无线网卡 → 主板 ACPI（与发送顺序无关）
    const title = wrapper.find('[data-test="monitor-temperature-max"]').attributes('title') ?? ''
    const orderedParts = ['CPU · Package id 0 45.5 °C', 'GPU · edge 52.0 °C', '无线网卡 · temp1 38.0 °C', '主板 ACPI · temp1 27.8 °C']
    let last = -1
    for (const part of orderedParts) {
      const at = title.indexOf(part)
      expect(at, `${part} 缺失或顺序不对`).toBeGreaterThan(last)
      last = at
    }
    // 最高温来自 GPU（52.0）：主值与归属都跟着最高温走
    expect(wrapper.find('[data-test="monitor-temperature-max"]').text()).toBe('52.0 °C')
    expect(wrapper.find('[data-test="monitor-temperature-summary"]').text()).toBe('GPU · edge')
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

    const title = wrapper.find('[data-test="monitor-temperature-max"]').attributes('title') ?? ''
    expect(title).toContain('CPU · Package id 0 45.5 °C')
    expect(title).toContain('主板 ACPI · temp1 27.8 °C')  // 无 label 时回退通道名
    expect(wrapper.find('[data-test="monitor-temperature-summary"]').text()).toBe('CPU · Package id 0')
  })

  it('不可用：温度与每核频率区块整块不渲染（不显示 0 / 不可用占位）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-temperature"]').exists()).toBe(false)
    expect(wrapper.findAll('[data-test="monitor-temperature-sensor"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-temperature-empty"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-frequency"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-frequency-empty"]').exists()).toBe(false)
    // 不可用不是 0：不出现温度 / 频率数字
    expect(wrapper.text()).not.toContain('°C')
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('MHz')
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('GHz')
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
    // #430：系统 / 内核拆成两个字段，build 号不再在「系统」里重复一次
    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.find('[data-test="monitor-host-system"]').text()).toBe('Windows 10 Pro')
    expect(host.find('[data-test="monitor-host-kernel"]').text()).toBe('10.0.19045')
    expect(host.text()).not.toContain('build 19045')
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
    expect(frequencyEmpty.text()).not.toContain('0 MHz')
    expect(frequencyEmpty.text()).not.toContain('0.00 GHz')

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

    const title = wrapper.find('[data-test="monitor-temperature-max"]').attributes('title') ?? ''
    // 只有合法读数进入明细（空 sensor / NaN 温度被 normalizer 过滤）
    expect(title.split('；')).toHaveLength(1)
    expect(title).toContain('45.5 °C')
    expect(title).not.toContain('30.0 °C')

    // 非法频率条目不参与平均；唯一有效读数（核 3 · 3.60 GHz）出现在该核条形的 title 里
    expect(wrapper.find('[data-test="monitor-frequency-value"]').text()).toBe('3.60 GHz')
    const coreBars = wrapper.findAll('[data-test="monitor-core"]')
    expect(coreBars[3].attributes('title')).toContain('3.60 GHz')
    expect(coreBars[0].attributes('title')).not.toContain('GHz')
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

  it('默认快照：内存使用率 / 已用/总量 / 可用 / 缓存 / 缓冲与交换行均渲染真实数据', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const memoryRow = wrapper.find('[data-row-id="memory"]')
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(memoryRow.find('[data-test="ledger-detail"]').text()).toBe('16.0 GiB / 32.0 GiB')

    // 展开明细给出五项（总量 / 已用 / 可用 / 缓存 / 缓冲）——「缓存 / 缓冲为 null 就不渲染该行」
    // 的语义现在落在展开项的**存在性**上（#363），不再是卡片里的列表行。
    expect(wrapper.findAll('[data-test="ledger-fields"]')).toHaveLength(0)
    await memoryRow.find('[data-test="ledger-expand-memory"]').trigger('click')
    await wrapper.vm.$nextTick()
    const labels = memoryRow.findAll('[data-test="ledger-field"]').map(field => field.attributes('data-label'))
    expect(labels).toEqual(['总量', '已用（总量 − 可用）', '可用', '缓存（可回收）', '缓冲（内核元数据）'])
    const values = memoryRow.findAll('[data-test="ledger-field-value"]').map(field => field.text())
    expect(values).toEqual(['32.0 GiB', '16.0 GiB', '16.0 GiB', '8.0 GiB', '256.0 MiB'])

    const swapRow = wrapper.find('[data-row-id="swap"]')
    expect(swapRow.exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-swap-usage"]').text()).toBe('25.0%')
    expect(swapRow.find('[data-test="ledger-detail"]').text()).toBe('2.0 GiB / 8.0 GiB')

    // #120 收口：内存 / 交换使用率色标与卷行、顶栏共享同一组常量（默认 50% / 25% → normal）
    expect(wrapper.find('[data-row-id="memory"]').attributes('data-level')).toBe('normal')
    expect(swapRow.attributes('data-level')).toBe('normal')
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

    const memoryRow = wrapper.find('[data-row-id="memory"]')
    expect(memoryRow.attributes('data-level')).toBe('danger')
    // 色标只影响颜色，不改变数值展示
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('90.0%')

    const swapRow = wrapper.find('[data-row-id="swap"]')
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

    // 缓冲不可得 → 组成带退化为「已用 + 可用」两段并标注（不补 0），展开项里缓冲写「不可用」
    const composition = wrapper.find('[data-test="monitor-memory-composition"]')
    expect(composition.find('[data-test="monitor-memory-composition-degraded"]').exists()).toBe(true)
    const segments = composition.findAll('.monitor-view__ledger-band-seg')
    expect(segments.map(segment => segment.attributes('data-segment'))).toEqual(['used', 'free'])
    const memoryRow = wrapper.find('[data-row-id="memory"]')
    await memoryRow.find('[data-test="ledger-expand-memory"]').trigger('click')
    await wrapper.vm.$nextTick()
    const fields = new Map(
      memoryRow.findAll('[data-test="ledger-field"]')
        .map(field => [field.attributes('data-label'), field.find('[data-test="ledger-field-value"]').text()]),
    )
    expect(fields.get('缓存（可回收）')).toBe('4.0 GiB')
    // 缓冲为 null：展开项显示「不可用」，不是 0 B
    expect(fields.get('缓冲（内核元数据）')).toBe('不可用')
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
    // 缓存与缓冲都不可得：组成带只有两段，两处的标注都在（不补 0、不静默丢段）
    const segments = wrapper.findAll('[data-test="monitor-memory-composition"] .monitor-view__ledger-band-seg')
    expect(segments.map(segment => segment.attributes('data-segment'))).toEqual(['used', 'free'])
    expect(wrapper.find('[data-test="monitor-memory-composition-degraded"]').text())
      .toContain('缓存与缓冲不可得')
  })

  it('无交换分区（swap 可用且 total_bytes=0）：行写「无交换」+ 事实说明，不显示 0%', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
    }))
    const wrapper = await mountView()

    const swapRow = wrapper.find('[data-row-id="swap"]')
    // 「无此维度」是合法事实：状态点了给「无交换」，明细给说明，值列写「—」（不是 0%）
    expect(swapRow.attributes('data-state')).toBe('none')
    expect(swapRow.find('.monitor-view__ledger-state-text').text()).toBe('无交换')
    expect(swapRow.find('[data-test="row-state"]').text()).toBe('无交换分区')
    expect(swapRow.find('.monitor-view__ledger-value').text()).toBe('—')
    expect(swapRow.text()).not.toContain('%')
    expect(swapRow.find('[data-test="monitor-memory-swap-usage"]').exists()).toBe(false)
  })

  it('交换不可用（无交换分区 → unsupported）：行写「无交换」+ 平台事实文案，不渲染 total=0 对象', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
      metrics: { swap: { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountView()

    // unsupported 在交换语境下仍是「本机没有这个维度」而不是「采集故障」：
    // 文案给出平台事实，而不是把裸 reason 丢给用户（契约 §2 的 unsupported 语义）
    const swapRow = wrapper.find('[data-row-id="swap"]')
    expect(swapRow.attributes('data-state')).toBe('none')
    expect(swapRow.find('.monitor-view__ledger-state-text').text()).toBe('无交换')
    expect(swapRow.find('[data-test="row-state"]').text()).toContain('本机无交换分区/页面文件')
    expect(swapRow.text()).not.toContain('0.0%')
  })

  it('交换可用但本帧对象缺失（能力粘性 / 未采集）：显示「暂无数据」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ swap: null }))
    const wrapper = await mountView()

    // 能力可用但本帧没有对象 = empty 档（不是 unavailable、也不是 none）
    const swapRow = wrapper.find('[data-row-id="swap"]')
    expect(swapRow.attributes('data-state')).toBe('empty')
    expect(swapRow.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(swapRow.find('.monitor-view__ledger-state-text').text()).toBe('暂无数据')
    expect(swapRow.find('[data-test="monitor-memory-swap-usage"]').exists()).toBe(false)
  })

  it('内存可用但本帧对象缺失：显示「暂无数据」空态（不是不可用），交换仍独立渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ memory: null }))
    const wrapper = await mountView()

    const memoryRow = wrapper.find('[data-row-id="memory"]')
    expect(memoryRow.attributes('data-state')).toBe('empty')
    expect(memoryRow.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(wrapper.find('[data-test="monitor-memory-value"]').exists()).toBe(false)
    // 交换可用性独立于内存：仍渲染真实数值
    expect(wrapper.find('[data-row-id="swap"]').attributes('data-state')).toBe('ok')
  })

  it('内存不可用但 payload 残留数据：按可用性显示不可用，不渲染陈旧数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountView()

    const memoryRow = wrapper.find('[data-row-id="memory"]')
    expect(memoryRow.attributes('data-state')).toBe('unavailable')
    expect(memoryRow.find('[data-test="row-state"]').text()).toContain('采集失败，暂不可用')
    // 不可用是「读到了但没读到」：值列写「不可用」而不是 0，陈旧数字一律不渲染
    expect(memoryRow.find('.monitor-view__ledger-value').text()).toBe('不可用')
    expect(memoryRow.find('[data-test="monitor-memory-value"]').text()).toBe('不可用')
    expect(memoryRow.text()).not.toContain('50.0%')
    expect(memoryRow.text()).not.toContain('16.0 GiB')
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
    expect(wrapper.find('[data-row-id="memory"] [data-test="ledger-detail"]').text()).toBe('16.0 GiB / 32.0 GiB')
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
    expect(rows[0].find('.monitor-view__ledger-value').text()).toBe('90.0%')
    expect(rows[1].find('.monitor-view__ledger-value').text()).toBe('69.0%')
    expect(rows[2].find('.monitor-view__ledger-value').text()).toBe('70.0%')
    expect(rows[0].find('[data-test="ledger-detail"]').text()).toBe('90.0 GiB / 100.0 GiB')
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

    const row = wrapper.find('[data-row-id="volumes"]')
    expect(row.attributes('data-state')).toBe('disabled')
    expect(row.find('[data-test="row-state"]').text()).toContain('不可用')
    // 空态沿用既有 unavailableDesc：disabled_by_config 映射为「已在配置中关闭」
    expect(row.find('[data-test="row-state"]').text()).toContain('已在配置中关闭')
    expect(wrapper.findAll('[data-test="monitor-disk-volume"]')).toHaveLength(0)
  })

  it('共享内存目录：渲染路径 / 占用 / 页文件数 / 最旧页文件年龄', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const shmRow = wrapper.find('[data-row-id="shm_dir"]')
    expect(wrapper.find('[data-test="monitor-shm-total"]').text()).toBe('720.0 MiB')
    expect(wrapper.find('[data-test="monitor-shm-total"]').element.tagName).toBe('BUTTON')
    expect(shmRow.find('[data-test="ledger-detail"]').text()).toBe('12 页文件 · 最旧 8 分钟')

    // 路径 / 页文件数 / 最旧页文件年龄在展开明细里（#363：一行明细 + 可展开完整明细）
    await shmRow.find('[data-test="ledger-expand-shm_dir"]').trigger('click')
    await wrapper.vm.$nextTick()
    const fields = new Map(
      shmRow.findAll('[data-test="ledger-field"]')
        .map(field => [field.attributes('data-label'), field.find('[data-test="ledger-field-value"]').text()]),
    )
    expect(fields.get('路径')).toBe('/srv/dztrader/shm')
    expect(fields.get('页文件数')).toBe('12')
    expect(fields.get('最旧页文件年龄')).toBe('8 分钟')
  })

  it('共享内存目录无页文件（oldest_file_age_sec=null）：不渲染该行，不显示 0 秒', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      shmDir: { path: '/srv/shm', total_bytes: 0, page_files: 0, oldest_file_age_sec: null },
    }))
    const wrapper = await mountView()

    const shmRow = wrapper.find('[data-row-id="shm_dir"]')
    await shmRow.find('[data-test="ledger-expand-shm_dir"]').trigger('click')
    await wrapper.vm.$nextTick()
    const labels = shmRow.findAll('[data-test="ledger-field"]').map(field => field.attributes('data-label'))
    expect(labels).toContain('页文件数')
    // 无页文件时 oldest_file_age_sec 为 null：不渲染该行（不是 0 秒）
    expect(labels).not.toContain('最旧页文件年龄')
    expect(shmRow.text()).not.toContain('0 秒')
  })

  it('共享内存目录不可用：显示不可用 + 原因；可用但本帧缺失显示「暂无数据」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { shm_dir: { available: false, reason: 'no_permission' } },
    }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-row-id="shm_dir"]')
    expect(row.attributes('data-state')).toBe('unavailable')
    expect(row.find('[data-test="row-state"]').text()).toContain('不可用')
    // 同上：no_permission 由共享映射给出可读文案
    expect(row.find('[data-test="row-state"]').text()).toContain('权限不足，无法读取该指标')
    // 不可用时值列写「不可用」（读到了但没读到），且没有跳历史入口
    expect(row.find('.monitor-view__ledger-value').text()).toBe('不可用')
    expect(wrapper.find('[data-test="monitor-shm-total"]').element.tagName).not.toBe('BUTTON')

    // 能力粘性可用但本帧对象缺失 → 「暂无数据」而不是「不可用」
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ shmDir: null }))
    await wrapper.vm.$nextTick()
    const empty = wrapper.find('[data-row-id="shm_dir"]')
    expect(empty.attributes('data-state')).toBe('empty')
    expect(empty.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(empty.find('[data-test="row-state"]').text()).not.toContain('不可用')
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
    const shmRow = wrapper.find('[data-row-id="shm_dir"]')
    await shmRow.find('[data-test="ledger-expand-shm_dir"]').trigger('click')
    await wrapper.vm.$nextTick()
    const fields = new Map(
      shmRow.findAll('[data-test="ledger-field"]')
        .map(field => [field.attributes('data-label'), field.find('[data-test="ledger-field-value"]').text()]),
    )
    expect(fields.get('路径')).toBe('C:\\dztrader\\shm')
    expect(fields.get('最旧页文件年龄')).toBe('30 秒')
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

  it('默认快照（能力可用）：两块盘按 device 升序平铺，行内含忙碌占比与写吞吐 / 延迟明细', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const rows = wrapper.findAll('[data-test="monitor-disk-io-row"]')
    expect(rows).toHaveLength(2)
    // payload 顺序为 sda、nvme0n1；store 按 device 升序重排
    expect(rows[0].attributes('data-device')).toBe('nvme0n1')
    expect(rows[1].attributes('data-device')).toBe('sda')

    // 不点开就能看见：忙碌占比（不是「利用率」）+ 一行写吞吐 / 延迟明细
    expect(rows[0].find('.monitor-view__ledger-label').text()).toBe('nvme0n1 忙碌占比')
    expect(rows[0].find('.monitor-view__ledger-value').text()).toBe('72.0%')
    expect(rows[0].find('[data-test="ledger-detail"]').text()).toBe('写 32.0 MiB/s · 延迟 0.45 ms')
    expect(rows[1].find('.monitor-view__ledger-label').text()).toBe('sda 忙碌占比')
    expect(rows[1].find('.monitor-view__ledger-value').text()).toBe('35.0%')
    expect(rows[1].find('[data-test="ledger-detail"]').text()).toBe('写 2.0 MiB/s · 延迟 1.25 ms')

    // 展开后是完整七项（读/写吞吐、读/写 IOPS、延迟、队列深度、忙碌占比）
    await rows[0].find('[data-test="ledger-expand-disk:nvme0n1"]').trigger('click')
    await wrapper.vm.$nextTick()
    const labels = rows[0].findAll('[data-test="ledger-field"]').map(field => field.attributes('data-label'))
    // 「平均延迟」是 #361 的字段名（延迟是每个已完成请求的平均驻留时间，明细里不再简称）
    expect(labels).toEqual(['读吞吐', '写吞吐', '读 IOPS', '写 IOPS', '平均延迟', '队列深度', '忙碌占比'])
    const values = rows[0].findAll('[data-test="ledger-field-value"]').map(field => field.text())
    expect(values).toEqual(['64.0 MiB/s', '32.0 MiB/s', '2048 IOPS', '1024 IOPS', '0.45 ms', '1.25', '72.0%'])

    // 能力可用时不会有 `disk_io` 空态行，盘行自身也不含「不可用」
    expect(wrapper.find('[data-row-id="disk_io"]').exists()).toBe(false)
    for (const row of rows) expect(row.text()).not.toContain('不可用')
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
    await row.find('[data-test="ledger-expand-disk:sda"]').trigger('click')
    await wrapper.vm.$nextTick()
    const values = row.findAll('[data-test="ledger-field-value"]').map(field => field.text())
    expect(values).toHaveLength(7)
    expect(values.every(value => value === '不可用')).toBe(true)
    // 不得用 0 冒充：不出现 0 B/s、0 IOPS、0.00 ms、0.0%
    expect(row.text()).not.toContain('0 B/s')
    expect(row.text()).not.toContain('0 IOPS')
    expect(row.text()).not.toContain('0.00 ms')
    expect(row.text()).not.toContain('0.0%')
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(1)
  })

  it('能力不可用（no_permission）：整块「不可用」+ 可操作文案，不渲染盘行', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'no_permission' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-row-id="disk_io"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.attributes('data-state')).toBe('unavailable')
    expect(unavailable.find('[data-test="row-state"]').text()).toContain('不可用')
    expect(unavailable.find('[data-test="row-state"]').text())
      .toContain('磁盘性能计数器不可用（需管理员权限或性能计数器未开启）')
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(0)
  })

  it('可用但本帧空数组 / null：显示「暂无数据」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ diskIo: [] }))
    const wrapper = await mountView()

    const empty = wrapper.find('[data-row-id="disk_io"]')
    expect(empty.exists()).toBe(true)
    expect(empty.attributes('data-state')).toBe('empty')
    expect(empty.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(wrapper.findAll('[data-test="monitor-disk-io-row"]')).toHaveLength(0)

    // 能力粘性可用、本帧值为 null（WS 推送）→ 同样走「暂无数据」，不改成「不可用」
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ diskIo: null }))
    await wrapper.vm.$nextTick()
    const stillEmpty = wrapper.find('[data-row-id="disk_io"]')
    expect(stillEmpty.attributes('data-state')).toBe('empty')
    expect(stillEmpty.find('[data-test="row-state"]').text()).not.toContain('不可用')
  })

  it('不可用原因文案：disabled_by_config / collect_failed / 未知 reason 分别映射', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'disabled_by_config' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-row-id="disk_io"] [data-test="row-state"]').text()).toContain('已在配置中关闭')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'collect_failed' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-row-id="disk_io"] [data-test="row-state"]').text()).toContain('采集失败，暂不可用')

    // 未知 reason 走既有 unavailableDesc：裸 reason 展示，不吞掉诊断信息
    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'some_new_reason' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-row-id="disk_io"] [data-test="row-state"]').text()).toContain('some_new_reason')
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

  it('默认快照（单块物理网卡）：收发各占一行，明细给错误/丢弃速率与累计量，且不显示虚拟标记', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    // #363：每张网卡的收发各占一行（平铺，不点开就能看见）
    const rows = wrapper.findAll('[data-test="monitor-network-row"]')
    expect(rows).toHaveLength(2)
    expect(rows.map(row => row.attributes('data-interface'))).toEqual(['eth0', 'eth0'])
    expect(rows.map(row => row.attributes('data-direction'))).toEqual(['rx', 'tx'])
    expect(rows[0].find('.monitor-view__ledger-value').text()).toBe('1.0 MiB/s')
    expect(rows[1].find('.monitor-view__ledger-value').text()).toBe('2.0 MiB/s')

    // 速率 0 是合法真实值（本窗口确实没有增量），显示 0.0/s 而不是「不可用」；
    // 错误/丢弃并列累计量（累计计数器没有"不可用"语义）
    expect(rows[0].find('[data-test="monitor-network-rx-errors"]').text()).toBe('错 0.0/s（累计 3）')
    expect(rows[0].find('[data-test="monitor-network-rx-drops"]').text()).toBe('丢 0.0/s（累计 7）')
    expect(rows[1].find('[data-test="monitor-network-tx-errors"]').text()).toBe('错 0.0/s（累计 1）')
    expect(rows[1].find('[data-test="monitor-network-tx-drops"]').text()).toBe('丢 0.0/s（累计 2）')

    // 累计字节与包数在展开明细里（十项口径完整可查）
    await rows[0].find('[data-test="ledger-expand-nic:eth0:rx"]').trigger('click')
    await wrapper.vm.$nextTick()
    const fields = new Map(
      rows[0].findAll('[data-test="ledger-field"]')
        .map(field => [field.attributes('data-label'), field.find('[data-test="ledger-field-value"]').text()]),
    )
    expect(fields.get('接收字节（累计）')).toBe('8.0 GiB')
    expect(fields.get('接收包数（累计）')).toBe('12,000,000')
    expect(fields.get('接收错误速率')).toBe('0.0/s')
    expect(fields.get('虚拟网卡')).toBe('否')

    // 物理网卡不带「虚拟」标记
    expect(wrapper.find('[data-test="monitor-network-virtual"]').exists()).toBe(false)
    expect(wrapper.find('[data-row-id="network"]').exists()).toBe(false)
  })

  it('全部可用时按 interface 升序排序，不依赖发送端顺序', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ virtualNetwork: true }))
    const wrapper = await mountView()

    // 夹具发送顺序为 veth1234、docker0、lo、eth0；台账按 interface 升序、每卡收发两行相邻
    expect(wrapper.findAll('[data-test="monitor-network-row"]').map(row => row.attributes('data-interface')))
      .toEqual(['docker0', 'docker0', 'eth0', 'eth0', 'lo', 'lo', 'veth1234', 'veth1234'])
  })

  it('虚拟网卡显式标注「虚拟」；前端不自行过滤（过滤是发送方义务，契约 §2.4）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ virtualNetwork: true }))
    const wrapper = await mountView()

    const tags = wrapper.findAll('[data-test="monitor-network-virtual"]')
    // 每张虚拟网卡的两个方向行各带一次标记（行是独立实体，标记跟着行走）
    expect(tags).toHaveLength(6)
    expect(tags.every(tag => tag.text() === '虚拟')).toBe(true)
    const rowsWithTag = wrapper.findAll('[data-test="monitor-network-row"]')
      .filter(row => row.find('[data-test="monitor-network-virtual"]').exists())
      .map(row => row.attributes('data-interface'))
    expect(rowsWithTag).toEqual(['docker0', 'docker0', 'lo', 'lo', 'veth1234', 'veth1234'])
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

    const rows = wrapper.findAll('[data-test="monitor-network-row"]')
    // 吞吐速率没有累计量可并列：null 就是「不可用」
    expect(rows[0].find('.monitor-view__ledger-value').text()).toBe('不可用')
    expect(rows[1].find('.monitor-view__ledger-value').text()).toBe('不可用')
    // 错误/丢弃速率 null 显示「不可用」，但不隐瞒累计量（累计计数器没有"不可用"语义）
    for (const field of ['rx-errors', 'rx-drops']) {
      const text = rows[0].find(`[data-test="monitor-network-${field}"]`).text()
      expect(text).toContain('不可用')
      expect(text).toContain('累计')
    }
    for (const field of ['tx-errors', 'tx-drops']) {
      const text = rows[1].find(`[data-test="monitor-network-${field}"]`).text()
      expect(text).toContain('不可用')
      expect(text).toContain('累计')
    }
    // 速率不可用时累计量仍照常显示（累计计数器没有"不可用"语义）
    await rows[0].find('[data-test="ledger-expand-nic:eth0:rx"]').trigger('click')
    await wrapper.vm.$nextTick()
    const bytes = rows[0].findAll('[data-test="ledger-field"]')
      .find(field => field.attributes('data-label') === '接收字节（累计）')
    expect(bytes?.find('[data-test="ledger-field-value"]').text()).toBe('8.0 GiB')
    expect(wrapper.text()).not.toContain('0 B/s')
    expect(wrapper.text()).not.toContain('0.0/s')
  })

  it('全被 hide_virtual 滤掉（空数组 + available=true）：显示「已隐藏全部虚拟网卡」而不是「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: [] }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-row-id="network"]')
    expect(row.exists()).toBe(true)
    expect(row.find('[data-test="row-state"]').text()).toContain('已隐藏全部虚拟网卡')
    // 契约 §2.5 明文：全部被过滤 ≠ 不可用，也不是「暂无数据」的空态
    expect(row.attributes('data-state')).toBe('empty')
    expect(row.find('[data-test="row-state"]').text()).not.toContain('不可用')
    expect(row.find('[data-test="row-state"]').text()).not.toContain('暂无数据')
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
  })

  it('hide_virtual=false 时空数组 = 本机确实无网卡：显示「暂无数据」而不是「已隐藏」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: [] }))
    const wrapper = await mountView()
    // 生效配置显示全部网卡（hide_virtual=false）：空数组不能再解释成"被过滤"
    useSysmonStore().applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    await wrapper.vm.$nextTick()

    const row = wrapper.find('[data-row-id="network"]')
    expect(row.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(row.find('[data-test="row-state"]').text()).not.toContain('已隐藏全部虚拟网卡')
  })

  it('能力可用但本帧未采集（network=null）：同样走「暂无数据」，不改成「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ network: null }))
    const wrapper = await mountView()

    const row = wrapper.find('[data-row-id="network"]')
    expect(row.attributes('data-state')).toBe('empty')
    expect(row.find('[data-test="row-state"]').text()).toContain('暂无数据')
    expect(row.find('[data-test="row-state"]').text()).not.toContain('不可用')
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
  })

  it('能力不可用（disabled_by_config / collect_failed）：整块「不可用」+ 原因，不渲染网卡行与开关', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { network: { available: false, reason: 'disabled_by_config' } },
    }))
    const wrapper = await mountView()

    const unavailable = wrapper.find('[data-row-id="network"]')
    expect(unavailable.exists()).toBe(true)
    expect(unavailable.attributes('data-state')).toBe('disabled')
    expect(unavailable.find('[data-test="row-state"]').text()).toContain('不可用')
    expect(unavailable.find('[data-test="row-state"]').text()).toContain('已在配置中关闭')
    expect(wrapper.findAll('[data-test="monitor-network-row"]')).toHaveLength(0)
    // 能力不可用时开关无意义，不渲染
    expect(wrapper.find('[data-test="monitor-network-show-all"]').exists()).toBe(false)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { network: { available: false, reason: 'collect_failed' } },
    }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('[data-row-id="network"] [data-test="row-state"]').text()).toContain('采集失败，暂不可用')
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
      // #363：数值本身就是那个按钮（值槽里直接是 <button>），选择器不再多一层
      ['[data-test="monitor-shm-total"]', '共享内存目录占用'],
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

  it('实时视图的页头只保留标题 + 视图切换：数据龄并入主机条右端状态槽', async () => {
    // 钉住「此刻」：状态槽的「更新于」与跨日判断都读它，不让用例随真实时间漂移。
    vi.spyOn(Date, 'now').mockReturnValue(Date.UTC(2026, 9, 21, 15, 0, 2))
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    // #430：原「● 实时快照」状态行与副标题都已并入主机条，页头不再有第三行
    expect(wrapper.find('.monitor-view__snapshot-state').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-subtitle"]').exists()).toBe(false)
    expect(wrapper.find('.monitor-view__heading .monitor-view__snapshot-state').exists()).toBe(false)

    const status = wrapper.find('[data-test="monitor-host-status"]')
    expect(status.exists()).toBe(true)
    // 夹具 timestamp 与本机此刻同为当日 → 只给时分秒
    expect(status.text()).toMatch(/^更新于 \d{2}:\d{2}:\d{2}$/)
    // 状态槽与脉冲点都是存活信号：DOM 上不再有独立状态行 / 独立状态点
    expect(wrapper.find('.monitor-view__snapshot-dot').exists()).toBe(false)
    vi.restoreAllMocks()
  })

  it('历史 / 配置视图保留副标题（#430：只有实时视图的主机信息搬进主机条）', async () => {
    // 副标题条件从 `snapshot || viewMode !== 'realtime'` 收窄为 `viewMode !== 'realtime'`：
    // 实时视图不再有副标题，而历史 / 配置视图（含无快照分支）必须原样保留。
    vi.spyOn(Date, 'now').mockReturnValue(Date.UTC(2026, 9, 21, 15, 0, 2))
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-tab-history"]').trigger('click')
    await flushPromises()
    const subtitle = wrapper.find('[data-test="monitor-subtitle"]')
    expect(subtitle.exists()).toBe(true)
    expect(subtitle.text()).toContain('Ubuntu 24.04.1 LTS')
    expect(subtitle.text()).toContain('更新于')
    // 主机条只属于实时视图；没有主机条时顶栏不收紧间距
    expect(wrapper.find('[data-test="monitor-host"]').exists()).toBe(false)
    expect(wrapper.find('.monitor-view__topbar').classes()).not.toContain('monitor-view__topbar--host-follows')

    await wrapper.find('[data-test="monitor-tab-config"]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-subtitle"]').exists()).toBe(true)
    vi.restoreAllMocks()
  })

  it('等待快照态：主机条整条不渲染，等待语义由整页空态承担', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-host"]').exists()).toBe(false)
    expect(wrapper.find('.monitor-view__snapshot-state').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-subtitle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-waiting"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-waiting"]').text()).toContain('等待系统监控数据')
  })
})

// #371：存活性可失效——距上次收到新快照超过 3 × push_interval_sec（默认 5 秒 → 15 秒）时，
// 「实时快照」必须变成一句可被推翻的话。时间用 fake timers 推进（不睡墙钟），
// 阈值来源是生效配置的 realtime.push_interval_sec，**判据是本机接收时刻**（#331 triage）。
describe('MonitorView 快照停推降级（#371）', () => {
  const FIXTURE_TS = Date.UTC(2026, 9, 21, 15, 0, 0)

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  /**
   * 挂载页面，然后让本机时钟静置 `ageMs`（期间一帧快照都没收到）。
   *
   * 不伪造 store 内部状态、也不动 `snapshot.timestamp`：只是把假时钟往前推，让组件自己的 1 秒
   * ticker 与「此刻」自然走到那个年龄——这正是生产里停推时的样子。
   */
  async function mountAtAge(
    ageMs: number,
    configPushIntervalSec?: number,
    snapshot = makeSysmonSnapshot(),
  ) {
    vi.useFakeTimers({ now: FIXTURE_TS })
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    if (configPushIntervalSec !== undefined) {
      vi.mocked(sysmonApi.getConfig).mockResolvedValue(
        makeSysmonConfig({ realtime: { push_interval_sec: configPushIntervalSec } }),
      )
    }
    const wrapper = await mountView()
    // 静置 ageMs：ticker 每秒把「此刻」往前推，而没有任何新帧到达
    await vi.advanceTimersByTimeAsync(ageMs)
    await flushPromises()
    return wrapper
  }

  it('停推后状态槽转「快照已停 N 秒」：页面不再出现「实时」这种不可推翻的话', async () => {
    // 数据龄 16 秒 > 15 秒阈值
    const wrapper = await mountAtAge(16_000)

    const host = wrapper.find('[data-test="monitor-host"]')
    const status = host.find('[data-test="monitor-host-status"]')
    expect(status.text()).toBe('快照已停 16 秒')
    // 状态槽即唯一出口：不再有独立的「● 实时快照」行，也没有并排的「实时」二字
    expect(wrapper.find('.monitor-view__snapshot-state').exists()).toBe(false)
    expect(host.text()).not.toContain('实时快照')
    // 脉冲点停止脉动（`--stale` 修饰类挂上主机条，CSS 关掉 animation）
    expect(host.classes()).toContain('monitor-view__host--stale')
  })

  it('秒数随时间自己走：不靠推送驱动，页面静置 10 秒后读数跟着涨', async () => {
    // 挂载时数据龄 16 秒 → 推进 10 秒（期间一帧推送都没有）→ 26 秒
    const wrapper = await mountAtAge(16_000)
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 16 秒')

    await vi.advanceTimersByTimeAsync(10_000)
    await flushPromises()

    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 26 秒')
  })

  it('恢复推送即自动消失（无粘性）：下一帧快照把「已停」换回「更新于 …」', async () => {
    const wrapper = await mountAtAge(16_000)
    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.classes()).toContain('monitor-view__host--stale')
    expect(host.find('[data-test="monitor-host-status"]').text()).toContain('快照已停')

    // 恢复推送：有新帧到达 → store 把「上次收到」刷新为此刻 → 数据龄归零
    useSysmonStore().applySnapshot(makeSysmonSnapshot())
    await flushPromises()

    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toMatch(/^更新于 \d{2}:\d{2}:\d{2}$/)
    expect(wrapper.find('[data-test="monitor-host"]').classes()).not.toContain('monitor-view__host--stale')
  })

  it('阈值随生效配置走：push_interval_sec = 30 时，数据龄 16 秒仍是「更新于 …」', async () => {
    // 30 × 3 = 90 秒阈值；16 秒远未到
    const wrapper = await mountAtAge(16_000, 30)
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toMatch(/^更新于 /)

    // 同一个间隔下越过 90 秒才降级（91 秒按共享的「多久以前」口径落到「1 分钟」档）
    await vi.advanceTimersByTimeAsync(75_000)   // 累计 91 秒
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 1 分钟')
  })

  it('停推不改变台账的指标等级：normal 行仍是 normal（数据旧 ≠ 指标异常）', async () => {
    const wrapper = await mountAtAge(16_000)

    // 停推只降级存活信号，不改数据本身的等级判定——两件事不得互相污染
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停')
    const normalRows = wrapper.findAll('.monitor-view__ledger-row[data-level="normal"]')
    expect(normalRows.length).toBeGreaterThan(0)
  })

  it('发送方钟慢 60 秒也不判停推：判据是本机接收时刻，不是 snapshot.timestamp', async () => {
    // 这一条是口径的证伪用例。挂载时 REST 补拉让「上次收到」= 此刻（新鲜），
    // 而快照自带的 timestamp 比本机早整整 60 秒（发送方这口钟是慢的）。
    // 旧口径（now − timestamp）会算出 60 秒 > 15 秒阈值 → 永久假红；
    // 新口径（now − 接收时刻）应算出 0 秒 → 「更新于 …」。
    vi.useFakeTimers({ now: FIXTURE_TS })
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ timestamp: FIXTURE_TS - 60_000 }),
    )
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toMatch(/^更新于 /)
    expect(wrapper.find('[data-test="monitor-host"]').classes()).not.toContain('monitor-view__host--stale')
  })

  it('发送方钟快 60 秒且确实停推时仍判停推：不会被负差值掩盖', async () => {
    // 反向证伪：真停推 60 秒，而最后一帧的 timestamp 比本机此刻还晚 60 秒（发送方钟快）。
    // 旧口径算出 −60 秒 → 永不判停推（静默失效）；新口径只看本机接收时刻 → 照常判停推。
    // 本机此刻走到 FIXTURE_TS + 60s，而最后一帧的 timestamp 是 FIXTURE_TS + 120s（比此刻还晚 60s）
    const senderClockAhead = makeSysmonSnapshot({ timestamp: FIXTURE_TS + 120_000 })
    const wrapper = await mountAtAge(60_000, undefined, senderClockAhead)
    // 前提成立：用本机此刻作差得 −60 秒 —— 旧口径由此永不判停推（静默失效）
    expect(useSysmonStore().snapshot?.timestamp).toBe(FIXTURE_TS + 120_000)
    expect((FIXTURE_TS + 60_000) - (FIXTURE_TS + 120_000)).toBe(-60_000)

    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 1 分钟')
    expect(wrapper.find('[data-test="monitor-host"]').classes()).toContain('monitor-view__host--stale')
  })

  it('视图重建后停推计时不重置：接收时刻记在 store 里，不在视图里', async () => {
    // 这条用例的牙在**重建**上：三个 Tab 之间切换不销毁组件（viewMode 是组件内 ref），
    // 视图局部 ref 在那条路径上不会归零，因此切 Tab 验不出问题。
    // 真正的路径是路由离开再回来（组件卸载重建）：若「上次收到」记在视图里，重建时会被
    // 重新写成此刻 → 一台已经在停推的机器显示成「刚收到」。store 里的记录跨重建存活。
    const wrapper = await mountAtAge(60_000)
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 1 分钟')

    // 切 Tab 往返（组件不重建，仍应保持停推）
    await wrapper.find('[data-test="monitor-tab-history"]').trigger('click')
    await flushPromises()
    await wrapper.find('[data-test="monitor-tab-realtime"]').trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 1 分钟')

    // 卸载后重建（= 路由离开 /monitor 再回来）：store 保留，计时必须接着走
    wrapper.unmount()
    const rebuilt = await mountView()

    const rebuiltHost = rebuilt.find('[data-test="monitor-host"]')
    expect(rebuiltHost.find('[data-test="monitor-host-status"]').text()).toContain('快照已停 1 分钟')
    expect(rebuiltHost.classes()).toContain('monitor-view__host--stale')
    // 反证：若接收时刻被重建逻辑重置，这里会是「更新于 …」
    expect(rebuiltHost.find('[data-test="monitor-host-status"]').text()).not.toContain('更新于')
  })

  it('组件卸载后不留定时器（数据龄的读者已经走了）', async () => {
    const wrapper = await mountAtAge(16_000)
    // 挂载后有且仅有数据龄一个定时器（本页不轮询，WS 推送不是定时器）
    expect(vi.getTimerCount()).toBe(1)
    wrapper.unmount()
    expect(vi.getTimerCount()).toBe(0)
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

describe('MonitorView 主机条与 CPU 块（#430 / #435）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('主机条字段与快照一一对应：主机名 / 系统 / 内核 / 已运行 / 右端更新于', async () => {
    // 钉住「此刻」：状态槽的「更新于」与跨日判断都读它，不让用例随真实时间漂移。
    vi.spyOn(Date, 'now').mockReturnValue(Date.UTC(2026, 9, 21, 15, 0, 2))
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.exists()).toBe(true)
    expect(host.find('[data-test="monitor-host-name"]').text()).toBe('trade-01')
    // 系统 = platform_description 剥掉 (kernel …)（派生只在展示层，契约原值不动）
    expect(host.find('[data-test="monitor-host-system"]').text()).toBe('Ubuntu 24.04.1 LTS')
    expect(host.find('[data-test="monitor-host-kernel"]').text()).toBe('6.8.0-45-generic')
    // 运行时长按参考图口径（uptime_sec = 3661 → 1 小时 1 分）
    expect(host.find('[data-test="monitor-host-uptime"]').text()).toBe('1 小时 1 分')
    expect(host.find('[data-test="monitor-host-status"]').text()).toMatch(/^更新于 \d{2}:\d{2}:\d{2}$/)
    // 标签与值分开渲染（主机面板同一做法：名字次级色、值主色）
    expect(host.findAll('.monitor-view__host-label').map(label => label.text()))
      .toEqual(['主机', '系统', '内核', '已运行'])
    // #429 决策 6：CPU 型号 · 逻辑核数已移出主机条（归 CPU 块），不再在主机条里出现
    expect(host.text()).not.toContain('AMD Ryzen 9 7950X')
    expect(host.text()).not.toContain('逻辑核')
    vi.restoreAllMocks()
    // 系统信息卡已从实时视图消失（其内容并入主机条）
    expect(wrapper.find('[data-test="monitor-system"]').exists()).toBe(false)
    // 温度卡同样消失：温度只在 CPU 块内占一格（本夹具无传感器，故该格整块不渲染）
    expect(wrapper.findAll('.ds-card [data-test="monitor-temperature"]')).toHaveLength(0)
    expect(wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-temperature"]').exists()).toBe(false)
  })

  it('主机条无可显示值的字段显示 — 而不是空白（平台描述 / 内核为空串）', async () => {
    const snapshot = makeSysmonSnapshot({ resourceData: true })
    snapshot.availability.platform_description = ''
    snapshot.platform.kernel = ''
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    // 系统与内核各自退化为 —（不是空串撑出的空白字段）
    expect(wrapper.find('[data-test="monitor-host-system"]').text()).toBe('—')
    expect(wrapper.find('[data-test="monitor-host-kernel"]').text()).toBe('—')
    // 有值的字段照常（不因某一项缺失而整行占位）
    expect(wrapper.find('[data-test="monitor-host"]').text()).toContain('trade-01')
  })

  it('悬停补充：更新于给采样/漏拍、已运行给启动时刻（#432）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const status = wrapper.find('[data-test="monitor-host-status"]')
    expect(status.attributes('title')).toBe('采样间隔 1000 ms · 累计漏拍 0')
    const uptime = wrapper.find('[data-test="monitor-host-uptime"]')
    const startTitle = uptime.attributes('title') ?? ''
    // 夹具 boot_time_unix_sec = 2026-10-20T12:00:00Z：断言年份而不是具体时分，
    // 避免测试机时区不同导致的日期漂移；同时挡「把秒当毫秒传」的 1970 回归
    expect(startTitle).toMatch(/^启动于 2026-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/)
    expect(startTitle).not.toContain('1970')
  })

  it('悬停补充遇缺失/非法字段不拼假值（#432）', async () => {
    const snapshot = makeSysmonSnapshot()
    snapshot.sampling.measure_interval_ms = 0   // 非法：不拼「采样间隔 0 ms」
    snapshot.sampling.missed_ticks = Number.NaN  // 非有限：不拼漏拍
    snapshot.system.boot_time_unix_sec = 0      // 无启动时刻：不给 title
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-host-status"]').attributes('title')).toBeUndefined()
    expect(wrapper.find('[data-test="monitor-host-uptime"]').attributes('title')).toBeUndefined()
  })

  it('主机条是顶栏之后的独立一条，不再挤在标题行里（#430）', async () => {
    const wrapper = await mountView()
    const host = wrapper.find('[data-test="monitor-host"]')
    expect(host.exists()).toBe(true)
    // 不在 topbar / header 内 → 与「标题 + 视图切换」不同排
    expect(wrapper.find('.monitor-view__topbar [data-test="monitor-host"]').exists()).toBe(false)
    expect(wrapper.find('.monitor-view__header [data-test="monitor-host"]').exists()).toBe(false)
    // 是滚动容器的直接子级（与顶栏平级）
    expect(wrapper.find('.monitor-view > [data-test="monitor-host"]').exists()).toBe(true)
    // 有主机条时顶栏收紧下间距（历史 / 配置视图没有主机条，保持 24，见上一条用例）
    expect(wrapper.find('.monitor-view__topbar').classes()).toContain('monitor-view__topbar--host-follows')
  })

  it('四数字的小字上下文：逻辑核 / 5 分钟与 15 分钟 / 温度归属（无效重复与自造术语已删）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ sensorData: true }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    // 「无核饱和」是零信息，不写；饱和真的发生时才补一句
    expect(wrapper.find('[data-test="monitor-cpu-context"]').text()).toBe('16 逻辑核')

    // 参照主机面板的「5 分钟 x · 15 分钟 y」；核数不在这里重复
    expect(wrapper.find('[data-test="monitor-load-context"]').text()).toBe('5 分钟 0.51 · 15 分钟 0.63')

    // 频率不再有「型号标注」这类自造小字（基准频率已在标题行的型号字符串里）
    expect(wrapper.find('[data-test="monitor-frequency-context"]').exists()).toBe(false)
    // #435：主值 = 全核平均（3600 + index×100 的 16 个读数 → 4350 MHz = 4.35 GHz，单位小字），不再取最高核
    const frequencyValue = wrapper.find('[data-test="monitor-frequency-value"]')
    expect(frequencyValue.text()).toBe('4.35 GHz')
    // 数值与单位分两段：单位是单独的 12px/secondary 元素（不是和数字同字号的一整串）
    expect(frequencyValue.find('.monitor-view__stat-unit').text()).toBe('GHz')
    expect(frequencyValue.text().replace(' GHz', '')).toBe('4.35')

    // 温度：主值 + 归属（芯片 · 通道），不再堆传感器/芯片计数
    const temperature = wrapper.find('[data-test="monitor-cpu"] [data-test="monitor-temperature"]')
    expect(temperature.exists()).toBe(true)
    expect(temperature.find('[data-test="monitor-temperature-max"]').text()).toBe('45.5 °C')
    expect(temperature.find('[data-test="monitor-temperature-summary"]').text()).toBe('CPU · Package id 0')
  })


  it('每核条形数量跟随快照（16 → 8 → 4），常驻 CPU 块', async () => {
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

  it('柱高 = 该核占用率（不是固定满格），且没有「饱和」文字标记（#435 8080 验收）', async () => {
    const wrapper = await mountView()

    const cores = wrapper.findAll('[data-test="monitor-core"]')
    expect(wrapper.find('[data-test="monitor-core-saturated"]').exists()).toBe(false)

    // 柱高 = 该核占用率（不是固定满格）：断言**具体数值**，而不是「变量名出现过」——
    // 后者对任何有限 ratio 都成立，等于什么都没验证。默认夹具 usage_ratio = 0.05 + index*0.031：
    // 核 0 → 5.0%、核 3 → 14.3%（两位均由共享 formatPercent 产出，与条形 title 同源）。
    expect(coreFillPercent(cores[0])).toBe('5.0%')
    expect(coreFillPercent(cores[3])).toBe('14.3%')
    // 两核的填充必须真的不同（恒等值会让上面两条同时通过而柱形全一样）
    expect(coreFillPercent(cores[0])).not.toBe(coreFillPercent(cores[3]))
  })

  it('ratio 为 null 的核不注入 --core-fill，轨道加斜纹（缺失不是 0%）', async () => {
    const cores = makeSysmonCpuCores(4)
    cores[1].usage_ratio = null
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuCores: cores }))
    const wrapper = await mountView()

    const items = wrapper.findAll('[data-test="monitor-core"]')
    // 有读数的核照常注入（用数值区分「注入过」与「没注入」）
    expect(coreFillPercent(items[0])).toBe('5.0%')
    // 无读数的核：变量缺席，样式里的 `var(--core-fill, 0%)` 取兜底 0%（空轨道）；
    // 与「真的 0%」的区别由轨道斜纹（图形层）+ title（不可用）共同承担
    expect(items[1].find('.monitor-view__core-bar-fill').attributes('style') ?? '').not.toContain('--core-fill')
    expect(items[1].find('.monitor-view__core-bar').classes()).toContain('monitor-view__core-bar--unavailable')
    expect(items[1].attributes('title')).toContain('不可用')
    // 有读数的核不加斜纹（缺失与真的 0% 在图形层可区分）
    expect(items[0].find('.monitor-view__core-bar').classes()).not.toContain('monitor-view__core-bar--unavailable')
    // 真的 0% 仍会注入 `0.0%`：缺席与 0 在内联样式层面可区分
    cores[2].usage_ratio = 0
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuCores: cores }))
    await wrapper.vm.$nextTick()
    expect(coreFillPercent(wrapper.findAll('[data-test="monitor-core"]')[2])).toBe('0.0%')
  })

  it('柱数以 cpu.cores 数组为准：logical_cores 与数组不一致时也不补齐 / 不截断', async () => {
    // 契约 sysmon §2.3 保证「可用时 cores 长度 == system.logical_cores」，但这里**故意**造不一致
    // 的夹具（logical_cores=8、数组 16 项）：逐核读数的事实源是数组，按 logical_cores 补齐会凭空
    // 造出没有读数的核、截断会丢掉真实读数。AC「柱数 == logical_cores」由此成为可证伪的条件：
    // 若实现改成按 logical_cores 渲染，本条立刻失败（8 ≠ 16）。
    const snapshot = makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(16) })
    snapshot.system.logical_cores = 8
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(16)
    // 四数字的上下文仍读 logical_cores（那是「这台机器有几核」的字段），两处口径各自如实
    expect(wrapper.find('[data-test="monitor-cpu-context"]').text()).toContain('8 逻辑核')
  })
})

/**
 * 从每核条形的内联样式里取 `--core-fill` 的值（没有则返回 null）。
 *
 * 直接断言 `toContain('--core-fill')` 对任何有限 ratio 都成立，守不住「柱高 = 该核占用率」，
 * 所以这里取出变量值做精确比较。
 */
function coreFillPercent(core: DOMWrapper<Element>): string | null {
  const style = core.find('.monitor-view__core-bar-fill').attributes('style') ?? ''
  const match = /--core-fill:\s*([^;]+)/.exec(style)
  return match ? match[1].trim() : null
}

describe('MonitorView CPU 块（#435：型号 / 等级着色 / 自适应列数 / 无负载迷你趋势）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('标题行右侧显示 system.cpu_model（原样 + title）；型号为空时整行不渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const model = wrapper.find('[data-test="monitor-cpu-model"]')
    expect(model.text()).toBe('AMD Ryzen 9 7950X 16-Core Processor')
    expect(model.attributes('title')).toBe('AMD Ryzen 9 7950X 16-Core Processor')
    // 型号在标题行里、标题 h2 之外（标题只写 CPU）
    expect(wrapper.find('[data-test="monitor-cpu"] .monitor-view__cpu-head').exists()).toBe(true)

    // 同一用例里第二次挂载要换一个 pinia：store 的 init 只拉一次快照，复用旧 store 会看到上一份数据
    wrapper.unmount()
    setActivePinia(createPinia())
    const snapshot = makeSysmonSnapshot({ resourceData: true })
    snapshot.system.cpu_model = ''
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const emptyModel = await mountView()
    expect(emptyModel.find('[data-test="monitor-cpu-model"]').exists()).toBe(false)
  })

  it('占用率主数值按 resourceLevel 着色（70 / 90 两档，与顶栏同判据）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.187 }))
    let wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-cpu-value"]').attributes('data-level')).toBe('normal')

    // 每次换一份快照都换一个 pinia：store 的 init 只拉一次快照（旧 store 会继续给上一份数据）
    for (const [ratio, level] of [[0.75, 'warn'], [0.95, 'danger']] as const) {
      wrapper.unmount()
      setActivePinia(createPinia())
      vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: ratio }))
      wrapper = await mountView()
      expect(wrapper.find('[data-test="monitor-cpu-value"]').attributes('data-level')).toBe(level)
    }
    wrapper.unmount()
    setActivePinia(createPinia())

    // 未知值不套色：整格走「不可用」空态（没有 data-level 可言）
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: null }))
    wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-cpu-value"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').exists()).toBe(true)
  })

  it('每核条形列数由 ResizeObserver 实测宽度注入：32 核 / 1300px → 16 列（两行等分）', async () => {
    // jsdom 无布局：可手动触发的桩 + 假 clientWidth 驱动「宽度 → 列数」这条链路
    // （用对象持有回调：直接 let + 类构造函数赋值时 TS 的控制流分析会把变量窄成 never）
    const resize: { callback: (() => void) | null } = { callback: null }
    vi.stubGlobal('ResizeObserver', class {
      constructor(callback: () => void) { resize.callback = callback }
      observe(): void {}
      disconnect(): void {}
    })
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(32) }),
    )
    const wrapper = await mountView()
    const container = wrapper.find('[data-test="monitor-cpu-cores"]').element as HTMLElement
    // 尚未测量（jsdom clientWidth = 0）时退回单行，不误折行
    expect(wrapper.find('.monitor-view__cores').attributes('style')).toContain('--core-columns: 32')

    Object.defineProperty(container, 'clientWidth', { configurable: true, value: 1300 })
    resize.callback?.()
    await wrapper.vm.$nextTick()

    expect(wrapper.find('.monitor-view__cores').attributes('style')).toContain('--core-columns: 16')
    expect(wrapper.findAll('[data-test="monitor-core"]')).toHaveLength(32)
  })

  it('卸载时断开每核条形的 ResizeObserver（不把观察器留给已卸载的 DOM）', async () => {
    let disconnects = 0
    vi.stubGlobal('ResizeObserver', class {
      observe(): void {}
      disconnect(): void { disconnects += 1 }
    })
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(16) }),
    )
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-cpu-cores"]').exists()).toBe(true)
    expect(disconnects, '挂载时不应立即断开').toBe(0)
    wrapper.unmount()
    expect(disconnects, '组件卸载时必须断开 RO（与 Topbar 的既有纪律一致）').toBe(1)
  })

  it('型号字符串里的 @ 3.40GHz 只出现在标题行的型号里，不再有「型号标注」这类自造小字', async () => {
    const snapshot = makeSysmonSnapshot({ sensorData: true })
    snapshot.system.cpu_model = 'Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz'
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(snapshot)
    const wrapper = await mountView()

    // 型号字符串在标题行原样保留（含 @ 后缀）
    expect(wrapper.find('[data-test="monitor-cpu-model"]').text()).toContain('@ 3.40GHz')
    // 频率格不再有人造的「型号标注 …」小字
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('型号标注')
  })

  it('每核条形区没有标题行（#435 决策 5：曲线下方直接是条形）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuCores: makeSysmonCpuCores(8) }))
    const wrapper = await mountView()

    const bars = wrapper.find('[data-test="monitor-cpu-cores"]')
    expect(bars.exists()).toBe(true)
    // 没有 h3 / panel-title / 「各核占用」之类的标题文字
    expect(bars.findAll('h3')).toHaveLength(0)
    expect(bars.find('.monitor-view__panel-title').exists()).toBe(false)
    expect(bars.text()).not.toContain('各核占用')
    // 条形直接是列表项（没有额外的表头行）
    expect(bars.find('.monitor-view__cores').element.firstElementChild?.getAttribute('data-test')).toBe('monitor-core')
  })

  it('每核条形空态区分「配置关闭」与「能力不可用」（配置关闭是唯一可自助恢复的一档）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.cores': { available: false, reason: 'disabled_by_config' } },
    }))
    let wrapper = await mountView()
    let empty = wrapper.find('[data-test="monitor-cores-empty"]')
    expect(empty.find('.ds-empty__title').text()).toBe('已在配置中关闭')
    expect(empty.find('.ds-empty__desc').text()).toContain('配置')
    wrapper.unmount()

    // 能力不可用：同一位置仍是「不可用」+ 原因，不并进「已关闭」
    setActivePinia(createPinia())
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.cores': { available: false, reason: 'no_permission' } },
    }))
    wrapper = await mountView()
    empty = wrapper.find('[data-test="monitor-cores-empty"]')
    expect(empty.find('.ds-empty__title').text()).toBe('不可用')
    expect(empty.find('.ds-empty__desc').text()).toContain('权限不足，无法读取该指标')
  })

  it('负载只有数字与 5/15 分小字：不再渲染迷你趋势（#435 决策 1）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-load"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-sparkline-load"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-load"] .sysmon-sparkline').exists()).toBe(false)
  })
})

describe('MonitorView 指标台账（#363：阈值与余量同源、缺口只在有阈值的行）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  /** 从内联样式里取注入的 CSS 变量值（没有则 null） */
  function injectedVar(element: DOMWrapper<Element>, name: string): string | null {
    const style = element.attributes('style') ?? ''
    const match = new RegExp(`${name}:\\s*([^;]+)`).exec(style)
    return match ? match[1].trim() : null
  }

  it('阈值列只出现契约里真实存在的值：内存 90% / 交换 50% / 卷「10% 空闲」，磁盘 / 网卡 / 共享内存为 —', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const thresholdOf = (rowId: string) =>
      wrapper.find(`[data-row-id="${rowId}"] [data-test="ledger-threshold"]`).text()
    expect(thresholdOf('memory')).toBe('90%')
    expect(thresholdOf('swap')).toBe('50%')
    expect(thresholdOf('volume:/')).toBe('10% 空闲')
    // 磁盘忙碌占比没有契约阈值（不是「忘了写」，是契约里没有这个值）
    expect(thresholdOf('disk:nvme0n1')).toBe('—')
    expect(thresholdOf('nic:eth0:rx')).toBe('—')
    expect(thresholdOf('shm_dir')).toBe('—')
  })

  it('余量与阈值同源：把 memory_used_pct 改成 80 后阈值列与余量列同时变（前端不另定一套）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()
    const memoryRow = () => wrapper.find('[data-row-id="memory"]')
    // 32 GiB × 90% − 16 GiB 已用 = 12.8 GiB 余量
    expect(memoryRow().find('[data-test="ledger-threshold"]').text()).toBe('90%')
    expect(memoryRow().find('[data-test="ledger-headroom"]').text()).toBe('12.8 GiB')

    useSysmonStore().applyConfig(makeSysmonConfig({ thresholds: { memory_used_pct: 80 } }))
    await wrapper.vm.$nextTick()

    // 阈值与余量一起变，且余量是「阈值对应的字节上限 − 已用」而不是另一套常量
    expect(memoryRow().find('[data-test="ledger-threshold"]').text()).toBe('80%')
    expect(memoryRow().find('[data-test="ledger-headroom"]').text()).toBe('9.6 GiB')
  })

  it('轨道缺口只在有契约阈值的行上：内存 / 交换 / 卷有 70 与 90 两道，磁盘 / 网卡 / 共享内存没有', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const gapsOf = (rowId: string) => wrapper.findAll(`[data-row-id="${rowId}"] .monitor-view__ledger-gap`)
    // 内存 / 交换 / 卷：两道缺口，位置就是 RESOURCE_WARN_RATIO / RESOURCE_DANGER_RATIO
    for (const rowId of ['memory', 'swap', 'volume:/']) {
      const gaps = gapsOf(rowId)
      expect(gaps, rowId).toHaveLength(2)
      // 位置来自 RESOURCE_WARN_RATIO / RESOURCE_DANGER_RATIO（0.7 / 0.9 → 百分比一位小数）
      expect(injectedVar(gaps[0], '--ledger-gap'), rowId).toBe('70.0%')
      expect(injectedVar(gaps[1], '--ledger-gap'), rowId).toBe('90.0%')
    }
    // 没有契约阈值的行：一道都不画（不是画了再藏）
    for (const rowId of ['disk:nvme0n1', 'nic:eth0:rx', 'shm_dir']) {
      expect(gapsOf(rowId), rowId).toHaveLength(0)
    }
  })

  it('轨道填充宽度来自该行的量程比率；无量程的行（磁盘 / 网卡 / 时钟）不注入填充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ resourceData: true }))
    const wrapper = await mountView()

    const fillOf = (rowId: string) =>
      injectedVar(wrapper.find(`[data-row-id="${rowId}"] .monitor-view__ledger-track-fill`), '--ledger-fill')
    expect(fillOf('memory')).toBe('50.0%')
    expect(fillOf('swap')).toBe('25.0%')
    expect(fillOf('volume:/')).toBe('40.0%')
    // 磁盘有填充（本就忙碌占比）
    expect(fillOf('disk:nvme0n1')).toBe('72.0%')
    // 网卡 / 时钟没有量程分母 → 根本不画轨道（画一条永远空的轨道等于说「0%」）
    for (const rowId of ['nic:eth0:rx', 'clock']) {
      expect(wrapper.find(`[data-row-id="${rowId}"] .monitor-view__ledger-track`).exists(), rowId).toBe(false)
      expect(wrapper.find(`[data-row-id="${rowId}"] .monitor-view__ledger-track-empty`).exists(), rowId).toBe(true)
    }
  })

  it('内存组成带四段与主数值并存：缓存 / 缓冲不可得时退化为两段并标注', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 32 * GIB,
        available_bytes: 16 * GIB,
        used_bytes: 16 * GIB,
        used_ratio: 0.5,
        cached_bytes: 8 * GIB,
        buffers_bytes: 1 * GIB,
      },
    }))
    const wrapper = await mountView()

    const segments = () => wrapper.findAll('[data-test="monitor-memory-composition"] .monitor-view__ledger-band-seg')
    expect(segments().map(segment => segment.attributes('data-segment'))).toEqual(['used', 'cached', 'buffers', 'free'])
    // 段宽按各自占总量比例注入：已用 16/32、缓存 8/32、缓冲 1/32、空闲 7/32
    expect(injectedVar(segments()[0], '--ledger-seg')).toBe('50.0%')
    expect(injectedVar(segments()[1], '--ledger-seg')).toBe('25.0%')
    expect(injectedVar(segments()[3], '--ledger-seg')).toBe('21.9%')
    // 主数值仍是 available 口径（50.0%），与组成带并存、不合并
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-memory-composition-degraded"]').exists()).toBe(false)
  })

  it('四类空态在台账里分别可见且文案不同：不可用 / 暂无数据 / 已关闭 / 无此维度', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
      diskIo: null,
      shmDir: null,
    }))
    const wrapper = await mountView()

    // unavailable（采集失败）
    expect(wrapper.find('[data-row-id="memory"] [data-test="row-state"]').text())
      .toContain('采集失败，暂不可用')
    // none（无交换分区）
    expect(wrapper.find('[data-row-id="swap"] [data-test="row-state"]').text()).toBe('无交换分区')
    // empty（本帧未采集）
    expect(wrapper.find('[data-row-id="disk_io"] [data-test="row-state"]').text()).toContain('暂无数据')
    expect(wrapper.find('[data-row-id="shm_dir"] [data-test="row-state"]').text()).toContain('暂无数据')
    // 四类空态的值列都不是 0
    for (const rowId of ['memory', 'swap', 'disk_io', 'shm_dir']) {
      const value = wrapper.find(`[data-row-id="${rowId}"] .monitor-view__ledger-value`).text()
      expect(value, rowId).not.toBe('0')
      expect(value, rowId).not.toMatch(/^0(\.0)?%$/)
    }
  })
})
