import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss from 'postcss'
import Topbar from '../Topbar.vue'
import { sysmonApi } from '@/api/sysmon'
import { useProcessStore } from '@/stores/process'
import { useSysmonStore } from '@/stores/sysmon'
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

// #452 / ADR 0054 顶栏重做：资源区只剩 CPU / 内存两枚常驻徽章（任何宽度可见），
// 右上角是墙钟 + 偏差括号；卷徽章、独立 NTP 徽章、按宽度自适应折叠与 `<lg` 单状态图标
// 全部删除。本 spec 锁三组事实：
//   1. 资源区只渲染 CPU / 内存（含 warn / danger 状态色与既有 title 细节）；
//   2. 已删除的形态没有回归（DOM 与 layout.css 两侧都断言：只删一处会被这里抓住）；
//   3. 墙钟的四档 DOM 片段 / 偏差配色 / 空态 / tooltip / tick / visibility / 卸载清理。
// 四档格式的**可见性**是纯 CSS 断点（jsdom 不做布局），因此用 layout.css 源码断言补位
// （与 MonitorViewTrend.spec 的响应式断言同一写法）。

const LAYOUT_CSS = join(
  dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'assets', 'css', 'layout.css',
)
const LAYOUT_CSS_SOURCE = readFileSync(LAYOUT_CSS, 'utf8')
const TOPBAR_SOURCE = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'Topbar.vue'), 'utf8')

/** 固定时钟时刻：2026-01-01（周四）13:25:36 本地时间 */
const FIXED_NOW = new Date(2026, 0, 1, 13, 25, 36)

const stubs = {
  Icon: { template: '<span class="icon" />' },
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

/** 已挂载的 wrapper：用例结束统一卸载（墙钟有 interval，不卸载会跨用例泄漏） */
const mountedWrappers: VueWrapper[] = []

async function mountTopbar(): Promise<VueWrapper> {
  const wrapper = mount(Topbar, { global: { stubs } })
  mountedWrappers.push(wrapper)
  await flushPromises()
  return wrapper
}

/** 手动卸载（从统一卸载表里摘掉，避免重复 unmount） */
function unmountManually(wrapper: VueWrapper): void {
  const index = mountedWrappers.indexOf(wrapper)
  if (index >= 0) mountedWrappers.splice(index, 1)
  wrapper.unmount()
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
})

