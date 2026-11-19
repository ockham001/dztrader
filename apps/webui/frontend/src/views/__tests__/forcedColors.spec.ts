import { describe, it, expect } from 'vitest'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Root } from 'postcss'

// #381 forced-colors（Windows 高对比度模式）下的图形可读性。
//
// 两条规范事实（#363 audit 实测 + 本票 CDP 复验，不以浏览器差异为转移）：
//   - `box-shadow` 在 forced-colors 下被强制为 `none`；
//   - `background-color` 被统一成 `Canvas` / `ButtonFace`，即**所有靠背景色表达差异的图形同色**。
//   - `border` / `outline` 保留，且作者给的任何颜色（含 `transparent`）都被换成 `CanvasText`。
// 最后一条是下面所有修法的支点：透明描边在普通渲染下**不画像素**（正常主题零视觉变化），
// 在 forced-colors 下自动变成可见的系统色边框 —— 不需要写 `@media (forced-colors: active)` 分支，
// 也不需要 `forced-color-adjust: none`（那会剥夺用户的高对比度配色选择）。
//
// 本 spec 锁的是「图形仍有边界」这件事本身，不是某个具体像素：
//   A. 台账量程轨道的**填充块**（#363 只给轨道加了 outline，填充块没有 → 框在、比例不在）；
//   B. 组成带的**外框**（段间缝由 #363 的 `border-inline-start` 撑起，但首末段没有外缘）；
//   C. 组成带「已用」的**实心块描边**（原实现只有 `box-shadow: inset`）；
//   D. 每核条形的**轨道 + 填充**（两者都是纯背景色）。
//
// 反向断言（防「只加了一条无关规则」的假绿）：样式表里任何一条 `box-shadow` 都不许**单独**
// 承担边界语义 —— 带 `box-shadow` 的规则必须在同一条规则里同时给出 `border` 或 `outline`。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const VIEW_SOURCE = readFileSync(join(SPEC_DIR, '..', 'MonitorView.vue'), 'utf8')

/** 取 `<style scoped>` 段并解析；测试与生产读同一段文本，不手抄选择器内容 */
function styleRoot(): Root {
  const start = VIEW_SOURCE.indexOf('<style scoped>')
  const end = VIEW_SOURCE.indexOf('</style>', start)
  expect(start, 'MonitorView.vue 缺少 <style scoped>').toBeGreaterThan(-1)
  expect(end, 'MonitorView.vue 的 <style scoped> 未闭合').toBeGreaterThan(start)
  return postcss.parse(VIEW_SOURCE.slice(start + '<style scoped>'.length, end))
}

const ROOT = styleRoot()

