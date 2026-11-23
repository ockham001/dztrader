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
//   A. 存储块**卷容量条**的轨道与填充块（#450：两者都是纯背景色，不描边就整条消失）；
//   A3. 存储块**磁盘忙碌条**的轨道与填充块（同上）；
//   B. 组成带的**外框**（段间缝由 #363 的 `border-inline-start` 撑起，但首末段没有外缘）；
//   C. 组成带「已用」的**实心块描边**（原实现只有 `box-shadow: inset`）；
//   D. 每核条形的**轨道 + 填充**（两者都是纯背景色）；
//   E. 数字格细条（#445 交换使用率）与内存组成带（#363 迁入）。
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

/**
 * 取某条属性在规则体里的**最后一条**声明值（CSS 后写覆盖先写）。无该属性返回 null。
 *
 * 为什么按值判而不按属性名判（#381 审查 finding 3）：`outline: none` / `border: 0`
 * 是对应属性的**合法取值**，写了等于没写。只看「出现过 `outline` 字样」的判据会被
 * 这两条声明绕过——审查实测三个绕过探针全部绿灯。判据必须是「实际画得出来」。
 */
function lastValue(body: string, prop: string): string | null {
  const re = new RegExp(`(?:^|[\\s;{])${prop}\\s*:\\s*([^;}]+)`, 'g')
  let match: RegExpExecArray | null
  let value: string | null = null
  while ((match = re.exec(body)) !== null) value = match[1].trim()
  return value
}

/**
 * 是否真的画得出边框：`border` / `border-*` 的**简写或宽度**不是 0 / none。
 *
 * `border-radius` / `border-collapse` / `border-color` 不画边框，不算。
 */
function hasRealBorder(body: string): boolean {
  for (const prop of ['border', 'border-top', 'border-right', 'border-bottom', 'border-left',
    'border-inline', 'border-block', 'border-inline-start', 'border-inline-end',
    'border-block-start', 'border-block-end', 'border-width']) {
    const value = lastValue(body, prop)
    if (value === null) continue
    if (/^0(?![.\d])/.test(value)) continue          // border: 0 / 0px
    if (/\bnone\b/.test(value)) continue              // border: none
    return true
  }
  return false
}

/**
 * 是否真的画得出外轮廓：简写的 style 不是 none/hidden、宽度不为 0。
 *
 * `outline: none` / `outline: 0` / `outline: 0 solid transparent` 都算「没画」——
 * 审查实测这三种都能绕过只看属性名的判据。
 */
function hasOutline(body: string): boolean {
  const shorthand = lastValue(body, 'outline')
  if (shorthand !== null) {
    const lower = shorthand.toLowerCase()
    if (lower === 'none' || lower === 'initial' || lower === 'unset' || lower === 'revert') return false
    if (/^0(?![.\d])/.test(shorthand)) return false   // outline: 0
    if (/\bnone\b|\bhidden\b/.test(lower)) return false // outline: 0 none / hidden
    return true
  }
  // 分写形态：outline-style 决定「画不画」，缺省（无 style 声明）在 CSS 里是 none
  const style = lastValue(body, 'outline-style')
  if (style === null || /\bnone\b|\bhidden\b/.test(style.toLowerCase())) return false
  const width = lastValue(body, 'outline-width')
  if (width !== null && /^0(?![.\d])/.test(width)) return false
  return true
}

/**
 * 纯装饰投影（非 inset 且带模糊）不承担边界语义，`box-shadow` 消失不影响信息。
 *
 * 审查 finding 3 的另一半：原判据把「装饰」与「边界」混为一谈，一条
 * `box-shadow: 0 2px 8px rgba(0,0,0,.16)` 会被误判成缺陷。浮层投影就属这一类。
 */
function isDecorativeShadow(body: string): boolean {
  if (/(^|[\s;{])box-shadow\s*:\s*[^;}]*\binset\b/.test(body)) return false
  // 三个长度 + 颜色，第三个长度（blur）非 0 → 有模糊 → 装饰投影。
  // 长度要**连裸 0 一起匹配**：`box-shadow: 0 2px 8px …` 的第一个偏移量写作 `0`（无单位），
  // 只认 `px` 会把三元组数成两项、blur 取到 undefined（本 spec 第一版就这样漏了装饰豁免）。
  const value = lastValue(body, 'box-shadow') ?? ''
  const parts = value.split(/,(?![^(]*\))/)[0].trim()
  const lengths = parts.match(/(?:^|\s)-?(?:[\d.]+px|0)(?=\s|$)/g) ?? []
  if (lengths.length < 3) return false
  return Number.parseFloat(lengths[2]) > 0
}

