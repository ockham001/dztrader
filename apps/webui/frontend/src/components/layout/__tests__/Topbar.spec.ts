import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '../Topbar.vue'
import { sysmonApi } from '@/api/sysmon'
import { useProcessStore } from '@/stores/process'
import { useSysmonStore } from '@/stores/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'
import type { ProcessState } from '@/types/api'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getConfig: vi.fn() },
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// #127：Topbar 经 useBreakpoint 读视口档位做行为分支（≥lg 宽屏 / <lg 窄屏）。
// jsdom 无 matchMedia，用 AppShell.spec 同款可编程 stub；默认 1600（xl）让既有宽屏断言保持。
let mockWidth = 1600
const listeners = new Set<() => void>()

beforeEach(() => {
  mockWidth = 1600
  listeners.clear()
  vi.stubGlobal('matchMedia', (q: string) => ({
    media: q,
    get matches(): boolean {
      const m = q.match(/\(min-width:\s*([\d.]+)px\)/)
      return m ? mockWidth >= Number.parseFloat(m[1]) : false
    },
    addEventListener: (_t: string, cb: () => void) => listeners.add(cb),
    removeEventListener: (_t: string, cb: () => void) => listeners.delete(cb),
  }))
})

afterEach(() => {
  vi.unstubAllGlobals()
})

function setWidth(w: number): void {
  mockWidth = w
  for (const cb of listeners) cb()
}