/** 去掉注释：断言只能命中真实声明，不能被解释性注释里的示例文字顶成恒真 */
function declarations(body: string): string {
  return body.replace(/\/\*[\s\S]*?\*\//g, '')
}

/** 某个选择器的**基础**规则声明（media 块内的不算，用 ruleInMedia 取） */
function ruleBody(selector: string): string {
  const bodies: string[] = []
  ROOT.walkRules((rule) => {
    if (rule.parent?.type === 'atrule') return
    if (rule.selectors.some((s) => s.trim() === selector)) bodies.push(rule.toString())
  })
  expect(bodies.length, `缺少 ${selector} 的基础规则`).toBeGreaterThan(0)
  return declarations(bodies.join('\n'))
}

/** 是否为「写了实际边框」的声明（`border-radius` / `border-collapse` 不算；`transparent` 算） */
function hasRealBorder(body: string): boolean {
  return /(^|[\s;{])border(-(top|right|bottom|left|inline|block)[a-z-]*)?\s*:/.test(body)
}

function hasOutline(body: string): boolean {
  return /(^|[\s;{])outline(-[a-z]+)?\s*:/.test(body)
}

describe('#381 forced-colors：靠背景色表达的图形必须有 border / outline', () => {
  it('A. 量程轨道的填充块自带描边：轨道有空框还不够，「填充到哪里」才是这条图形要传达的信息', () => {
    const body = ruleBody('.monitor-view__ledger-track-fill')
    // 描边色可以是 transparent（普通渲染零像素，forced-colors 下自动取 CanvasText）
    expect(body, '填充块必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 负 offset：描边画在填充块内部，否则会在轨道上额外长出 2px 宽度
    expect(body).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    // 不许用 border 顶替：border 占盒模型，`--ledger-fill: 0.4%` 会被 1px 边界抬高成可见的谎报
    expect(hasRealBorder(body), '不得用 border（会改变盒模型 / 比例语义）').toBe(false)
  })

  it('A2. 四个等级色都继承同一条描边规则（不各写一份，避免其中一档漏掉）', () => {
    // 等级色只改 background，不重复定义 outline：重复定义会有「改一档忘一档」的漂移面
    for (const level of ['normal', 'warn', 'danger']) {
      const body = ruleBody(`.monitor-view__ledger-row[data-level='${level}'] .monitor-view__ledger-track-fill`)
      expect(body, level).toContain('background: var(--status-')
      expect(hasOutline(body), `${level} 不应各自重复定义 outline`).toBe(false)
    }
  })

  it('B. 组成带整条有外框：段间缝只管内部，首末段的外缘只有 border 撑不起来', () => {
    const band = ruleBody('.monitor-view__ledger-band')
    expect(band, '组成带必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 仍是 10px 高的细带子：border 会挤掉内容，必须用不占盒模型的 outline
    expect(hasRealBorder(band)).toBe(false)
  })

  it('C. 「已用」段同时有两种描边：box-shadow 管正常主题，outline 管 forced-colors', () => {
    const used = ruleBody('.monitor-view__ledger-band-seg--used')
    // 正常主题的内描边保持原样（#363 C7 的视觉口径，本票不动）
    expect(used).toContain('box-shadow: inset 0 0 0 var(--border-width-default) var(--bg-layout-1)')
    // forced-colors 下 box-shadow 被强制 none，必须另有 outline 顶上
    expect(used, '「已用」需要 outline 兜底').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(used).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    // 段宽由 flex-basis 注入比例，border 会改变各段占比
    expect(hasRealBorder(used)).toBe(false)
  })

  it('D. 每核条形：轨道与填充各一圈描边，柱高与填充高才分得开', () => {
    const bar = ruleBody('.monitor-view__core-bar')
    expect(bar, '轨道需要 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 轨道高度是 `--core-bar-height` 定值，border 会把画布缩掉 2px
    expect(hasRealBorder(bar)).toBe(false)

    const fill = ruleBody('.monitor-view__core-bar-fill')
    expect(fill, '填充需要 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(fill).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    expect(hasRealBorder(fill)).toBe(false)
    // 填充高度仍只由 `--core-fill` 驱动：加了描边不得引入新的高度来源
    expect(fill).toContain('height: var(--core-fill, 0%)')
  })

  it('反向：样式表里没有任何一条 box-shadow 单独承担边界语义', () => {
    // 判据：一条规则只要写了 box-shadow（无论 inset 与否），就必须在同一条规则里同时写
    // border 或 outline。只写 box-shadow 的规则在 forced-colors 下会整条消失。
    const offenders: string[] = []
    ROOT.walkRules((rule) => {
      const body = declarations(rule.toString())
      if (!/(^|[\s;{])box-shadow\s*:/.test(body)) return
      // 值为 none 的声明不画任何东西，不算「承担边界」
      if (/(^|[\s;{])box-shadow\s*:\s*none/.test(body)) return
      if (hasRealBorder(body) || hasOutline(body)) return
      offenders.push(`${rule.selector} → ${body.trim().replace(/\s+/g, ' ').slice(0, 80)}`)
    })
    expect(offenders).toEqual([])
  })

  it('反向：没有用 forced-color-adjust 抄近路（它剥夺用户的高对比度配色选择）', () => {
    // 按声明判定，不按源码文本：解释性注释里会**提到**这两样东西（说明为什么不用），
    // 直接 `not.toContain` 会把说明本身当违规（本 spec 第一版就是这样恒红的）。
    let adjustDecls = 0
    ROOT.walkDecls((decl) => {
      if (decl.prop.toLowerCase() === 'forced-color-adjust') adjustDecls += 1
    })
    expect(adjustDecls, '不得用 forced-color-adjust').toBe(0)
    // 也没有为 forced-colors 单独写媒体分支：透明描边已经覆盖两种模式，
    // 多一个 @media 分支就多一份「只在某浏览器生效」的漂移面
    let forcedMedia = 0
    ROOT.walkAtRules('media', (atRule) => {
      if (atRule.params.includes('forced-colors')) forcedMedia += 1
    })
    expect(forcedMedia, '不得为 forced-colors 单写媒体分支').toBe(0)
  })
})
