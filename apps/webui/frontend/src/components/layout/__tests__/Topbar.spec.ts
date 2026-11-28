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

// #452 / ADR 0054 顶栏重做（最终形态）：资源区三枚「图标 + 数值」常驻徽章 —— CPU → 内存 →
// 时钟偏差（均无文字标签），右上只有连接状态 + 主题；没有卷徽章、没有独立文字 `NTP` 徽章、
// 没有墙钟/日期时间、没有自适应折叠 / `▾ N` 浮层 / `<lg` 单状态图标。
// 本 spec 锁三组事实：
//   1. 三枚徽章的图标 / 数值 / 等级色 / title / aria-label（图标不自承担语义）；
//   2. 已删除形态没有回归（DOM 与 layout.css / 源码两侧都断言）；
//   3. 宽窄行为一致（纯 CSS，无媒体规则隐藏徽章；窄屏只收紧间距）——390 的真实像素验收
//      另有浏览器实测（jsdom 无布局，这里只能锁样式不变量）。
// 四档墙钟 / 每秒 tick / visibility 用例已随墙钟删除（#452 最终拍板：顶栏不显示日期时间）。

const LAYOUT_CSS = join(
  dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'assets', 'css', 'layout.css',
)
const LAYOUT_CSS_SOURCE = readFileSync(LAYOUT_CSS, 'utf8')
const TOPBAR_SOURCE = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'Topbar.vue'), 'utf8')

const stubs = {
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

/** 已挂载的 wrapper：用例结束统一卸载（全局顶栏有 WS store 订阅，避免跨用例泄漏） */
const mountedWrappers: VueWrapper[] = []

async function mountTopbar(): Promise<VueWrapper> {
  const wrapper = mount(Topbar, { global: { stubs } })
  mountedWrappers.push(wrapper)
  await flushPromises()
  return wrapper
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
})

afterEach(() => {
  for (const wrapper of mountedWrappers.splice(0)) wrapper.unmount()
  vi.unstubAllGlobals()
})

// ===== 选择器助手 =====

function cpuBadge(wrapper: VueWrapper) {
  return wrapper.find('#resCpu')
}

function memBadge(wrapper: VueWrapper) {
  return wrapper.find('#resMem')
}

function ntpBadge(wrapper: VueWrapper) {
  return wrapper.find('#resNtp')
}

function valueOf(wrapper: VueWrapper, id: 'resCpu' | 'resMem' | 'resNtp'): string {
  return wrapper.find(`#${id} .res-value`).text()
}

/** 徽章图标的内联 `--icon-url`（Icon 组件按 name 拼 `/icons/builtin/<name>.svg`） */
function iconUrlOf(wrapper: VueWrapper, id: 'resCpu' | 'resMem' | 'resNtp'): string {
  return wrapper.find(`#${id} [data-icon]`).attributes('style') ?? ''
}

/** 资源区直接子元素（顺序 = MD/TD/STG → 分隔线 → CPU/MEM/时钟偏差） */
function resourceChildren(wrapper: VueWrapper) {
  return wrapper.findAll('#topbarResources > *')
}

// ===== layout.css 断言助手（响应式不可见性只能从样式表断言）=====

const CSS_ROOT = postcss.parse(LAYOUT_CSS_SOURCE)

/** 选择器在**任何**位置被 `display: none` 隐藏过吗（基础规则或媒体块） */
/**
 * 任意断点是否用**片段**隐藏了某类 DOM（含后代选择器 / 媒体块）。
 *
 * 不能只匹配「选择器完全等于类名」：旧实现删掉的正是 `.topbar__resources--gauge
 * .topbar__resource--cpu { display: none }` 这类后代选择器，精确匹配会漏掉同类回归。
 * 除 display 外也认 visibility / content-visibility / width:0 这些隐藏形态。
 */
function isHiddenAnywhere(selectorFragment: string): boolean {
  let hidden = false
  CSS_ROOT.walkRules((rule) => {
    if (!rule.selectors.some(part => part.includes(selectorFragment))) return
    rule.walkDecls((decl) => {
      const prop = decl.prop.trim()
      const value = decl.value.trim()
      if (prop === 'display' && value === 'none') hidden = true
      if (prop === 'visibility' && value === 'hidden') hidden = true
      if (prop === 'content-visibility' && value === 'hidden') hidden = true
      if ((prop === 'width' || prop === 'max-width') && value === '0') hidden = true
    })
  })
  return hidden
}