afterEach(() => {
  for (const wrapper of mountedWrappers.splice(0)) wrapper.unmount()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

// ===== 选择器助手 =====

function cpuBadge(wrapper: VueWrapper) {
  return wrapper.find('#resCpu')
}

function memBadge(wrapper: VueWrapper) {
  return wrapper.find('#resMem')
}

function clockEl(wrapper: VueWrapper) {
  return wrapper.find('[data-test="topbar-clock"]')
}

function clockTimeText(wrapper: VueWrapper): string {
  return wrapper.find('[data-test="topbar-clock-time"]').text()
}

function clockOffset(wrapper: VueWrapper) {
  return wrapper.find('[data-test="topbar-clock-offset"]')
}

/** 资源徽章（不含 MD / TD / STG 服务徽章） */
function resourceBadges(wrapper: VueWrapper) {
  return wrapper.findAll('.topbar__resource:not(.topbar__resource--svc)')
}

// ===== layout.css 断言助手（响应式不可见性只能从样式表断言）=====

const CSS_ROOT = postcss.parse(LAYOUT_CSS_SOURCE)

/** 某选择器在（可选）媒体块里的规则体文本；`media === null` 取基础规则 */
function cssDecls(selector: string, media: string | null): string[] {
  const bodies: string[] = []
  CSS_ROOT.walkRules((rule) => {
    if (!rule.selectors.some(part => part.trim() === selector)) return
    const parent = rule.parent
    const atRule = parent && parent.type === 'atrule' ? parent as postcss.AtRule : null
    if (media === null && atRule !== null) return
    const params = media === null ? null : media.replace(/^@media\s+/, '')
    if (params !== null && (!atRule || atRule.params !== params)) return
    bodies.push(rule.toString())
  })
  return bodies
}

/** 选择器在**任何**位置被 `display: none` 隐藏过吗（媒体块或基础规则） */
function isHiddenAnywhere(selector: string): boolean {
  let hidden = false
  CSS_ROOT.walkRules((rule) => {
    if (!rule.selectors.some(part => part.trim() === selector)) return
    rule.walkDecls('display', (decl) => {
      if (decl.value.trim() === 'none') hidden = true
    })
  })
  return hidden
}

describe('Topbar 资源徽章（CPU / 内存常驻；#452 / ADR 0054）', () => {
  it('只渲染 CPU / 内存两枚资源徽章：卷徽章 / NTP 徽章 / overflow / gauge / 测量行全部消失', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    expect(resourceBadges(wrapper)).toHaveLength(2)
    expect(cpuBadge(wrapper).exists()).toBe(true)
    expect(memBadge(wrapper).exists()).toBe(true)

    for (const selector of [
      '#resNtp',
      '[data-test="topbar-disk-badge"]',
      '[data-test="topbar-overflow-trigger"]',
      '[data-test="topbar-overflow-panel"]',
      '[data-test="topbar-resource-gauge"]',
      '[data-test="topbar-resource-measure"]',
    ]) {
      expect(wrapper.find(selector).exists(), `已删除的形态不应回归: ${selector}`).toBe(false)
    }
    // 卷徽章的 DOM 锚点（挂载点）也已不存在
    expect(wrapper.find('[data-mount]').exists()).toBe(false)
  })

  it('删除的形态在源码 / 样式层也清理干净（不留死规则与死导出引用）', () => {
    for (const token of [
      'topbar-disk-badge',
      'resNtp',
      'topbar-overflow',
      'topbar-resource-gauge',
      'topbar-resource-measure',
      'topbarOverflow',
      'visibleItemCount',
      'clockBadge',
      'useBreakpoint',
    ]) {
      expect(TOPBAR_SOURCE, `Topbar.vue 不应再引用 ${token}`).not.toContain(token)
    }
    for (const selector of [
      '.topbar__resource--disk',
      '.topbar__resource-measure',
      '.topbar__overflow',
      '.topbar__overflow-trigger',
      '.topbar__gauge',
      '.topbar__resources--gauge',
      '.topbar__ntp',
      '.topbar__datetime',
    ]) {
      expect(cssDecls(selector, null).length, `layout.css 不应残留 ${selector} 规则`).toBe(0)
    }
    // `.res-dot`（卷徽章的状态点）也不再有任何规则
    expect(LAYOUT_CSS_SOURCE).not.toContain('.res-dot')
  })

  it('任何宽度都常驻：390 / 768 / 1024 / 1440 下 CPU / 内存都在，且没有任何断点隐藏它们', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    // 组件不读视口宽度（无 JS 行为分支）：stub 只是把「不同视口」表达出来，DOM 恒同。
    vi.stubGlobal('matchMedia', (query: string) => ({
      media: query,
      matches: false,
      addEventListener: () => {},
      removeEventListener: () => {},
    }))
    for (const width of [390, 768, 1024, 1440]) {
      const wrapper = await mountTopbar()
      expect(cpuBadge(wrapper).exists(), `${width} 宽下 CPU 徽章必须常驻`).toBe(true)
      expect(memBadge(wrapper).exists(), `${width} 宽下 MEM 徽章必须常驻`).toBe(true)
      unmountManually(wrapper)
    }
    // 样式表侧：`.topbar__resource--cpu/--mem` 不得被任何断点规则隐藏（否则上面 DOM 断言
    // 只是「渲染了但看不见」，390 验收到不了）。
    expect(isHiddenAnywhere('.topbar__resource--cpu')).toBe(false)
    expect(isHiddenAnywhere('.topbar__resource--mem')).toBe(false)
  })

  it('快照尚未到达：CPU / 内存显示等待态 `—`，不得显示假数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(cpuBadge(wrapper).find('.res-value').text()).toBe('—')
    expect(memBadge(wrapper).find('.res-value').text()).toBe('—')
    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    for (const fake of ['+12ms', '76%', '93%', '45%']) {
      expect(wrapper.text()).not.toContain(fake)
    }
  })

  it('CPU 徽章绑定实时快照百分比（整数百分比是顶部窄条的刻意取舍）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.234 }))
    const wrapper = await mountTopbar()

    expect(cpuBadge(wrapper).find('.res-value').text()).toBe('23%')
    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--normal')
    expect(cpuBadge(wrapper).attributes('title')).toBe('CPU 使用率 23%')
  })

  it('CPU 徽章按 70% / 90% 阈值映射 warn / danger（顶栏保留的资源告警）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.75 }))
    const wrapper = await mountTopbar()

    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--warn')
    expect(cpuBadge(wrapper).attributes('data-level')).toBe('warn')
    expect(cpuBadge(wrapper).attributes('title')).toBe('CPU 使用率 75%')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.95 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--danger')
  })

  it('CPU 不可用（可用性映射 false）时显示「不可用」+ 原因', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.total': { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountTopbar()

    expect(cpuBadge(wrapper).find('.res-value').text()).toBe('不可用')
    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    expect(cpuBadge(wrapper).attributes('title')).toContain('collect_failed')
  })

  it('内存徽章绑定实时快照使用率，按 95% 阈值映射 danger', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(memBadge(wrapper).find('.res-value').text()).toBe('50%')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--normal')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * 1024 ** 3,
        available_bytes: 5 * 1024 ** 3,
        used_bytes: 95 * 1024 ** 3,
        used_ratio: 0.95,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    await wrapper.vm.$nextTick()
    expect(memBadge(wrapper).find('.res-value').text()).toBe('95%')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--danger')
    expect(memBadge(wrapper).attributes('title')).toBe('内存使用率 95%')
  })

  it('内存不可用（可用性映射 false / payload null）时显示「不可用」，不得退回 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountTopbar()
    expect(memBadge(wrapper).find('.res-value').text()).toBe('不可用')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    expect(memBadge(wrapper).attributes('title')).toContain('collect_failed')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ memory: null }))
    await wrapper.vm.$nextTick()
    expect(memBadge(wrapper).find('.res-value').text()).toBe('不可用')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
  })

  it('WS 推送更新时顶栏随之刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(cpuBadge(wrapper).find('.res-value').text()).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.5 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadge(wrapper).find('.res-value').text()).toBe('50%')
    expect(memBadge(wrapper).find('.res-value').text()).toBe('50%')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 墙钟（#452：本机墙钟 + 偏差括号）', () => {
  beforeEach(() => {
    // 只伪造 Date / setInterval：flushPromises 用的 setTimeout 保持真实，mount 才不会挂起
    vi.useFakeTimers({ toFake: ['Date', 'setInterval', 'clearInterval'] })
    vi.setSystemTime(FIXED_NOW)
  })

  it('四档文本片段全部渲染在 <time> 里（可见性由 CSS 断点决定，秒永远保留）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    const clock = clockEl(wrapper)
    expect(clock.exists()).toBe(true)
    expect(clock.element.tagName).toBe('TIME')
    expect(clock.attributes('datetime')).toBe('2026-01-01T13:25:36')
    expect(wrapper.find('[data-test="topbar-clock-date"]').text()).toBe('2026-01-01')
    expect(wrapper.find('[data-test="topbar-clock-date-short"]').text()).toBe('01-01')
    expect(wrapper.find('[data-test="topbar-clock-weekday"]').text()).toBe('周四')
    expect(clockTimeText(wrapper)).toBe('13:25:36')
    expect(clockOffset(wrapper).text()).toBe('(+13ms)')
  })

  it('四档格式的 CSS：<768 只留时间；768+ 周几；1024+ 月日；1280+ 完整日期（秒不被任何断点隐藏）', () => {
    // 默认（base，<768）：日期 / 短日期 / 周几都藏起来，时间常驻
    for (const selector of ['.topbar-clock__date', '.topbar-clock__date-short', '.topbar-clock__weekday']) {
      expect(cssDecls(selector, null).join('\n'), `${selector} 基础规则应为 display: none`).toContain('display: none')
    }
    expect(isHiddenAnywhere('.topbar-clock__time')).toBe(false)
    expect(isHiddenAnywhere('.topbar-clock__offset')).toBe(false)

    expect(cssDecls('.topbar-clock__weekday', '@media (min-width: 768px)').join('\n')).toContain('display: inline')
    expect(cssDecls('.topbar-clock__date-short', '@media (min-width: 768px)')).toEqual([])
    expect(cssDecls('.topbar-clock__date-short', '@media (min-width: 1024px)').join('\n')).toContain('display: inline')
    expect(cssDecls('.topbar-clock__date', '@media (min-width: 1024px)')).toEqual([])
    expect(cssDecls('.topbar-clock__date', '@media (min-width: 1280px)').join('\n')).toContain('display: inline')
    expect(cssDecls('.topbar-clock__date-short', '@media (min-width: 1280px)').join('\n')).toContain('display: none')

    // 四档只用了既有断点 768 / 1024 / 1280（没有新野断点，也不含容器查询）
    expect(LAYOUT_CSS_SOURCE).not.toContain('@container')
    for (const breakpoint of [768, 1024, 1280]) {
      expect(LAYOUT_CSS_SOURCE).toContain(`@media (min-width: ${breakpoint}px)`)
    }
  })

  it('偏差 normal：括号中性（不套 warn / danger 类），整块时钟保持中性', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    expect(clockOffset(wrapper).classes()).not.toContain('topbar-clock__offset--warn')
    expect(clockOffset(wrapper).classes()).not.toContain('topbar-clock__offset--danger')
    expect(clockEl(wrapper).classes()).not.toContain('topbar__resource--warn')
    expect(clockEl(wrapper).classes()).not.toContain('topbar__resource--danger')
  })

  it('偏差 warn（恰好 100ms）：只有括号内文本变黄，时钟其余部分不变色', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 100 },
    }))
    const wrapper = await mountTopbar()

    expect(clockOffset(wrapper).text()).toBe('(+100ms)')
    expect(clockOffset(wrapper).classes()).toContain('topbar-clock__offset--warn')
    expect(clockOffset(wrapper).classes()).not.toContain('topbar-clock__offset--danger')
    // 「只有括号内文本」：时钟容器 / 时间片段都没有状态类
    expect(clockEl(wrapper).classes()).not.toContain('topbar-clock__offset--warn')
    expect(wrapper.find('[data-test="topbar-clock-time"]').classes()).not.toContain('topbar-clock__offset--warn')
  })

  it('偏差 danger（恰好 1000ms）：只有括号内文本变红', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: -1000 },
    }))
    const wrapper = await mountTopbar()

    expect(clockOffset(wrapper).text()).toBe('(-1000ms)')
    expect(clockOffset(wrapper).classes()).toContain('topbar-clock__offset--danger')
    expect(clockEl(wrapper).classes()).not.toContain('topbar-clock__offset--danger')
  })

  it('阈值取生效配置（不是写死的 100 / 1000）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()
    // 配置镜像未到达时按契约默认值 100 / 1000：12.5ms 落在 normal
    expect(clockOffset(wrapper).classes()).not.toContain('topbar-clock__offset--warn')

    useSysmonStore().applyConfig(makeSysmonConfig({ clock: { offset_warn_ms: 10 } }))
    await wrapper.vm.$nextTick()
    expect(clockOffset(wrapper).classes()).toContain('topbar-clock__offset--warn')
  })

  it('尚未探测 / 本轮全部失败：括号显示 `(—)`（中性），不得用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_UNPROBED }))
    const wrapper = await mountTopbar()
    expect(clockOffset(wrapper).text()).toBe('(—)')
    expect(clockOffset(wrapper).classes()).not.toContain('topbar-clock__offset--warn')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ clock: SYSMON_CLOCK_ALL_FAILED }))
    await wrapper.vm.$nextTick()
    expect(clockOffset(wrapper).text()).toBe('(—)')
    expect(clockEl(wrapper).text()).not.toContain('0ms')
  })

  it('配置关闭（clock = null）：括号不渲染，title 说明监控已关闭', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountTopbar()

    expect(clockOffset(wrapper).exists()).toBe(false)
    expect(clockEl(wrapper).attributes('title')).toContain('关闭')
    expect(clockEl(wrapper).attributes('aria-label')).toContain('关闭')
  })

  it('快照未到达：括号显示 `(—)` 等待态（此时还不知道配置是否关闭）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(clockOffset(wrapper).text()).toBe('(—)')
    expect(clockEl(wrapper).attributes('title')).toContain('等待')
  })

  it('tooltip 含时区 / 偏差（1 位小数带符号）/ 误差界 / 来源 / stratum / 上次探测相对时间', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    const title = clockEl(wrapper).attributes('title') ?? ''
    expect(title).toContain('时区')
    expect(title).toContain('偏差 +12.5 ms')
    expect(title).toContain('误差界 18.3 ms')
    expect(title).toContain('来源 ntp.aliyun.com')
    expect(title).toContain('stratum 2')
    expect(title).toContain('上次探测 30 秒前')
    // 读屏可读：完整日期星期时间 + 偏差状态
    const aria = clockEl(wrapper).attributes('aria-label') ?? ''
    expect(aria).toContain('2026-01-01 周四 13:25:36')
    expect(aria).toContain('时钟偏差')
    expect(aria).toContain('正常')
  })

  it('偏差不可用时 title 给出原因（失败原因汇总）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_ALL_FAILED }))
    const wrapper = await mountTopbar()

    const title = clockEl(wrapper).attributes('title') ?? ''
    expect(title).toContain('超时未响应')
    expect(title).toContain('上次探测 30 秒前')
  })

  it('每秒 tick：时钟文本随本机时刻前进（fake timers）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()
    expect(clockTimeText(wrapper)).toBe('13:25:36')

    await vi.advanceTimersByTimeAsync(1_000)
    await wrapper.vm.$nextTick()
    expect(clockTimeText(wrapper)).toBe('13:25:37')

    await vi.advanceTimersByTimeAsync(1_000)
    await wrapper.vm.$nextTick()
    expect(clockTimeText(wrapper)).toBe('13:25:38')
  })

  it('页面隐藏时暂停（不回放秒），恢复后立即对齐当前时刻再继续', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    let hidden = false
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden })
    const wrapper = await mountTopbar()
    expect(vi.getTimerCount()).toBe(1)

    hidden = true
    document.dispatchEvent(new Event('visibilitychange'))
    expect(vi.getTimerCount()).toBe(0)
    // 隐藏 5 秒：定时器已停，时钟停在隐藏时刻（不补播隐藏期间的秒）
    await vi.advanceTimersByTimeAsync(5_000)
    await wrapper.vm.$nextTick()
    expect(clockTimeText(wrapper)).toBe('13:25:36')

    // 恢复：先对齐当前时刻（13:25:41），再重启每秒 tick——不是从 13:25:36 逐秒追
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    await wrapper.vm.$nextTick()
    expect(clockTimeText(wrapper)).toBe('13:25:41')
    expect(vi.getTimerCount()).toBe(1)

    await vi.advanceTimersByTimeAsync(1_000)
    await wrapper.vm.$nextTick()
    expect(clockTimeText(wrapper)).toBe('13:25:42')
  })

  it('卸载时清 interval 与 visibility 监听（不留定时器）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()
    expect(vi.getTimerCount()).toBe(1)
    const removeSpy = vi.spyOn(document, 'removeEventListener')

    unmountManually(wrapper)
    expect(vi.getTimerCount()).toBe(0)
    expect(removeSpy).toHaveBeenCalledWith('visibilitychange', expect.any(Function))
    removeSpy.mockRestore()
  })
})

