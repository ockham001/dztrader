import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '../Topbar.vue'
import { sysmonApi } from '@/api/sysmon'
import { useProcessStore } from '@/stores/process'
import { useSysmonStore } from '@/stores/sysmon'
import { visibleItemCount } from '@/composables/topbarOverflow'
import {
  SYSMON_CLOCK_ALL_FAILED,
  SYSMON_CLOCK_LIVE,
  SYSMON_CLOCK_UNPROBED,
  makeSysmonConfig,
  makeSysmonSnapshot,
} from '@/test-utils/sysmonSnapshot'
import type { ProcessState } from '@/types/api'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getConfig: vi.fn() },
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// #127 / ADR 0048：Topbar 经 useBreakpoint 读视口档位做窄屏收敛分支（≥lg 宽度自适应 / <lg 单图标）。
// jsdom 无 matchMedia，用 AppShell.spec 同款可编程 stub；默认 1600（xl）走宽屏。
let mockWidth = 1600
const listeners = new Set<() => void>()

// ADR 0048：折叠要读容器实测宽度 —— jsdom 无布局（clientWidth 恒 0），因此用可手动触发的
// ResizeObserver 桩 + 在用例里给容器 / 测量行打假尺寸，才能驱动「放不下 → 折叠」的分支。
let resizeCallback: (() => void) | null = null

