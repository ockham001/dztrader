import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Root } from 'postcss'
import { mount } from '@vue/test-utils'

// #380 地标与 skip link（壳层 a11y 结构）
//
// audit（#363 收尾）用 CDP `Accessibility.getFullAXTree` 在 8080 实测的基线：
//   main = 0、banner = 2（Topbar.vue 的 `.topbar` 与 MonitorView.vue 的标题组，都未命名）、
//   skip link = 无（首个 Tab 落在「DZTrader」品牌按钮上）。
// `/monitor` 的 68 个可聚焦项里前 15 个全是导航壳，键盘用户每次换页要 Tab 15 次才进正文。
//
// 本 spec 锁三件事，且每条都**对着改动前的实际结构**写过反证（见文件末尾 describe）：
//   1. 内容槽是 `<main>`，带 id（skip link 锚点）与 `tabindex="-1"`（焦点可落）；
//   2. skip link 是壳层第一个 Tab 站，href 指向那个 id，且**在 main 之外**——
//      否则读屏不把它当「跳到主内容」，而是当成正文里的一个链接；
//   3. banner 数量：壳层 header 是唯一「不是 main 后代」的 header，且它有名。
//
// 口径依据（HTML-AAM / ARIA in HTML 的 `banner` 角色映射）：`<header>` 只有当它
// **不是** `article` / `aside` / `main` / `nav` / `section` 的后代时，才映射为 `banner`。
// 所以「把页面 header 挪进 main」本身就消除了第二个 banner —— 不需要去改任何页面组件
// （MonitorView.vue 正被 #371 改，本票与它零交集）。

const LAYOUT_CSS = join(fileURLToPath(import.meta.url), '..', '..', '..', '..', 'assets', 'css', 'layout.css')
const CSS_SOURCE = readFileSync(LAYOUT_CSS, 'utf8')
let CSS_ROOT: Root | null = null

function cssRoot(): Root {
  if (!CSS_ROOT) CSS_ROOT = postcss.parse(CSS_SOURCE)
  return CSS_ROOT
}

/** 取某个选择器的规则体文本（多条同名规则拼接）；找不到返回 null */
function ruleBody(root: Root, selector: string): string | null {
  const bodies: string[] = []
  root.walkRules((rule) => {
    if (rule.selectors.some((s) => s.trim() === selector)) bodies.push(rule.toString())
  })
  return bodies.length > 0 ? bodies.join('\n') : null
}

/** 是否为「会被算作 banner」的 header：不在 article/aside/main/nav/section 之内 */
function isBannerHeader(header: Element): boolean {
  return header.closest('article, aside, main, nav, section') === null
}

// ---- 挂载（沿用 AppShell.spec.ts 的 matchMedia / wsClient 桩）----

let mockWidth = 1200

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({
    connectionState: { value: 'connected' },
    lastError: { value: null },
    connect: vi.fn(),
    disconnect: vi.fn(),
    mdConnect: vi.fn(),
    mdDisconnect: vi.fn(),
    subscribeLog: vi.fn(),
    unsubscribeLog: vi.fn(),
  }),
}))

beforeEach(() => {
  mockWidth = 1200
  vi.stubGlobal('matchMedia', (q: string) => ({
    media: q,
    get matches(): boolean {
      const m = q.match(/\(min-width:\s*([\d.]+)px\)/)
      return m ? mockWidth >= Number.parseFloat(m[1]) : false
    },
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  }))
})

afterEach(() => {
  vi.unstubAllGlobals()
})

const TopbarStub = {
  name: 'Topbar',
  emits: ['toggle-mobile'],
  // 真实的 banner 结构：壳层顶栏是 <header>。名字**刻意不给** —— 它属于 Topbar 自己，
  // 由 Topbar.spec.ts 对真实组件断言；这里给 stub 加名字只会让本文件「自己证明自己」。
  template: '<header class="topbar"><button type="button" class="topbar-toggle" /></header>',
}

/**
 * 页面级 header 的替身（模拟 MonitorView.vue:730 那种「页面内自带 <header>」）。
 * 不注册成 Vue stub：本 spec 只关心**渲染出来的 DOM 结构**，直接写进 slot HTML 更贴近实况。
 */
const PAGE_WITH_HEADER = '<div class="page-stub"><header class="page-header"><h1>资源监控</h1></header></div>'