const stubs = {
  Icon: { template: '<span class="icon" />' },
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

async function mountTopbar() {
  const wrapper = mount(Topbar, { global: { stubs } })
  await flushPromises()
  return wrapper
}

type TopbarWrapper = Awaited<ReturnType<typeof mountTopbar>>

function cpuBadgeText(wrapper: TopbarWrapper): string {
  return wrapper.find('#resCpu .res-value').text()
}

function memBadgeText(wrapper: TopbarWrapper): string {
  return wrapper.find('#resMem .res-value').text()
}

function diskBadges(wrapper: TopbarWrapper) {
  return wrapper.findAll('[data-test="topbar-disk-badge"]')
}

function restBadge(wrapper: TopbarWrapper) {
  return wrapper.find('[data-test="topbar-disk-rest"]')
}

function resourceGauge(wrapper: TopbarWrapper) {
  return wrapper.find('[data-test="topbar-resource-gauge"]')
}

describe('Topbar 资源徽章（契约 sysmon 实时快照）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('删除写死的 NTP / 磁盘假徽章；内存徽章改由实时快照驱动，不残留假数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resNtp').exists()).toBe(false)
    expect(wrapper.find('#resDisk0').exists()).toBe(false)
    expect(wrapper.find('#resDisk1').exists()).toBe(false)
    // 内存徽章存在，但快照未到达 → 等待态，不得显示假百分比（#74）
    expect(wrapper.find('#resMem').exists()).toBe(true)
    expect(memBadgeText(wrapper)).toBe('—')
    for (const fake of ['NTP', '+12ms', '76%', '93%', '45%']) {
      expect(wrapper.text()).not.toContain(fake)
    }
  })

  it('快照尚未到达时 CPU / 内存徽章显示等待态（不得显示 0）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
    expect(memBadgeText(wrapper)).toBe('—')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
  })

  it('CPU 徽章绑定实时快照百分比', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.234 }))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('23%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--normal')
  })

  it('CPU 徽章按阈值映射警告/危险状态色', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.75 }))
    const wrapper = await mountTopbar()
    expect(cpuBadgeText(wrapper)).toBe('75%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--warn')

    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.95 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadgeText(wrapper)).toBe('95%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--danger')
  })

  it('CPU 不可用（可用性映射 false / 数值 null）时显示「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
  })

  it('内存徽章绑定实时快照使用率（#74，与 CPU 同构）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('50%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--normal')
    expect(wrapper.find('#resMem').attributes('title')).toBe('内存使用率 50%')
  })

  it('内存徽章按 70%/90% 阈值映射警告/危险状态色', async () => {
    const GIB = 1024 ** 3
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * GIB,
        available_bytes: 25 * GIB,
        used_bytes: 75 * GIB,
        used_ratio: 0.75,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountTopbar()
    expect(memBadgeText(wrapper)).toBe('75%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--warn')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * GIB,
        available_bytes: 5 * GIB,
        used_bytes: 95 * GIB,
        used_ratio: 0.95,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    await wrapper.vm.$nextTick()
    expect(memBadgeText(wrapper)).toBe('95%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--danger')
    expect(wrapper.find('#resMem').attributes('title')).toBe('内存使用率 95%')
  })

  it('内存不可用（可用性映射 false）时显示「不可用」+ 原因', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
    expect(wrapper.find('#resMem').attributes('title')).toContain('collect_failed')
  })

  it('内存可用但本帧无有效比率（payload null）时显示「不可用」，不得退回 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ memory: null }))
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
  })

  it('WS 推送更新时顶栏随之刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(memBadgeText(wrapper)).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.5 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadgeText(wrapper)).toBe('50%')
    expect(memBadgeText(wrapper)).toBe('50%')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 各盘徽章（#75：由快照卷集合驱动）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照：按卷集合渲染徽章，顺序为使用率降序（#122，不依赖发送端顺序）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())

    const wrapper = await mountTopbar()

    // #127：宽屏默认只直接显示前 2 个卷，第 3 个折叠进「其余 1 个」；展开后校验完整顺序
    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).text()).toContain('其余 1 个')
    await restBadge(wrapper).trigger('click')

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(3)
    // #122：按使用率降序（62% > 40% > 25%）
    expect(badges[0].attributes('data-mount')).toBe('/home')
    expect(badges[0].find('.res-value').text()).toBe('62%')
    // 色标 data-level 与监控页卷列表同源（<70% → normal）
    expect(badges[0].attributes('data-level')).toBe('normal')

    expect(badges[1].attributes('data-mount')).toBe('/')
    expect(badges[1].find('.res-value').text()).toBe('40%')
    expect(badges[1].attributes('data-level')).toBe('normal')

    expect(badges[2].attributes('data-mount')).toBe('/data')
    // 标签取挂载点末段（顶栏窄条 overflow:hidden），完整路径在 data-mount / title 里
    expect(badges[2].find('.res-label').text()).toBe('data')
    expect(badges[2].find('.res-value').text()).toBe('25%')
    expect(badges[2].attributes('data-level')).toBe('normal')
    expect(badges[2].attributes('title')).toBe('/data 使用率 25%')
  })

  it('卷徽章阈值边界：恰好 70% → warn、恰好 90% → danger（与监控页共用资源等级）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/warn', available: true, total_bytes: 1000 * GIB, used_bytes: 700 * GIB, used_ratio: 0.7 },
        { mount: '/danger', available: true, total_bytes: 1000 * GIB, used_bytes: 900 * GIB, used_ratio: 0.9 },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    // 上限内（2 卷）不折叠
    expect(restBadge(wrapper).exists()).toBe(false)
    // 使用率降序：danger(90%) 在前
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['/danger', '/warn'])
    expect(badges[0].attributes('data-level')).toBe('danger')
    expect(badges[0].classes()).toContain('topbar__resource--danger')
    expect(badges[1].attributes('data-level')).toBe('warn')
    expect(badges[1].classes()).toContain('topbar__resource--warn')
  })

  it('不硬编码盘符或数量：卷集合随快照变化（Windows 盘符同样按数据渲染）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: 'C:', available: true, total_bytes: 500 * GIB, used_bytes: 250 * GIB, used_ratio: 0.5 },
        { mount: 'D:', available: true, total_bytes: 1000 * GIB, used_bytes: 800 * GIB, used_ratio: 0.8 },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['D:', 'C:'])
    expect(badges[0].find('.res-value').text()).toBe('80%')
    expect(badges[0].classes()).toContain('topbar__resource--warn')
    expect(badges[0].attributes('data-level')).toBe('warn')
    expect(badges[1].find('.res-value').text()).toBe('50%')
    expect(badges[1].classes()).toContain('topbar__resource--normal')
    expect(badges[1].attributes('data-level')).toBe('normal')
  })

  it('单卷不可用：徽章显示「不可用」+ 原因，不得用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
    expect(badges[1].find('.res-value').text()).toBe('不可用')
    expect(badges[1].classes()).toContain('topbar__resource--unavailable')
    // 不可用卷不套状态色标：没有 data-level，不得用 0/normal 冒充
    expect(badges[1].attributes('data-level')).toBeUndefined()
    expect(badges[1].attributes('title')).toBe('/backup 卷不可用: no_permission')
    expect(badges[1].text()).not.toContain('0%')
  })

  it('volumes 不可用 / 快照未到达：不渲染盘徽章（不残留假数字）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(diskBadges(wrapper)).toHaveLength(0)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { volumes: { available: false, reason: 'disabled_by_config' } },
    }))
    await wrapper.vm.$nextTick()
    expect(diskBadges(wrapper)).toHaveLength(0)
  })

  it('WS 推送刷新盘徽章（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    // 默认 3 卷 → 宽屏收敛为 2 个 + 「其余 1 个」（#127）
    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).exists()).toBe(true)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/only', available: true, total_bytes: 10 * GIB, used_bytes: 9 * GIB, used_ratio: 0.9 },
      ],
    }))
    await wrapper.vm.$nextTick()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(1)
    expect(restBadge(wrapper).exists()).toBe(false)
    expect(badges[0].attributes('data-mount')).toBe('/only')
    expect(badges[0].find('.res-value').text()).toBe('90%')
    expect(badges[0].classes()).toContain('topbar__resource--danger')
    expect(badges[0].attributes('data-level')).toBe('danger')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 顶栏密度（#127：≥lg 卷徽章收敛 / <lg 单状态图标）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('3 卷 → 2 个卷徽章 + 「其余 1 个」；点击展开 3 个，再点收起', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).text()).toBe('其余 1 个')
    // 折叠控件用原生 button（可访问性：键盘 Enter/Space 原生支持，不靠 span role=button + 手写键盘处理）
    expect(restBadge(wrapper).element.tagName).toBe('BUTTON')
    expect(restBadge(wrapper).attributes('type')).toBe('button')
    expect(restBadge(wrapper).attributes('aria-expanded')).toBe('false')

    await restBadge(wrapper).trigger('click')
    expect(diskBadges(wrapper)).toHaveLength(3)
    expect(restBadge(wrapper).attributes('aria-expanded')).toBe('true')
    expect(restBadge(wrapper).text()).toBe('收起')
    // 展开后仍按使用率降序（前 2 个不变，/data 补在末尾）
    expect(diskBadges(wrapper).map(badge => badge.attributes('data-mount'))).toEqual(['/home', '/', '/data'])

    await restBadge(wrapper).trigger('click')
    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).text()).toBe('其余 1 个')
  })

  it('「其余 N 个」title 列出被折叠卷的挂载点与使用率 / 不可用原因', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/home', available: true, total_bytes: 100 * GIB, used_bytes: 62 * GIB, used_ratio: 0.62 },
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/data', available: true, total_bytes: 100 * GIB, used_bytes: 25 * GIB, used_ratio: 0.25 },
        { mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountTopbar()

    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).text()).toBe('其余 2 个')
    // 「其余 N 个」代表整组卷而非某一卷：不得带 data-mount / data-level 污染按卷比较的映射表
    expect(restBadge(wrapper).attributes('data-mount')).toBeUndefined()
    expect(restBadge(wrapper).attributes('data-level')).toBeUndefined()
    const title = restBadge(wrapper).attributes('title') ?? ''
    expect(title).toContain('/data 使用率 25%')
    // 不可用卷排末尾落进「其余」，title 如实写不可用原因（不得用 0 冒充）
    expect(title).toContain('/backup 卷不可用: no_permission')
  })

  it('宽屏（≥lg）：不渲染状态图标，也不加折叠模式类', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(resourceGauge(wrapper).exists()).toBe(false)
    expect(wrapper.find('#topbarResources').classes()).not.toContain('topbar__resources--gauge')
    // 宽屏徽章组保持：CPU / 内存仍在，卷按上限收敛
    expect(wrapper.find('#resCpu').exists()).toBe(true)
    expect(wrapper.find('#resMem').exists()).toBe(true)
    expect(diskBadges(wrapper)).toHaveLength(2)
  })

  it('窄屏（<lg）：容器加 --gauge 类并渲染单个状态图标，状态色取最差等级', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.95 }))
    setWidth(800)
    const wrapper = await mountTopbar()

    expect(wrapper.find('#topbarResources').classes()).toContain('topbar__resources--gauge')
    const gauge = resourceGauge(wrapper)
    expect(gauge.exists()).toBe(true)
    // 既有 CSS 的状态色 / 闪烁按 `.topbar__gauge ... svg` 选择器书写，因此必须是内联 svg
    expect(gauge.find('svg').exists()).toBe(true)
    // CPU 95% 为 CPU / 内存 / 卷中的最差等级
    expect(gauge.classes()).toContain('topbar__resource--danger')
    // title 汇总各资源读数
    const title = gauge.attributes('title') ?? ''
    expect(title).toContain('CPU 使用率 95%')
    expect(title).toContain('内存使用率 50%')
    expect(title).toContain('/home 使用率 62%')
    // 窄屏不渲染「其余 N 个」点击控件（它只属于宽屏的折叠/展开）
    expect(restBadge(wrapper).exists()).toBe(false)
  })

  it('窄屏状态图标：任一卷达到 warn 阈值即为 warn（与监控页共用 resourceLevel）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: 0.2,
      volumes: [
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 20 * GIB, used_ratio: 0.2 },
        { mount: '/data', available: true, total_bytes: 100 * GIB, used_bytes: 75 * GIB, used_ratio: 0.75 },
      ],
    }))
    setWidth(800)
    const wrapper = await mountTopbar()

    expect(resourceGauge(wrapper).classes()).toContain('topbar__resource--warn')
  })

  it('窄屏状态图标：全部资源正常时为 normal', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.2 }))
    setWidth(800)
    const wrapper = await mountTopbar()

    expect(resourceGauge(wrapper).classes()).toContain('topbar__resource--normal')
  })

  it('窄屏状态图标：快照未到达时中性等待态（不得用 normal 冒充「一切正常」）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    setWidth(800)
    const wrapper = await mountTopbar()

    const gauge = resourceGauge(wrapper)
    expect(gauge.exists()).toBe(true)
    expect(gauge.classes()).toContain('topbar__resource--unavailable')
    expect(gauge.classes()).not.toContain('topbar__resource--normal')
    expect(gauge.attributes('title')).toContain('等待')
  })

  it('跨断点切换：宽屏 ↔ 窄屏时状态图标与 --gauge 类响应式出现 / 消失', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    expect(resourceGauge(wrapper).exists()).toBe(false)
    expect(diskBadges(wrapper)).toHaveLength(2)

    setWidth(800)  // xl → md（窄屏）
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#topbarResources').classes()).toContain('topbar__resources--gauge')
    expect(resourceGauge(wrapper).exists()).toBe(true)

    setWidth(1200)  // → lg（宽屏）
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#topbarResources').classes()).not.toContain('topbar__resources--gauge')
    expect(resourceGauge(wrapper).exists()).toBe(false)
    expect(diskBadges(wrapper)).toHaveLength(2)
  })

  it('展开态跨断点复位：宽屏展开全部 → 窄屏 → 回宽屏，恢复「前 2 + 其余 N 个」默认密度', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    await restBadge(wrapper).trigger('click')
    expect(diskBadges(wrapper)).toHaveLength(3)

    setWidth(800)  // → 窄屏
    await wrapper.vm.$nextTick()
    setWidth(1200)  // → 宽屏
    await wrapper.vm.$nextTick()

    // 展开态不跨断点保留：回到宽屏是默认密度（AC「不持久化」）
    expect(diskBadges(wrapper)).toHaveLength(2)
    expect(restBadge(wrapper).text()).toBe('其余 1 个')
  })
})