beforeEach(() => {
  mockWidth = 1600
  listeners.clear()
  resizeCallback = null
  vi.stubGlobal('ResizeObserver', class {
    constructor(callback: () => void) { resizeCallback = callback }
    observe(): void {}
    disconnect(): void {}
  })
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
  // 溢出浮层 Teleport 到 body：wrapper.unmount() 不会带走它，用例之间必须手动清理
  document.body.querySelectorAll('[data-test="topbar-overflow-panel"]').forEach(el => el.remove())
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

function ntpBadge(wrapper: TopbarWrapper) {
  return wrapper.find('#resNtp')
}

function ntpBadgeText(wrapper: TopbarWrapper): string {
  return wrapper.find('#resNtp .res-value').text()
}

function diskBadges(wrapper: TopbarWrapper) {
  return wrapper.findAll('[data-test="topbar-disk-badge"]')
}

function overflowTrigger(wrapper: TopbarWrapper) {
  return wrapper.find('[data-test="topbar-overflow-trigger"]')
}

/**
 * 溢出浮层**不在组件子树里**：它 Teleport 到 body（资源组行有 `overflow: hidden`，
 * 容器内绝对定位会被裁掉）。因此必须到 document.body 里找，`wrapper.find` 找不到。
 */
function overflowPanel() {
  const element = document.body.querySelector('[data-test="topbar-overflow-panel"]')
  if (!element) return null
  return { element, findAll: (selector: string) => Array.from(element.querySelectorAll(selector)) }
}

function resourceGauge(wrapper: TopbarWrapper) {
  return wrapper.find('[data-test="topbar-resource-gauge"]')
}

describe('Topbar 资源徽章（契约 sysmon 实时快照）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('写死的假徽章全部移除：不得残留 CPU / 内存 / 磁盘 / NTP 的假数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resDisk0').exists()).toBe(false)
    expect(wrapper.find('#resDisk1').exists()).toBe(false)
    // CPU / 内存徽章存在，但快照未到达 → 等待态，不得显示假百分比（#74）
    expect(wrapper.find('#resCpu').exists()).toBe(true)
    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(memBadgeText(wrapper)).toBe('—')
    for (const fake of ['+12ms', '76%', '93%', '45%']) {
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

    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--warn')
    expect(wrapper.find('#resCpu').attributes('title')).toBe('CPU 使用率 75%')
  })

  it('CPU 不可用（可用性映射 false / 数值 null）时显示「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.total': { available: false, reason: 'collect_failed' } },
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
  })

  it('内存徽章按 70%/90% 阈值映射警告/危险状态色（95% → danger、75% → warn）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * 1024 ** 3,
        available_bytes: 5 * 1024 ** 3,
        used_bytes: 95 * 1024 ** 3,
        used_ratio: 0.95,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountTopbar()

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

describe('Topbar NTP 徽章（ADR 0048 / 契约 sysmon §2.9 顶栏徽章条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('位于 CPU / 内存之后、卷徽章之前', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    const items = wrapper.findAll('#resCpu, #resMem, #resNtp, [data-test="topbar-disk-badge"]')
    expect(items.map(item => item.attributes('id') ?? item.attributes('data-mount')))
      .toEqual(['resCpu', 'resMem', 'resNtp', '/', '/data', '/home'])
  })

  it('正常态：显示紧凑带符号毫秒，title 含精确偏差 / 误差界 / 来源 / 上次探测', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    // 12.5 ms → `+13ms`（Math.round 到最近整毫秒；精确值在 title 里）
    expect(ntpBadgeText(wrapper)).toBe('+13ms')
    const title = ntpBadge(wrapper).attributes('title') ?? ''
    expect(title).toContain('+12.5 ms')
    expect(title).toContain('18.3 ms')
    expect(title).toContain('ntp.aliyun.com')
    expect(title).toContain('上次探测 30 秒前')
    // 误差界 18.3 < 200：不带质量提示
    expect(title).not.toContain('测量质量')
  })

  it('偏差按配置阈值定档：100ms warn / 1000ms danger；负偏差按绝对值', async () => {
    const cases = [
      { offsetMs: 90, expected: 'topbar__resource--normal' },
      { offsetMs: 100, expected: 'topbar__resource--warn' },
      { offsetMs: -350.4, expected: 'topbar__resource--warn' },
      { offsetMs: 1000, expected: 'topbar__resource--danger' },
    ] as const
    for (const { offsetMs, expected } of cases) {
      // 每轮重建 pinia：store 有 `loaded` 短路，复用实例会让后续轮次看不到新快照
      setActivePinia(createPinia())
      vi.clearAllMocks()
      vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
        clock: { ...SYSMON_CLOCK_LIVE, offset_ms: offsetMs },
      }))
      const wrapper = await mountTopbar()
      expect(ntpBadge(wrapper).classes()).toContain(expected)
      if (offsetMs < 0) expect(ntpBadgeText(wrapper)).toBe('-350ms')
      wrapper.unmount()
    }
  })

  it('阈值取生效配置（不是写死的 100 / 1000）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    // 配置镜像未到达时按契约默认值 100 / 1000：12.5 ms 落在 normal
    expect(wrapper.find('#resNtp').classes()).toContain('topbar__resource--normal')

    // 放宽到 warn=200 后同一读数仍是 normal，收紧到 warn=10 后变 warn
    const sysmon = useSysmonStore()
    sysmon.applyConfig(makeSysmonConfig({ clock: { offset_warn_ms: 10 } }))
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#resNtp').classes()).toContain('topbar__resource--warn')
  })

  it('误差界越界只在 title 提示「测量质量」，不改变偏差着色（契约 §2.8）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 20, bound_ms: 400 },
    }))
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resNtp').classes()).toContain('topbar__resource--normal')
    expect(ntpBadge(wrapper).attributes('title')).toContain('测量质量')
  })

  it('尚未探测：显示 `—`（中性等待态），不得用 0 冒称', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_UNPROBED }))
    const wrapper = await mountTopbar()

    expect(ntpBadgeText(wrapper)).toBe('—')
    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    expect(ntpBadge(wrapper).attributes('title')).toContain('尚未探测')
  })

  it('本轮全部失败：显示「不可用」+ 失败原因汇总，不得用 0 或上一轮结果冒称', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_ALL_FAILED }))
    const wrapper = await mountTopbar()

    expect(ntpBadgeText(wrapper)).toBe('不可用')
    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    const title = ntpBadge(wrapper).attributes('title') ?? ''
    expect(title).toContain('超时未响应')
    expect(title).toContain('无法解析或连接失败')
  })

  it('配置关闭（clock = null）：整枚徽章不渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountTopbar()

    expect(ntpBadge(wrapper).exists()).toBe(false)
  })

  it('快照未到达：显示 `—` 等待态（不渲染「尚未探测」文案）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(ntpBadgeText(wrapper)).toBe('—')
    expect(ntpBadge(wrapper).attributes('title')).toContain('等待')
  })

  it('WS 推送后随快照刷新', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_UNPROBED }))
    const wrapper = await mountTopbar()
    expect(ntpBadgeText(wrapper)).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    await wrapper.vm.$nextTick()
    expect(ntpBadgeText(wrapper)).toBe('+13ms')
  })
})