/** 某选择器在基础（非媒体）位置的规则体（拼接所有命中规则） */
function ruleBody(selector: string): string {
  let body = ''
  CSS_ROOT.walkRules((rule) => {
    if (rule.selectors.some(part => part.trim() === selector)) body += `${rule.toString()}\n`
  })
  return body
}

/** 某选择器在指定媒体块内的规则体（同一档位可能有多个块，取包含该选择器的那个） */
function ruleInMedia(mediaParams: string, selector: string): string {
  let found = ''
  CSS_ROOT.walkAtRules('media', (atRule) => {
    if (found !== '' || atRule.params !== mediaParams) return
    let body = ''
    atRule.walkRules((rule) => {
      if (rule.selectors.some(part => part.trim() === selector)) body += `${rule.toString()}\n`
    })
    found = body
  })
  return found
}

describe('Topbar 资源区三枚图标徽章（#452 / ADR 0054）', () => {
  beforeEach(() => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
  })

  it('只渲染 CPU / 内存 / 时钟偏差三枚指标徽章，顺序在服务徽章与分隔线之后', async () => {
    const wrapper = await mountTopbar()

    const children = resourceChildren(wrapper)
    // 3 枚服务徽章 + 1 条分隔线 + 3 枚指标徽章
    expect(children).toHaveLength(7)
    expect(children.slice(0, 3).map(child => child.attributes('id'))).toEqual(['resMd', 'resTd', 'resStg'])
    expect(children[3].classes()).toContain('topbar__resource-divider')
    expect(children.slice(4).map(child => child.attributes('id'))).toEqual(['resCpu', 'resMem', 'resNtp'])
    for (const badge of children.slice(4)) {
      expect(badge.classes()).toContain('topbar__metric')
    }
  })

  it('三枚徽章都是「12px 图标 + 数值」：图标名正确且与内置资源同名，图标 aria-hidden', async () => {
    const wrapper = await mountTopbar()

    expect(iconUrlOf(wrapper, 'resCpu')).toContain('/icons/builtin/CPU.svg')
    expect(iconUrlOf(wrapper, 'resMem')).toContain('/icons/builtin/memory-module.svg')
    expect(iconUrlOf(wrapper, 'resNtp')).toContain('/icons/builtin/clock_time.svg')

    for (const id of ['resCpu', 'resMem', 'resNtp'] as const) {
      const icon = wrapper.find(`#${id} [data-icon]`)
      // 图标单独进可访问性树会让读屏只听到「图片」；语义全部由徽章的 aria-label 承担
      expect(icon.attributes('aria-hidden')).toBe('true')
      expect(icon.attributes('style')).toContain('width: 12px')
      // 没有文字标签：徽章里只有图标 + 数值两个元素
      expect(wrapper.find(`#${id}`).findAll('.res-label')).toHaveLength(0)
    }
  })

  it('数值带单位 + tabular-nums 由 layout.css 保证（固定宽度不随推送跳位）', () => {
    const valueRule = CSS_ROOT.toString().match(/\.topbar__metric \.res-value \{([^}]*)\}/)?.[1] ?? ''
    expect(valueRule).toContain('font-variant-numeric: tabular-nums')
    expect(valueRule).toContain('font-size: var(--font-size-10)')
  })

  it('CPU 徽章绑定实时快照百分比；读屏名与 title 都是完整名称 + 读数', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.234 }))
    const wrapper = await mountTopbar()

    expect(valueOf(wrapper, 'resCpu')).toBe('23%')
    expect(cpuBadge(wrapper).classes()).toContain('topbar__resource--normal')
    expect(cpuBadge(wrapper).attributes('data-level')).toBe('normal')
    expect(cpuBadge(wrapper).attributes('role')).toBe('img')
    expect(cpuBadge(wrapper).attributes('aria-label')).toBe('CPU 使用率 23%')
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

  it('内存徽章绑定实时快照使用率，按 95% 阈值映射 danger', async () => {
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

    expect(valueOf(wrapper, 'resMem')).toBe('95%')
    expect(memBadge(wrapper).classes()).toContain('topbar__resource--danger')
    expect(memBadge(wrapper).attributes('aria-label')).toBe('内存使用率 95%')
    expect(memBadge(wrapper).attributes('title')).toBe('内存使用率 95%')
  })

  it('CPU / 内存不可用：数值显示「不可用」+ 中性色，title / aria 给原因（不得用 0 冒充）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: {
        'cpu.total': { available: false, reason: 'collect_failed' },
        memory: { available: false, reason: 'collect_failed' },
      },
    }))
    const wrapper = await mountTopbar()

    for (const [id, name] of [['resCpu', 'CPU'], ['resMem', '内存']] as const) {
      expect(valueOf(wrapper, id)).toBe('不可用')
      expect(wrapper.find(`#${id}`).classes()).toContain('topbar__resource--unavailable')
      expect(wrapper.find(`#${id}`).attributes('aria-label')).toContain('不可用')
      expect(wrapper.find(`#${id}`).attributes('title')).toContain('collect_failed')
      expect(wrapper.find(`#${id}`).attributes('title')).toContain(name)
    }
  })

  it('CPU 首采 ratio=null / 内存 payload 非法：显示「不可用」而不是 0%', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      memory: null,
    }))
    const wrapper = await mountTopbar()

    for (const id of ['resCpu', 'resMem'] as const) {
      expect(valueOf(wrapper, id)).toBe('不可用')
      expect(wrapper.find(`#${id}`).classes()).toContain('topbar__resource--unavailable')
    }
    expect(wrapper.text()).not.toContain('0%')
  })

  it('快照尚未到达：CPU / 内存显示等待态 `—`（不得显示假数字），时钟偏差显示 `—`', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(valueOf(wrapper, 'resCpu')).toBe('—')
    expect(valueOf(wrapper, 'resMem')).toBe('—')
    expect(valueOf(wrapper, 'resNtp')).toBe('—')
    expect(cpuBadge(wrapper).attributes('aria-label')).toContain('等待快照')
    expect(ntpBadge(wrapper).attributes('aria-label')).toContain('等待快照')
    for (const fake of ['+12ms', '76%', '93%', '45%']) {
      expect(wrapper.text()).not.toContain(fake)
    }
  })

  it('WS 推送更新时三枚徽章随之刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(valueOf(wrapper, 'resCpu')).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      cpuUsageRatio: 0.5,
      clock: SYSMON_CLOCK_LIVE,
    }))
    await wrapper.vm.$nextTick()
    expect(valueOf(wrapper, 'resCpu')).toBe('50%')
    expect(valueOf(wrapper, 'resMem')).toBe('50%')
    expect(valueOf(wrapper, 'resNtp')).toBe('+13ms')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 时钟偏差徽章（NTP 图标 + 毫秒数值）', () => {
  it('正常态：紧凑带符号毫秒 + normal；title 含误差界 / 来源 / stratum / 上次探测', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()

    expect(valueOf(wrapper, 'resNtp')).toBe('+13ms')
    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--normal')
    expect(ntpBadge(wrapper).attributes('aria-label')).toBe('时钟偏差 +13ms，正常')
    const title = ntpBadge(wrapper).attributes('title') ?? ''
    expect(title).toContain('时钟偏差 +12.5 ms')
    expect(title).toContain('误差界 18.3 ms')
    expect(title).toContain('来源 ntp.aliyun.com（stratum 2）')
    expect(title).toContain('上次探测 30 秒前')
  })

  it('偏差按配置阈值定档：恰好 100ms → warn、恰好 1000ms → danger（只有数值文本变黄 / 变红）', async () => {
    const cases = [
      { offsetMs: 99, expected: 'topbar__resource--normal' },
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
      if (offsetMs < 0) expect(valueOf(wrapper, 'resNtp')).toBe('-350ms')
      wrapper.unmount()
      const index = mountedWrappers.indexOf(wrapper)
      if (index >= 0) mountedWrappers.splice(index, 1)
    }
  })

  it('阈值取生效配置（不是写死的 100 / 1000）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountTopbar()
    // 配置镜像未到达时按契约默认值 100 / 1000：12.5ms 落在 normal
    expect(ntpBadge(wrapper).classes()).not.toContain('topbar__resource--warn')

    // 收紧到 warn=10 后同一读数变 warn（配置镜像到达后以生效阈值为准）
    useSysmonStore().applyConfig(makeSysmonConfig({ clock: { offset_warn_ms: 10 } }))
    await wrapper.vm.$nextTick()
    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--warn')
  })

  it('误差界越界只进 title 的「测量质量」提示，不改变偏差着色（契约 §2.8）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 20, bound_ms: 400 },
    }))
    const wrapper = await mountTopbar()

    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--normal')
    expect(ntpBadge(wrapper).attributes('title')).toContain('测量质量')
  })

  it('尚未探测 / 本轮全部失败：数值 `—`（中性），不得用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_UNPROBED }))
    const wrapper = await mountTopbar()
    expect(valueOf(wrapper, 'resNtp')).toBe('—')
    expect(ntpBadge(wrapper).classes()).toContain('topbar__resource--unavailable')
    expect(ntpBadge(wrapper).attributes('aria-label')).toBe('时钟偏差尚未探测')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ clock: SYSMON_CLOCK_ALL_FAILED }))
    await wrapper.vm.$nextTick()
    expect(valueOf(wrapper, 'resNtp')).toBe('—')
    expect(ntpBadge(wrapper).attributes('aria-label')).toBe('时钟偏差不可用')
    expect(ntpBadge(wrapper).attributes('title')).toContain('超时未响应')
    expect(ntpBadge(wrapper).attributes('title')).toContain('上次探测 30 秒前')
  })

  it('配置关闭（clock = null）：整枚徽章不渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountTopbar()

    expect(ntpBadge(wrapper).exists()).toBe(false)
    // 另外两枚不受影响
    expect(cpuBadge(wrapper).exists()).toBe(true)
    expect(memBadge(wrapper).exists()).toBe(true)
  })
})