describe('Topbar 服务状态徽章（#199：聚合 process_status 镜像，永不折叠）', () => {
  beforeEach(() => {
    // 资源快照与 #199 无关：固定给成功响应，避免 REST 失败噪声
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
  })

  const SERVICE_ANCHORS = ['resMd', 'resTd', 'resStg'] as const
  type ServiceAnchor = typeof SERVICE_ANCHORS[number]

  function serviceBadge(wrapper: VueWrapper, id: ServiceAnchor) {
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
    // 未知不是色标等级：不写 data-level（与资源徽章的「不可用」一致）
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

  it('服务徽章只有一份（没有折叠测量行 / 浮层里的副本），且 MD / TD / STG 三个锚点都在', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    for (const id of SERVICE_ANCHORS) {
      expect(wrapper.findAll(`#${id}`)).toHaveLength(1)
    }
    expect(wrapper.findAll('.topbar__resource--svc')).toHaveLength(3)
  })
})

// #380：壳层顶栏是全站唯一的 banner 地标。无名的 banner 让读屏的「跳到 banner」
// 变成一个不知道通往哪里的命令（audit 实测改动前两个 banner 的 name 都是空）。
describe('Topbar banner 地标（#380）', () => {
  it('根元素是 <header> 且带 aria-label（banner 有名）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    const root = wrapper.element as unknown as HTMLElement
    expect(root.tagName).toBe('HEADER')
    expect(root.getAttribute('aria-label')).toBe('全局状态栏')
  })

  it('头栏里没有 h1/h2：名字只能来自 aria-label，不得退化成用页面标题当地标名', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    expect(wrapper.findAll('h1, h2, h3, h4, h5, h6')).toHaveLength(0)
  })
})