async function mountShell(slot = '<div class="test-slot">content</div>') {
  const { default: Shell } = await import('../AppShell.vue')
  return mount(Shell, {
    global: { stubs: { Sidebar: true, Topbar: TopbarStub, teleport: true } },
    slots: { default: slot },
    attachTo: document.body,
  })
}

type Shell = Awaited<ReturnType<typeof mountShell>>

/** 组件根元素（`.app-shell` 的 DOM 节点）；test-utils 的 element 是 unknown，此处收口一次 */
function shellRoot(wrapper: Shell): HTMLElement {
  return wrapper.element as unknown as HTMLElement
}

function mainEl(wrapper: Shell): HTMLElement {
  const el = shellRoot(wrapper).querySelector('main')
  if (!el) throw new Error('壳层没有 <main>')
  return el
}

function skipLink(wrapper: Shell): HTMLAnchorElement {
  const el = shellRoot(wrapper).querySelector('a.ds-skip-link')
  if (!el) throw new Error('壳层没有 skip link')
  return el as HTMLAnchorElement
}

function headers(wrapper: Shell): HTMLElement[] {
  return Array.from(shellRoot(wrapper).querySelectorAll('header')) as HTMLElement[]
}

describe('#380 地标：<main>', () => {
  it('壳层恰好一个 <main>，包住 slot 内容', async () => {
    const wrapper = await mountShell()
    const main = mainEl(wrapper)
    expect(shellRoot(wrapper).querySelectorAll('main')).toHaveLength(1)
    expect(main.querySelector('.test-slot')).not.toBeNull()
    wrapper.unmount()
  })

  it('<main> 带 id 与 tabindex="-1"（skip link 能锚定、焦点能落进去）', async () => {
    const wrapper = await mountShell()
    const main = mainEl(wrapper)
    // 具体 id 由 skip link 的 href 决定（下一组用例断言两者一致），这里只锁「有 id」
    expect(main.id).not.toBe('')
    // 没有 tabindex=-1，浏览器按锚点跳转只滚动不聚焦，下一个 Tab 又回导航壳
    expect(main.getAttribute('tabindex')).toBe('-1')
    wrapper.unmount()
  })
})

describe('#380 地标：skip link', () => {
  it('skip link 是壳层里第一个可聚焦元素（Tab 第一站，先于侧栏/顶栏）', async () => {
    const wrapper = await mountShell()
    const focusables = shellRoot(wrapper).querySelectorAll(
      'a[href], button, input, select, textarea, [tabindex]:not([tabindex="-1"])',
    )
    expect(focusables.length).toBeGreaterThan(1) // 采集健全性：壳层里不止它一个可聚焦
    expect(focusables[0]).toBe(skipLink(wrapper))
    wrapper.unmount()
  })

  it('skip link 的 href 指向 <main> 的 id，文案是「跳到主内容」', async () => {
    const wrapper = await mountShell()
    const link = skipLink(wrapper)
    expect(link.textContent?.trim()).toBe('跳到主内容')
    expect(link.getAttribute('href')).toBe(`#${mainEl(wrapper).id}`)
    wrapper.unmount()
  })

  it('skip link 在 <main> 之外（不是正文里的普通链接）', async () => {
    const wrapper = await mountShell()
    expect(mainEl(wrapper).contains(skipLink(wrapper))).toBe(false)
    wrapper.unmount()
  })
})

describe('#380 banner：两个变一个（结构，非文本）', () => {
  it('页面自带 <header> 落在 main 内 → 不算 banner；壳层 header 是唯一 banner', async () => {
    const wrapper = await mountShell(PAGE_WITH_HEADER)

    const all = headers(wrapper)
    // 改动前：壳层 .topbar + 页面 .page-header = 2 个 header 都映射成 banner
    expect(all).toHaveLength(2)
    // 改动后：只有壳层那个不在 main 里 → banner 数量 1
    const banners = all.filter(isBannerHeader)
    expect(banners).toHaveLength(1)
    expect(banners[0].classList.contains('topbar')).toBe(true)
    wrapper.unmount()
  })
})