describe('Topbar 已删除形态不回归（墙钟 / 日期 / 文字 NTP / 卷 / 折叠）', () => {
  beforeEach(() => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
  })

  it('没有墙钟 / 日期时间 / 每秒定时器：DOM 里没有 <time>、没有时钟组件', async () => {
    const wrapper = await mountTopbar()
    expect(wrapper.findAll('time')).toHaveLength(0)
    expect(wrapper.find('[data-test="topbar-clock"]').exists()).toBe(false)
    // 源码层同样不得再引用墙钟实现（否则「又加回来了」不会体现在 DOM 断言里）
    for (const token of ['TopbarClock', 'topbarClock', 'setInterval', 'visibilitychange']) {
      expect(TOPBAR_SOURCE, `Topbar.vue 不应再引用 ${token}`).not.toContain(token)
    }
  })

  it('没有卷徽章 / 文字 NTP 徽章 / overflow / gauge / 测量行', async () => {
    const wrapper = await mountTopbar()
    for (const selector of [
      '[data-test="topbar-disk-badge"]',
      '[data-test="topbar-overflow-trigger"]',
      '[data-test="topbar-overflow-panel"]',
      '[data-test="topbar-resource-gauge"]',
      '[data-test="topbar-resource-measure"]',
    ]) {
      expect(wrapper.find(selector).exists(), `已删除的形态不应回归: ${selector}`).toBe(false)
    }
    expect(wrapper.find('[data-mount]').exists()).toBe(false)
    // 资源区没有 `.res-label`（服务徽章的标签除外：它们不是资源指标徽章）
    expect(wrapper.find('.topbar__metric .res-label').exists()).toBe(false)
    for (const token of ['topbar-disk-badge', 'topbar-overflow', 'topbar-resource-gauge', 'topbar-resource-measure']) {
      expect(TOPBAR_SOURCE, `Topbar.vue 不应再引用 ${token}`).not.toContain(token)
    }
  })

  it('layout.css 里墙钟 / 折叠 / 浮层 / 卷状态点规则全部清理干净', () => {
    expect(LAYOUT_CSS_SOURCE).not.toContain('topbar-clock')
    expect(LAYOUT_CSS_SOURCE).not.toContain('@keyframes ntp-warn')
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
      // 选择器可能出现在注释里（说明已删除），这里只查是否还有真实规则
      let hasRule = false
      CSS_ROOT.walkRules((rule) => {
        if (rule.selectors.some(part => part.trim() === selector)) hasRule = true
      })
      expect(hasRule, `layout.css 不应残留 ${selector} 规则`).toBe(false)
    }
    expect(LAYOUT_CSS_SOURCE).not.toContain('.res-dot')
  })
})