describe('Topbar 服务状态徽章（#199：聚合 process_status 镜像）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    // 资源快照与 #199 无关：固定给成功响应，避免 REST 失败噪声
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
  })

  const SERVICE_ANCHORS = ['resMd', 'resTd', 'resStg'] as const
  type ServiceAnchor = typeof SERVICE_ANCHORS[number]

  function serviceBadge(wrapper: TopbarWrapper, id: ServiceAnchor) {
    return wrapper.find(`#${id}`)
  }

  /** 模拟 WS process_status 帧到达（与 wsHandlers 一样走 store 的 applyProcessStatus） */
  function applyStatuses(entries: Array<{ name: string; state: ProcessState }>): void {
    const store = useProcessStore()
    for (const entry of entries) {
      store.applyProcessStatus({ ...entry, pid: entry.state === 'Running' ? 100 : 0 })
    }
  }

  function expectLevel(badge: ReturnType<typeof serviceBadge>, level: string): void {
    expect(badge.attributes('data-level')).toBe(level)
    expect(badge.classes()).toContain(`topbar__resource--${level}`)
  }

  function expectNeutralUnknown(badge: ReturnType<typeof serviceBadge>): void {
    expect(badge.exists()).toBe(true)
    expect(badge.classes()).toContain('topbar__resource--unavailable')
    expect(badge.classes()).not.toContain('topbar__resource--normal')
    expect(badge.classes()).not.toContain('topbar__resource--warn')
    expect(badge.classes()).not.toContain('topbar__resource--danger')
    // 未知不是色标等级：不写 data-level（与卷徽章的「不可用」一致）
    expect(badge.attributes('data-level')).toBeUndefined()
    expect(badge.attributes('title')).toContain('未知')
  }

  it('镜像未到达 / 无任何进程：MD / TD / STG 均为中性「未知」，不得默认绿', async () => {
    const wrapper = await mountTopbar()

    for (const id of SERVICE_ANCHORS) {
      expectNeutralUnknown(serviceBadge(wrapper, id))
    }

    // 进程镜像到达但该类别无进程：仍是中性未知，title 说明无对应进程
    applyStatuses([{ name: 'dzweb', state: 'Running' }])
    await wrapper.vm.$nextTick()
    for (const id of SERVICE_ANCHORS) {
      expectNeutralUnknown(serviceBadge(wrapper, id))
    }
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('无对应进程')
  })

  it('dzmd_ctp Running → MD 绿；TD / STG 无进程时保持中性未知', async () => {
    const wrapper = await mountTopbar()
    applyStatuses([{ name: 'dzmd_ctp', state: 'Running' }])
    await wrapper.vm.$nextTick()

    expectLevel(serviceBadge(wrapper, 'resMd'), 'normal')
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('dzmd_ctp 运行中')
    expectNeutralUnknown(serviceBadge(wrapper, 'resTd'))
    expectNeutralUnknown(serviceBadge(wrapper, 'resStg'))
  })

  it('MD 进程 Stopped / Crashed → MD 红（不再默认绿）', async () => {
    const wrapper = await mountTopbar()

    applyStatuses([{ name: 'dzmd_ctp', state: 'Stopped' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'danger')
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('dzmd_ctp 已停止')

    applyStatuses([{ name: 'dzmd_ctp', state: 'Crashed' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'danger')
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('dzmd_ctp 已崩溃')
  })

  it('TD / STG 与 MD 同构：进程 Stopped / Crashed → 红', async () => {
    const wrapper = await mountTopbar()

    for (const [anchor, name] of [['resTd', 'dztd_ctp'], ['resStg', 'stg_demo']] as const) {
      applyStatuses([{ name, state: 'Stopped' }])
      await wrapper.vm.$nextTick()
      expectLevel(serviceBadge(wrapper, anchor), 'danger')

      applyStatuses([{ name, state: 'Crashed' }])
      await wrapper.vm.$nextTick()
      expectLevel(serviceBadge(wrapper, anchor), 'danger')
    }
  })

  it('dztd_* 归入 TD、stg_* 归入 STG（按进程名类别聚合）', async () => {
    const wrapper = await mountTopbar()
    applyStatuses([
      { name: 'dztd_ctp', state: 'Running' },
      { name: 'stg_demo', state: 'Stopped' },
    ])
    await wrapper.vm.$nextTick()

    expectLevel(serviceBadge(wrapper, 'resTd'), 'normal')
    expect(serviceBadge(wrapper, 'resTd').attributes('title')).toContain('dztd_ctp 运行中')
    expectLevel(serviceBadge(wrapper, 'resStg'), 'danger')
    expect(serviceBadge(wrapper, 'resStg').attributes('title')).toContain('stg_demo 已停止')
    // 同帧下 MD 无进程：中性未知
    expectNeutralUnknown(serviceBadge(wrapper, 'resMd'))
  })

  it('多进程聚合取最差等级（danger > warn > normal）：Running + Stopped → danger；Starting + Running → warn', async () => {
    const wrapper = await mountTopbar()
    applyStatuses([
      { name: 'dzmd_ctp', state: 'Running' },
      { name: 'dzmd_xtp', state: 'Stopped' },
    ])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'danger')
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('dzmd_ctp 运行中')
    expect(serviceBadge(wrapper, 'resMd').attributes('title')).toContain('dzmd_xtp 已停止')

    // Starting 是过渡态（warn）：不得因另一个 Running 进程被稀释成正常
    applyStatuses([{ name: 'dzmd_xtp', state: 'Starting' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'warn')
  })

  it('状态随 WS 推送实时更新（无需刷新）', async () => {
    const wrapper = await mountTopbar()
    expectNeutralUnknown(serviceBadge(wrapper, 'resMd'))

    applyStatuses([{ name: 'dzmd_ctp', state: 'Running' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'normal')

    applyStatuses([{ name: 'dzmd_ctp', state: 'Crashed' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'danger')
  })

  it('未知态不闪烁：只带中性类，不含 layout.css 挂 animation 的 warn / danger 类', async () => {
    const wrapper = await mountTopbar()

    for (const id of SERVICE_ANCHORS) {
      const classes = serviceBadge(wrapper, id).classes()
      expect(classes).not.toContain('topbar__resource--warn')
      expect(classes).not.toContain('topbar__resource--danger')
      expect(classes).toContain('topbar__resource--unavailable')
    }

    // 真实告警才允许命中闪烁规则（layout.css：.topbar__resource--warn/--danger .res-icon 才有 animation）
    applyStatuses([{ name: 'dzmd_ctp', state: 'Starting' }])
    await wrapper.vm.$nextTick()
    expect(serviceBadge(wrapper, 'resMd').classes()).toContain('topbar__resource--warn')
  })

  it('已移除进程的旧状态不再计入聚合：删除唯一 dzmd 后 MD 回到中性未知', async () => {
    const wrapper = await mountTopbar()
    const store = useProcessStore()
    applyStatuses([{ name: 'dzmd_ctp', state: 'Running' }])
    await wrapper.vm.$nextTick()
    expectLevel(serviceBadge(wrapper, 'resMd'), 'normal')

    // 契约 process：process_config 全量覆盖、条目消失 = 进程已移除——状态镜像随之清理
    store.applyProcessConfig({})
    await wrapper.vm.$nextTick()
    expectNeutralUnknown(serviceBadge(wrapper, 'resMd'))
  })
})
