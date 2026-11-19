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
// 本 spec 锁四件事，且每条都**对着改动前的实际结构**写过反证（见文件末尾 describe）：
//   1. 内容槽是 `<main>`，带 id（skip link 锚点）与 `tabindex="-1"`（焦点可落）；
//   2. skip link 是壳层第一个 Tab 站，href 指向那个 id，且**在 main 之外**——
//      否则读屏不把它当「跳到主内容」，而是当成正文里的一个链接；
//   3. banner 数量：壳层 header 是唯一「不是 main 后代」的 header（名字由 Topbar.spec.ts 锁）；
//   4. `.ds-skip-link` 的 CSS 不变量 + `.app-shell__main` 的布局不变量（jsdom 无布局，
//      删掉那 4 条声明会让全站内容被裁死，只有 CSS 文本断言能守）。
//
// 口径依据（HTML-AAM / ARIA in HTML 的 `banner` 角色映射）：`<header>` 只有当它
// **不是** `article` / `aside` / `main` / `nav` / `section` 的后代时，才映射为 `banner`。
// 所以「把页面 header 挪进 main」本身就消除了第二个 banner —— 不需要去改任何页面组件
// （MonitorView.vue 正被 #371 改，本票与它零交集）。

const LAYOUT_CSS = join(fileURLToPath(import.meta.url), '..', '..', '..', '..', 'assets', 'css', 'layout.css')
const SHELL_VUE = join(fileURLToPath(import.meta.url), '..', '..', 'AppShell.vue')
const CSS_SOURCE = readFileSync(LAYOUT_CSS, 'utf8')
const SHELL_SOURCE = readFileSync(SHELL_VUE, 'utf8')
let CSS_ROOT: Root | null = null
let SHELL_STYLE_ROOT: Root | null = null

function cssRoot(): Root {
  if (!CSS_ROOT) CSS_ROOT = postcss.parse(CSS_SOURCE)
  return CSS_ROOT
}

/** AppShell.vue 的 `<style scoped>` 段（`.app-shell__*` 布局类定义在这里） */
function shellStyleRoot(): Root {
  if (SHELL_STYLE_ROOT) return SHELL_STYLE_ROOT
  const start = SHELL_SOURCE.indexOf('<style scoped>')
  const end = SHELL_SOURCE.indexOf('</style>', start)
  expect(start, 'AppShell.vue 缺少 <style scoped>').toBeGreaterThan(-1)
  expect(end, 'AppShell.vue 的 <style scoped> 未闭合').toBeGreaterThan(start)
  SHELL_STYLE_ROOT = postcss.parse(SHELL_SOURCE.slice(start + '<style scoped>'.length, end))
  return SHELL_STYLE_ROOT
}

/** 取某个选择器的规则体文本（多条同名规则拼接）；找不到返回 null */
function ruleBody(root: Root, selector: string): string | null {
  const bodies: string[] = []
  root.walkRules((rule) => {
    if (rule.selectors.some((s) => s.trim() === selector)) bodies.push(rule.toString())
  })
  return bodies.length > 0 ? bodies.join('\n') : null
}