describe('Topbar 宽窄行为（纯 CSS：三枚徽章任何宽度都常驻，不溢出）', () => {
  it('没有任何媒体规则隐藏三枚指标徽章或服务徽章（390 也可见）', () => {
    for (const fragment of ['topbar__metric', 'topbar__resource--svc']) {
      expect(isHiddenAnywhere(fragment), `${fragment} 不得被任何断点隐藏`).toBe(false)
    }
    // 不引入容器查询（会造成同一份信息按容器宽度出现 / 消失）
    expect(LAYOUT_CSS_SOURCE).not.toContain('@container')
  })

  it('<768 三枚指标徽章改为竖排（图标在上、数值在下），进一步省横向宽度', () => {
    const block = ruleInMedia('(max-width: 767.98px)', '.topbar__metric')
    expect(block).toContain('flex-direction: column')
    expect(block).toContain('align-items: center')
    // 宽屏仍是横排（图标 + 数值同一行）
    expect(ruleBody('.topbar__metric')).toContain('display: inline-flex')
    expect(ruleBody('.topbar__metric')).toContain('align-items: center')
  })

  it('窄屏只收紧间距 / 内边距：<768 收 topbar padding、<640 再收资源区 gap', () => {
    expect(ruleInMedia('(max-width: 767.98px)', '.topbar')).toContain('padding: 0 var(--spacer-10)')
    expect(ruleInMedia('(max-width: 639.98px)', '.topbar__resources')).toContain('gap: var(--spacer-6)')
    // 徽章自身不缩、不换行、不截断（否则窄屏会先丢 MEM / 时钟）
    const baseRule = CSS_ROOT.nodes.find(
      node => node.type === 'rule' && node.selectors.some(part => part.trim() === '.topbar__metric'),
    )
    // postcss 节点类型收窄：直接取规则文本断言，避免把媒体块里的同名节点当成基础规则
    const baseText = baseRule?.toString() ?? ''
    expect(baseText).toContain('flex-shrink: 0')
    expect(baseText).toContain('white-space: nowrap')
  })

  it('资源区自身允许收缩（overflow:hidden 兜底），不把顶栏撑出横向滚动', () => {
    const resources = CSS_ROOT.nodes.find(
      node => node.type === 'rule' && node.selectors.some(part => part.trim() === '.topbar__resources'),
    )
    const text = resources?.toString() ?? ''
    expect(text).toContain('min-width: 0')
    expect(text).toContain('overflow: hidden')
    expect(text).toContain('flex: 1 1 auto')
  })

  it('图标永远中性色且不随告警闪烁；三枚指标 normal 数值用成功绿（状态只在数值上）', () => {
    const iconRule = CSS_ROOT.toString().match(/\.topbar__metric \.res-icon \{([^}]*)\}/)?.[1] ?? ''
    expect(iconRule).toContain('color: var(--text-secondary)')
    expect(iconRule).not.toContain('animation')
    const warnIconRule = CSS_ROOT.toString().match(
      /\.topbar__metric\.topbar__resource--warn \.res-icon,[\s\S]*?\{([^}]*)\}/,
    )?.[1] ?? ''
    expect(warnIconRule).toContain('animation: none')
    const ntpNormalRule = CSS_ROOT.toString().match(
      /\.topbar__metric--ntp\.topbar__resource--normal \.res-value \{([^}]*)\}/,
    )?.[1] ?? ''
    expect(ntpNormalRule).toContain('color: var(--status-success-on-surface)')
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

// #530 B1：消息中心抽屉内含 `position: fixed` 的 Modal 遮罩。topbar 是 sticky + z-index:90
// 且 @supports 下有 backdrop-filter（fixed 后代的包含块），内联渲染会把遮罩压成 32px 条并
// 困在 90 层（侧栏 300 会盖住）。这里锁定「抽屉被 Teleport 到 body、不在 topbar 内部」。
describe('Topbar 消息中心抽屉的浮层层级（#530 B1）', () => {
  it('打开抽屉后 modal-overlay 挂在 body 下，不被 topbar 的堆叠上下文/包含块困住', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    await wrapper.get('[data-test="topbar-notify"]').trigger('click')
    await flushPromises()

    const overlay = document.body.querySelector('.modal-overlay')
    expect(overlay).not.toBeNull()
    // 关键断言：遮罩的祖先链里不能有 .topbar（sticky + z-index:90 + backdrop-filter）
    expect(overlay!.closest('.topbar')).toBeNull()
    expect(overlay!.parentElement).toBe(document.body)
  })
})