describe('Topbar 各盘徽章（#75：由快照卷集合驱动，ADR 0048 固定顺序 + 状态点）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照：卷按固定顺序（深度升序 → 名称升序）渲染，徽章只给状态点不给百分比', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())

    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    // ADR 0048：/ 深度 0 在最前，/data、/home 同深度按名称升序
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['/', '/data', '/home'])
    for (const badge of badges) {
      // 卷徽章不再平铺百分比（ADR 0048）：没有 .res-value，只有状态点 + 短标签
      expect(badge.find('.res-value').exists()).toBe(false)
      expect(badge.find('.res-dot').exists()).toBe(true)
      expect(badge.attributes('data-level')).toBe('normal')
    }
    // 完整挂载点与使用率在 title 里
    expect(badges[0].attributes('title')).toBe('/ 使用率 40%')
    expect(badges[1].attributes('title')).toBe('/data 使用率 25%')
    // 标签取挂载点末段（root 保留 `/`），完整路径在 data-mount / title 里
    expect(badges[0].find('.res-label').text()).toBe('/')
    expect(badges[1].find('.res-label').text()).toBe('data')
    expect(badges[2].find('.res-label').text()).toBe('home')
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
    // ADR 0048 固定顺序：同深度按名称升序 → /danger 在 /warn 之前（与使用率无关）
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['/danger', '/warn'])
    expect(badges[0].attributes('data-level')).toBe('danger')
    expect(badges[0].classes()).toContain('topbar__resource--danger')
    expect(badges[1].attributes('data-level')).toBe('warn')
    expect(badges[1].classes()).toContain('topbar__resource--warn')
  })

  it('不硬编码盘符或数量：Windows 盘符同样按固定顺序渲染（C: 在 D: 之前）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: 'C:', available: true, total_bytes: 500 * GIB, used_bytes: 250 * GIB, used_ratio: 0.5 },
        { mount: 'D:', available: true, total_bytes: 1000 * GIB, used_bytes: 800 * GIB, used_ratio: 0.8 },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
    // 两个盘符根同深度 → 按名称升序：系统盘 C: 天然最前（不是「角色」判定，ADR 0048）
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['C:', 'D:'])
    expect(badges[0].classes()).toContain('topbar__resource--normal')
    expect(badges[0].attributes('data-level')).toBe('normal')
    expect(badges[0].attributes('title')).toBe('C: 使用率 50%')
    expect(badges[1].classes()).toContain('topbar__resource--warn')
    expect(badges[1].attributes('data-level')).toBe('warn')
    expect(badges[1].attributes('title')).toBe('D: 使用率 80%')
  })

  it('单卷不可用：徽章走中性色，title 写不可用原因，不得用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
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
    expect(diskBadges(wrapper)).toHaveLength(3)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/only', available: true, total_bytes: 10 * GIB, used_bytes: 9 * GIB, used_ratio: 0.9 },
      ],
    }))
    await wrapper.vm.$nextTick()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(1)
    expect(badges[0].attributes('data-mount')).toBe('/only')
    expect(badges[0].classes()).toContain('topbar__resource--danger')
    expect(badges[0].attributes('data-level')).toBe('danger')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 资源折叠（ADR 0048：按可用宽度自适应，服务徽章永不折叠）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('折叠算法：放得下就全部显示，放不下从尾部收起并预留溢出入口宽度', () => {
    // 纯函数单测（组件里的实测宽度在 jsdom 里恒为 0，无法驱动折叠分支）
    expect(visibleItemCount([60, 60, 60], 300, 14, 34)).toBe(3)
    // 60*4 + 14*3 = 282 ≤ 300：4 项仍全部可见（不预留溢出入口）
    expect(visibleItemCount([60, 60, 60, 60], 300, 14, 34)).toBe(4)
    // 60*5 + 14*4 = 356 > 300 → 预算 300-34-14 = 252：第 4 项后累计 282 > 252，只显示 3 项
    expect(visibleItemCount([60, 60, 60, 60, 60], 300, 14, 34)).toBe(3)
    // 连一项都放不下（预算 240-34-14 = 192 < 200 的第一项）
    expect(visibleItemCount([200, 200], 240, 14, 34)).toBe(0)
    // 未测量（0）时按「全部可见」——测试环境不会把徽章全折掉
    expect(visibleItemCount([60, 60], 0, 14, 34)).toBe(2)
    expect(visibleItemCount([], 300, 14, 34)).toBe(0)
  })

  it('未测量宽度（jsdom）时全部资源项直接显示，且无溢出入口', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    // 默认快照：CPU / MEM / NTP + 3 卷 = 6 项全部可见
    expect(wrapper.find('#resCpu').exists()).toBe(true)
    expect(wrapper.find('#resMem').exists()).toBe(true)
    expect(ntpBadge(wrapper).exists()).toBe(true)
    expect(diskBadges(wrapper)).toHaveLength(3)
    expect(overflowTrigger(wrapper).exists()).toBe(false)
  })

  it('MD / TD / STG 服务徽章永不折叠：资源项被折叠起来时它们仍在，且不重复渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    for (const id of ['resMd', 'resTd', 'resStg']) {
      expect(wrapper.find(`#${id}`).exists()).toBe(true)
      // 服务徽章在资源组行之外：测量行（隐形第二份渲染）里没有它们的副本
      expect(wrapper.findAll(`#${id}`)).toHaveLength(1)
    }

    // 真正进入折叠态：资源组行可用宽度 120、每项 60（+14 间距）→ 装不下 6 项，
    // 尾部若干项收进「▾ N」（与「溢出浮层」用例同一套测量桩）
    const container = wrapper.find('[data-test="topbar-resource-run"]').element as HTMLElement
    const measureRow = wrapper.find('[data-test="topbar-resource-measure"]').element as HTMLElement
    Object.defineProperty(container, 'clientWidth', { configurable: true, value: 120 })
    for (const child of Array.from(measureRow.children)) {
      child.getBoundingClientRect = () => ({ width: 60, height: 20, top: 0, left: 0, right: 0, bottom: 0, x: 0, y: 0, toJSON: () => ({}) })
    }
    resizeCallback?.()
    await wrapper.vm.$nextTick()

    // 折叠确实发生了（否则本用例又退化成「没进入折叠态」的恒真断言）
    expect(overflowTrigger(wrapper).exists()).toBe(true)
    expect(diskBadges(wrapper).length).toBeLessThan(3)

    // 服务徽章不受折叠影响：仍在、仍只一份、仍带各自的 id 锚点
    for (const id of ['resMd', 'resTd', 'resStg']) {
      expect(wrapper.find(`#${id}`).exists()).toBe(true)
      expect(wrapper.findAll(`#${id}`)).toHaveLength(1)
    }
  })

  it('隐形测量行渲染全部资源项、不进可访问性树', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    const measure = wrapper.find('[data-test="topbar-resource-measure"]')
    expect(measure.exists()).toBe(true)
    expect(measure.attributes('aria-hidden')).toBe('true')
    // CPU / MEM / NTP + 3 卷 = 6 项（与可见项同集合）
    expect(measure.findAll('.topbar__resource')).toHaveLength(6)
  })

  it('溢出浮层：宽度不足时收起尾部项，点击入口打开浮层逐条列出，再点收起', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    // jsdom 没有布局：ResizeObserver 是可手动触发的桩，给资源组行 / 测量行打上假尺寸
    const container = wrapper.find('[data-test="topbar-resource-run"]').element as HTMLElement
    const measureRow = wrapper.find('[data-test="topbar-resource-measure"]').element as HTMLElement
    Object.defineProperty(container, 'clientWidth', { configurable: true, value: 300 })
    for (const child of Array.from(measureRow.children)) {
      child.getBoundingClientRect = () => ({ width: 60, height: 20, top: 0, left: 0, right: 0, bottom: 0, x: 0, y: 0, toJSON: () => ({}) })
    }
    resizeCallback?.()
    await wrapper.vm.$nextTick()

    // 6 项各 60px + 间距 14 → 预算 300-34-14 = 252：第 4 项后累计 282 > 252 → 只显示前 3 项
    // （CPU / MEM / NTP 直显，3 个卷徽章全部收进浮层）
    expect(diskBadges(wrapper)).toHaveLength(0)
    expect(overflowTrigger(wrapper).exists()).toBe(true)
    expect(overflowTrigger(wrapper).attributes('aria-expanded')).toBe('false')
    expect(overflowTrigger(wrapper).text()).toContain('3')

    await overflowTrigger(wrapper).trigger('click')
    expect(overflowPanel()).not.toBeNull()
    expect(overflowTrigger(wrapper).attributes('aria-expanded')).toBe('true')
    // 浮层逐条列出被收起的 3 个卷，条目带标题与状态色点
    const rows = overflowPanel()!.findAll('.topbar__overflow-item')
    expect(rows).toHaveLength(3)
    // 浮层显示完整挂载点（徽章的短标签只是窄条妥协）
    expect(rows.map(row => row.querySelector('.topbar__overflow-label')?.textContent)).toEqual(['/', '/data', '/home'])
    // 浮层补上读数：徽章本身只有状态点，浮层是「点开看细节」入口
    expect(rows.map(row => row.querySelector('.topbar__overflow-value')?.textContent))
      .toEqual(['40%', '25%', '62%'])
    expect(rows[0].getAttribute('title')).toBe('/ 使用率 40%')

    await overflowTrigger(wrapper).trigger('click')
    expect(overflowPanel()).toBeNull()
  })

  it('宽度恢复后折叠项全部回到直显，浮层随之关闭', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    const container = wrapper.find('[data-test="topbar-resource-run"]').element as HTMLElement
    const measureRow = wrapper.find('[data-test="topbar-resource-measure"]').element as HTMLElement
    Object.defineProperty(container, 'clientWidth', { configurable: true, value: 300 })
    for (const child of Array.from(measureRow.children)) {
      child.getBoundingClientRect = () => ({ width: 60, height: 20, top: 0, left: 0, right: 0, bottom: 0, x: 0, y: 0, toJSON: () => ({}) })
    }
    resizeCallback?.()
    await wrapper.vm.$nextTick()
    await overflowTrigger(wrapper).trigger('click')
    expect(overflowPanel()).not.toBeNull()

    Object.defineProperty(container, 'clientWidth', { configurable: true, value: 1000 })
    resizeCallback?.()
    await wrapper.vm.$nextTick()
    await wrapper.vm.$nextTick()

    expect(overflowTrigger(wrapper).exists()).toBe(false)
    expect(overflowPanel()).toBeNull()
    expect(diskBadges(wrapper)).toHaveLength(3)
  })

  it('宽屏（≥lg）：不渲染状态图标，也不加折叠模式类', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(resourceGauge(wrapper).exists()).toBe(false)
    expect(wrapper.find('#topbarResources').classes()).not.toContain('topbar__resources--gauge')
    expect(wrapper.find('#resCpu').exists()).toBe(true)
    expect(wrapper.find('#resMem').exists()).toBe(true)
    expect(diskBadges(wrapper)).toHaveLength(3)
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
    // CPU 95% 为 CPU / 内存 / 时钟 / 卷中的最差等级
    expect(gauge.classes()).toContain('topbar__resource--danger')
    // title 汇总各资源读数
    const title = gauge.attributes('title') ?? ''
    expect(title).toContain('CPU 使用率 95%')
    expect(title).toContain('内存使用率 50%')
    expect(title).toContain('/ 使用率 40%')
    // 窄屏不渲染溢出入口（它只属于宽屏的折叠）
    expect(overflowTrigger(wrapper).exists()).toBe(false)
  })

  it('窄屏状态图标：NTP 偏差 danger 参与「取最差」，时钟问题不被折叠掉', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: 0.2,
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 1500 },
    }))
    setWidth(800)
    const wrapper = await mountTopbar()

    expect(resourceGauge(wrapper).classes()).toContain('topbar__resource--danger')
    expect(resourceGauge(wrapper).attributes('title')).toContain('时钟偏差')
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
    expect(diskBadges(wrapper)).toHaveLength(3)

    setWidth(800)  // xl → md（窄屏）
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#topbarResources').classes()).toContain('topbar__resources--gauge')
    expect(resourceGauge(wrapper).exists()).toBe(true)

    setWidth(1200)  // → lg（宽屏）
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#topbarResources').classes()).not.toContain('topbar__resources--gauge')
    expect(resourceGauge(wrapper).exists()).toBe(false)
    expect(diskBadges(wrapper)).toHaveLength(3)
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