describe('#380 skip link 的 CSS（.ds-skip-link 只在 assets/css 定义）', () => {
  it('规则体含 position: absolute|fixed 与「藏起来」机制（transform 推出视口）', () => {
    const body = ruleBody(cssRoot(), '.ds-skip-link')
    expect(body, 'layout.css 缺少 .ds-skip-link 规则').not.toBeNull()
    expect(body).toMatch(/position:\s*(absolute|fixed)/)
    // 藏起来的机制：必须有 transform 位移且是负值（推出视口上沿），
    // 不能靠 display:none / visibility:hidden —— 那会让它退出 Tab 序列与可访问性树
    expect(body).toMatch(/transform:\s*translateY\(-\d+%\)/)
    expect(body).not.toMatch(/display:\s*none/)
    expect(body).not.toMatch(/visibility:\s*hidden/)
  })

  it(':focus 把位移归零（聚焦时真的出现在视口内）', () => {
    const body = ruleBody(cssRoot(), '.ds-skip-link:focus')
    expect(body, '缺少 .ds-skip-link:focus 规则').not.toBeNull()
    expect(body).toMatch(/transform:\s*(none|translateY\(0\))/)
  })

  it('token-only：布局/颜色/尺寸不写裸 px 与裸色（dz/token-literal 同口径）', () => {
    const body = ruleBody(cssRoot(), '.ds-skip-link') ?? ''
    const decls = body.replace(/\/\*[\s\S]*?\*\//g, '')
    // 颜色：不得出现 hex / rgb() / hsl() / 颜色关键词
    expect(decls).not.toMatch(/#[0-9a-f]{3,8}\b/i)
    expect(decls).not.toMatch(/\b(?:rgba?|hsla?)\(/i)
    // 间距/字号/圆角：这些属性必须走 token
    for (const prop of ['padding', 'font-size', 'border-radius', 'top', 'left']) {
      const m = decls.match(new RegExp(`${prop}:\\s*([^;]+);`))
      if (!m) continue
      expect(m[1], `${prop} 必须是 token`).not.toMatch(/\b\d+(?:\.\d+)?px\b/)
    }
  })
})

// ---- 反证：谓词必须能判红 ----
// 这些用例喂的是**改动前的真实结构**（main 不存在、skip link 在 main 内、页面 header 在 main 外），
// 证明上面的断言不是「写成什么都会绿」。没有这一段，本 spec 会退化成 #309 点名的恒真断言。
describe('#380 反证：结构退回改动前必须判红', () => {
  it('没有 <main> 时 mainEl 抛错（旧结构：body > #app > .app-shell > .workspace）', () => {
    const host = document.createElement('div')
    host.innerHTML = '<div class="app-shell"><div class="workspace app-shell__workspace"><slot></slot></div></div>'
    expect(host.querySelector('main')).toBeNull()
    expect(() => {
      const el = host.querySelector('main')
      if (!el) throw new Error('壳层没有 <main>')
      return el
    }).toThrow()
  })

  it('skip link 在 main 内时「不算正文链接」的断言判红', () => {
    const host = document.createElement('div')
    host.innerHTML = '<div class="workspace"><main id="main-content"><a href="#main-content" class="ds-skip-link">跳到主内容</a></main></div>'
    const main = host.querySelector('main')!
    const link = host.querySelector('a.ds-skip-link')!
    expect(main.contains(link)).toBe(true) // 与正确结构的断言相反 → 断言会红
  })

  it('页面 header 在 main 外时 banner 数量是 2（改动前的实测形态）', () => {
    const host = document.createElement('div')
    host.innerHTML = [
      '<div class="app-shell">',
      '  <div class="workspace">',
      '    <header class="topbar"></header>',
      '    <div class="monitor-view"><div><header class="monitor-view__header"></header></div></div>',
      '  </div>',
      '</div>',
    ].join('')
    const headers = Array.from(host.querySelectorAll('header'))
    expect(headers).toHaveLength(2)
    expect(headers.filter(isBannerHeader)).toHaveLength(2) // 两个都是 banner → 正是 #380 的缺陷
  })

  it('把页面 header 挪进 main 后 banner 数量降到 1（本票的修法本身）', () => {
    const host = document.createElement('div')
    host.innerHTML = [
      '<div class="app-shell">',
      '  <div class="workspace">',
      '    <header class="topbar"></header>',
      '    <main id="main-content"><header class="monitor-view__header"></header></main>',
      '  </div>',
      '</div>',
    ].join('')
    expect(Array.from(host.querySelectorAll('header')).filter(isBannerHeader)).toHaveLength(1)
  })
})