/** 去掉注释后的声明文本：断言只应看真实声明，不能被注释里的示例文字命中 */
function declarations(body: string | null): string {
  return (body ?? '').replace(/\/\*[\s\S]*?\*\//g, '')
}

/** 是否为「会被算作 banner」的 header：不在 article/aside/main/nav/section 之内 */
function isBannerHeader(header: Element): boolean {
  return header.closest('article, aside, main, nav, section') === null
}

/**
 * 生产谓词：在给定根节点下找正文地标，找不到就是结构错误。
 * 正向用例与反证用例**都走这一个函数** —— 反证才有意义（否则反证只是在验证
 * 「我自己手写的 throw 会 throw」，与被测代码无关；#309 点名的恒真断言形态）。
 */
function requireMain(root: ParentNode): HTMLElement {
  const el = root.querySelector('main')
  if (!el) throw new Error('壳层没有 <main>')
  return el as HTMLElement
}

/** 生产谓词：skip link 是否被包进了正文地标（被包进去就不算「跳到主内容」的独立入口） */
function skipLinkInsideMain(root: ParentNode): boolean {
  const main = root.querySelector('main')
  const link = root.querySelector('a.ds-skip-link')
  if (!main || !link) throw new Error('结构缺 main 或 skip link')
  return main.contains(link)
}

/**
 * 生产谓词：「藏起来」机制推出的百分比（`transform: translateY(-200%)` → 200）。
 * 返回 null 表示没有可识别的机制。小于 100 的值**不算藏起来** —— 位移不足自身高度时，
 * 元素仍有部分留在视口内，跳链会一直可见（#380 复核 F4：`-1%` / `-0%` 曾被旧正则放过）。
 */
function hiddenOffsetPercent(body: string | null): number | null {
  const m = declarations(body).match(/transform:\s*translateY\(\s*-(\d+(?:\.\d+)?)%\s*\)/)
  return m ? Number.parseFloat(m[1]) : null
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
  return requireMain(shellRoot(wrapper))
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
    expect(skipLinkInsideMain(shellRoot(wrapper))).toBe(false)
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
  it('规则体含 position: fixed/absolute 与「藏起来」机制（transform 推出视口）', () => {
    const body = ruleBody(cssRoot(), '.ds-skip-link')
    expect(body, 'layout.css 缺少 .ds-skip-link 规则').not.toBeNull()
    expect(body).toMatch(/position:\s*(absolute|fixed)/)
    // 藏起来的机制：位移必须**至少等于自身高度**（≥100%），否则元素仍有部分留在视口内、
    // 跳链一直可见。只看「有没有负号」会放过 -1% / -0%（#380 复核 F4 实测三条正则都真）。
    expect(hiddenOffsetPercent(body)).toBeGreaterThanOrEqual(100)
    // 不能靠 display:none / visibility:hidden —— 那会让它退出 Tab 序列与可访问性树
    expect(declarations(body)).not.toMatch(/display:\s*none/)
    expect(declarations(body)).not.toMatch(/visibility:\s*hidden/)
  })

  it(':focus 把位移归零（聚焦时真的出现在视口内）', () => {
    const body = ruleBody(cssRoot(), '.ds-skip-link:focus')
    expect(body, '缺少 .ds-skip-link:focus 规则').not.toBeNull()
    expect(declarations(body)).toMatch(/transform:\s*(none|translateY\(0\))/)
  })

  it('z-index 高于默认口令 banner（9000）——否则 390 宽下跳链被它整块遮住', () => {
    const body = declarations(ruleBody(cssRoot(), '.ds-skip-link'))
    const z = /z-index:\s*(\d+)/.exec(body)
    expect(z, '.ds-skip-link 缺少 z-index').not.toBeNull()
    // App.vue 的 .default-password-banner 是 9000（fixed + top:16px，min-width 360px），
    // 390 宽时几乎横满视口并压住左上的 skip link：复核实测 elementFromPoint 命中 banner。
    expect(Number(z![1])).toBeGreaterThan(9000)
    // 也不该越过弹窗遮罩（10000）：弹窗打开时焦点被 focus trap 锁在框内，
    // skip link 本就不该浮在遮罩之上。
    expect(Number(z![1])).toBeLessThan(10000)
  })

  it('token-only：颜色/间距/字号/圆角/阴影全走 token（dz/token-literal 同口径）', () => {
    // 覆盖范围必须与用例名一致：早先只查 5 个属性却叫「token-only」，
    // box-shadow 里的 color-mix(...) 就在覆盖缺口里（#380 复核意见）。
    const body = declarations(ruleBody(cssRoot(), '.ds-skip-link'))
    // 颜色：不得出现 hex / rgb() / hsl() / 颜色关键词
    expect(body).not.toMatch(/#[0-9a-f]{3,8}\b/i)
    expect(body).not.toMatch(/\b(?:rgba?|hsla?)\(/i)
    // 间距/字号/圆角/阴影：这些属性必须走 token
    for (const prop of ['padding', 'font-size', 'border-radius', 'top', 'left', 'box-shadow']) {
      const m = body.match(new RegExp(`(?:^|[;{\\s])${prop}:\\s*([^;]+);`))
      expect(m, `缺少 ${prop} 声明`).not.toBeNull()
      expect(m![1], `${prop} 必须是 token`).not.toMatch(/\b\d+(?:\.\d+)?px\b/)
    }
  })
})

// ---- 布局不变量（jsdom 无布局，只能断言 CSS 规则体文本）----
// #380 复核实测：删掉 .app-shell__main 的 flex / min-height:0 后，
// main 高 868→2349、页面内滚动失效、文档滚动高度仍 900 —— 全站内容被裁死，
// 而当时 63 例单测全绿。这不是代码错，是可回归性缺口，所以直接锁规则体。
describe('#380 .app-shell__main 的布局不变量（删掉即全站内容被裁）', () => {
  it('flex: 1 1 auto + min-height: 0 + display: flex + flex-direction: column 四条齐备', () => {
    const body = declarations(ruleBody(shellStyleRoot(), '.app-shell__main'))
    expect(body, 'AppShell.vue 缺少 .app-shell__main 规则').not.toBe('')
    // flex 让正文吃掉 workspace 的剩余高度
    expect(body, '缺少 flex: 1 1 auto').toMatch(/flex:\s*1\s+1\s+auto/)
    // min-height: 0 是 flex 子项能真正滚动的唯一前提（默认 min-height:auto 会撑破容器）
    expect(body, '缺少 min-height: 0').toMatch(/min-height:\s*0/)
    // 它自己还得是 flex 容器，页面组件（.monitor-view 等）的 flex:1 才有意义
    expect(body, '缺少 display: flex').toMatch(/display:\s*flex/)
    expect(body, '缺少 flex-direction: column').toMatch(/flex-direction:\s*column/)
  })

  it('聚焦时不在整块正文外围画 UA 描边（outline-offset 内缩）', () => {
    const body = declarations(ruleBody(shellStyleRoot(), '.app-shell__main'))
    // tabindex="-1" 让 main 可被聚焦，UA 默认会在整个正文外画一圈 1px 描边
    // （1440 下实测 1220×868）—— 读起来像「选中了全页」。内缩或显式 none 都算处理过。
    expect(body).toMatch(/outline-offset:\s*-|outline:\s*none|outline:\s*0/)
  })
})

// ---- 反证：四条用例，全部走**生产谓词**，喂的是改动前的真实结构 ----
// 关键：不能自己 `querySelector` 后当场 `throw` 再 `expect(...).toThrow()` ——
// 那验证的是本用例手写的 throw，与被测代码无关（#380 复核 F3 点名的恒真形态）。
// 这里每一条都调用上面那个**正向用例也在用**的函数，断言它的返回值在旧结构下必然不满足。
describe('#380 反证：结构退回改动前，生产谓词必须判红', () => {
  it('requireMain 在没有 <main> 的旧结构上抛错（正向用例也用同一函数取 main）', () => {
    const host = document.createElement('div')
    host.innerHTML = '<div class="app-shell"><div class="workspace app-shell__workspace"><div>content</div></div></div>'
    expect(() => requireMain(host)).toThrow(/没有 <main>/)
  })

  it('skipLinkInsideMain 在「skip link 被塞进 main」时返回 true（正向用例要求 false）', () => {
    const host = document.createElement('div')
    host.innerHTML = '<div class="workspace"><main id="main-content"><a href="#main-content" class="ds-skip-link">跳到主内容</a></main></div>'
    expect(skipLinkInsideMain(host)).toBe(true)
  })

  it('页面 header 在 main 外时 isBannerHeader 计数是 2（改动前的实测形态）', () => {
    const host = document.createElement('div')
    host.innerHTML = [
      '<div class="app-shell">',
      '  <div class="workspace">',
      '    <header class="topbar"></header>',
      '    <div class="monitor-view"><div><header class="monitor-view__header"></header></div></div>',
      '  </div>',
      '</div>',
    ].join('')
    expect(Array.from(host.querySelectorAll('header'))).toHaveLength(2)
    expect(Array.from(host.querySelectorAll('header')).filter(isBannerHeader)).toHaveLength(2)
  })

  it('把页面 header 挪进 main 后 isBannerHeader 计数降到 1（本票的修法本身）', () => {
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

  // F4 的自证：hiddenOffsetPercent 必须把「看起来像藏起来、其实没藏住」的值判红
  it.each([
    ['translateY(-200%)', 200],
    ['translateY(-100%)', 100],
    ['translateY(-1%)', 1],
    ['translateY(-0%)', 0],
    ['translateY(0)', null],
    ['none', null],
  ])('hiddenOffsetPercent 对 %s 返回 %s（<100 即判不合格）', (value, expected) => {
    const body = `.x { transform: ${value}; }`
    const percent = hiddenOffsetPercent(body)
    expect(percent).toBe(expected)
    if (percent !== null) {
      // 正向用例的判据：≥100 才算藏起来。-1% / -0% 在这里必须落到「不合格」一侧。
      expect(percent >= 100).toBe(percent === 200 || percent === 100)
    }
  })
})