describe('#381 forced-colors：靠背景色表达的图形必须有 border / outline', () => {
  it('A. 卷容量条的轨道与填充块各一圈描边：填充比例才分得开', () => {
    const track = ruleBody('.monitor-view__volume-track')
    expect(track, '容量条轨道必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 6px 高的细条不能被 border 挤掉画布
    expect(hasRealBorder(track)).toBe(false)

    const fill = ruleBody('.monitor-view__volume-fill')
    expect(fill, '容量条填充必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 负 offset：描边画在填充块内部，否则会在轨道上额外长出 2px 宽度
    expect(fill).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    // 不许用 border 顶替：border 占盒模型，`--volume-fill: 0.4%` 会被 1px 边界抬高成可见的谎报
    expect(hasRealBorder(fill), '不得用 border（会改变盒模型 / 比例语义）').toBe(false)
    // 填充宽度仍只由运行时变量驱动
    expect(fill).toContain('width: var(--volume-fill, 0%)')
  })

  it('A2. 卷容量条的三档色只改 background，不重复定义 outline（避免改一档忘一档）', () => {
    for (const level of ['warn', 'danger']) {
      const body = ruleBody(`.monitor-view__volume-fill[data-level='${level}']`)
      expect(body, level).toContain('background: var(--status-')
      expect(hasOutline(body), `${level} 不应各自重复定义 outline`).toBe(false)
    }
    // normal 与磁盘忙碌条同族：健康 = 成功色（只有越线才换 warn / danger）
    expect(ruleBody('.monitor-view__volume-fill')).toContain('background: var(--status-success-default)')
  })

  it('A3. 磁盘忙碌条的轨道与填充块各一圈描边', () => {
    const track = ruleBody('.monitor-view__disk-track')
    expect(track, '忙碌条轨道必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(hasRealBorder(track)).toBe(false)
    const fill = ruleBody('.monitor-view__disk-fill')
    expect(fill, '忙碌条填充必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(fill).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    expect(hasRealBorder(fill)).toBe(false)
    expect(fill).toContain('width: var(--disk-busy, 0%)')
  })

  it('B. 组成带整条有外框：段间缝只管内部，首末段的外缘只有 border 撑不起来', () => {
    const band = ruleBody('.monitor-view__memory-band')
    expect(band, '组成带必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 仍是 10px 高的细带子：border 会挤掉内容，必须用不占盒模型的 outline
    expect(hasRealBorder(band)).toBe(false)
  })

  it('C. 「已用」段同时有两种描边：box-shadow 管正常主题，outline 管 forced-colors', () => {
    const used = ruleBody('.monitor-view__memory-band-seg--used')
    // 正常主题的内描边保持原样（#363 C7 的视觉口径，本票不动）
    expect(used).toContain('box-shadow: inset 0 0 0 var(--border-width-default) var(--bg-layout-1)')
    // forced-colors 下 box-shadow 被强制 none，必须另有 outline 顶上
    expect(used, '「已用」需要 outline 兜底').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(used).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    // 段宽由 flex-basis 注入比例，border 会改变各段占比
    expect(hasRealBorder(used)).toBe(false)
  })

  it('E. 数字格细条（#445 交换使用率）：轨道与填充各一圈描边，填充比例才分得开', () => {
    const track = ruleBody('.monitor-view__stat-meter')
    expect(track, '细条轨道必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    // 4px 高的细条不能被 border 挤掉画布
    expect(hasRealBorder(track)).toBe(false)
    const fill = ruleBody('.monitor-view__stat-meter-fill')
    expect(fill, '细条填充必须有 outline').toMatch(/(^|[\s;{])outline\s*:\s*[^;]*solid\s+transparent/)
    expect(fill).toMatch(/outline-offset:\s*calc\(-1\s*\*\s*var\(--border-width-default\)\)/)
    expect(hasRealBorder(fill)).toBe(false)
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
      // 纯装饰投影（非 inset + 有模糊）不承担边界语义：它消失不影响任何信息
      if (isDecorativeShadow(body)) return
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
