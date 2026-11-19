import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { dirname, join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Rule } from 'postcss'

// #379：三个自绘表单控件的 <input> 曾用 `display: none` 隐藏，等于把控件连同无障碍树
// 一起摘掉 —— CDP 实测 `input.focus()` 后 activeElement 退回 BODY、`getClientRects()` 为 0、
// 整棵 AXTree 里没有 checkbox 角色节点。键盘与读屏用户完全无法操作开关与勾选框。
//
// 元素计数（按 class 词的 `<label>` 元素数，各文件逐一核对；不要凭 grep 行数——
// 一行一个元素，但 `v-for` 会在运行时展开成多个）：
//   `.ds-check` 16 个 label（logs 2 / marketSources 1 / sysmon 4 / trading 5 / views 4）
//   `.ds-radio` 1 个 label（BrokerCard）
//   `.ds-switch` 运行时 4 个：`<SyncSwitch>` 3 处调用 + LogViewerTab 手写 1
//
// 本 spec 锁五件不变量：
//   1. 三处 input 规则里不得再出现 `display: none`（以及 `visibility: hidden` 这类同样
//      摘掉无障碍节点的写法）—— 这是**回归**的唯一机器信号，改回去必须变红；
//   2. 隐藏形态是标准的 visually-hidden：1×1 + 绝对定位 + clip-path: inset(50%)，
//      控件仍在无障碍树里、仍可聚焦，人眼不可见且不占布局；
//   3. `.ds-check` / `.ds-radio` 是绝对定位 input 的包含块（`position: relative`），
//      否则 1×1 盒会挂到最近的 positioned 祖先上，可能撑出滚动条或布局偏移；
//   4. 焦点环画在**视觉控件**上（1×1 且被 clip 的 input 上画环等于没画），且**不依赖
//      `:has()`**——Vite 5 默认 target 到 firefox78，`:has()` 要 121+，本仓无 browserslist；
//   5. 第 4 条所依赖的 DOM 前提（视觉块是 input 的紧邻下一兄弟）在每个 `.vue` 调用点都成立。
//      这一条若失守，焦点环会**静默消失**而其余断言全绿。
//
// 断言全部锚定到「具体选择器的规则体」（同 MonitorViewTrend.spec 的 ruleBody 口径），
// 不写「文件里有没有 position: absolute」这类任何规则都满足的恒真断言。
// 反向自证见本票报告：把 `display: none` 加回去 / 删掉 `color: inherit` / 加一行注释提到
// `display: none`，对应的断言必须变红。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const CSS_DIR = join(SPEC_DIR, '..')
const COMPONENTS_CSS = join(CSS_DIR, 'components.css')
const SRC_ROOT = join(CSS_DIR, '..', '..')
const source = readFileSync(COMPONENTS_CSS, 'utf8')
const root = postcss.parse(source, { from: COMPONENTS_CSS })

/** 选择器列表中**精确包含**该选择器（按逗号拆分后 trim 相等）的全部规则。
 *
 *  必须精确相等而不是 `includes`：`.ds-switch input` 是 `.ds-switch input:checked + …`
 *  的子串，用子串匹配会拿错规则、断言的完全是另一条声明（恒真/假红两种都有）。
 *  同一选择器可能出现在多条规则里（基础规则 + 媒体查询覆盖 + 同选择器的补充声明），故返回全部。 */
function rulesWithSelector(selector: string): Rule[] {
  const out: Rule[] = []
  root.walkRules((rule) => {
    const selectors = (rule.selectors ?? []).map((s) => s.trim())
    if (selectors.includes(selector)) out.push(rule)
  })
  return out
}

/** 取规则体内某属性的值（同名声明取最后一条，与 CSS 层叠语义一致） */
function decl(rule: Rule, prop: string): string | undefined {
  let value: string | undefined
  rule.walkDecls((d) => {
    if (d.prop.trim().toLowerCase() === prop) value = d.value.trim()
  })
  return value
}

/** 某选择器的规则里，**声明了该属性**的那几条（同一选择器可能既有环规则又有 color 补充） */
function rulesDeclaring(selector: string, prop: string): Rule[] {
  return rulesWithSelector(selector).filter((rule) => decl(rule, prop) !== undefined)
}

/** 三处控件：隐藏规则选择器 / 视觉块（焦点环归属）/ 该视觉块 class 词 */
const CONTROLS = [
  { input: '.ds-check input', visual: '.ds-check__box', token: 'ds-check__box' },
  { input: '.ds-radio input', visual: '.ds-radio__dot', token: 'ds-radio__dot' },
  { input: '.ds-switch input', visual: '.ds-switch__track', token: 'ds-switch__track' },
] as const

const CONTAINER = ['position: absolute', 'overflow: hidden', 'clip-path: inset(50%)', 'white-space: nowrap', 'border: 0'] as const

describe('#379 隐藏控件：input 不得被 display: none 摘出无障碍树', () => {
  it('三处 input 的规则体都不含 display: none / visibility: hidden', () => {
    for (const { input } of CONTROLS) {
      const rules = rulesWithSelector(input)
      // 先确认规则存在，否则下面的循环零次迭代、断言恒真
      expect(rules.length, `${input} 规则不存在`).toBeGreaterThan(0)
      for (const rule of rules) {
        expect(decl(rule, 'display'), `${input} 又用了 display: none（会把控件摘出无障碍树）`).not.toBe('none')
        expect(decl(rule, 'visibility'), `${input} 不得用 visibility: hidden`).not.toBe('hidden')
      }
    }
  })

  it('变体选择器同样受管：任何 .ds-{check,radio,switch} 起始且含 input 的规则都不得 display: none', () => {
    // 上一条只覆盖**精确**三写法；`.ds-check input[type="checkbox"]`、`.ds-switch > input`
    // 这类变体会绕过它。按「选择器以 .ds-{check,radio,switch} 起始 + 含 input 词」的宽口径
    // 再扫一遍整份文件。
    //
    // 用 postcss AST 的 walkRules 而不是对原文跑正则：注释是独立的 Comment 节点，天然跳过。
    // 正则版本（审查实测）会被**注释里提到的** `.ds-switch input { display: none }` 假红 ——
    // 代码一个字没改，只加一行说明就红，那会让注释变成禁区。
    const offenders: string[] = []
    const scanned: string[] = []
    root.walkRules((rule) => {
      for (const raw of rule.selectors ?? []) {
        const selector = raw.trim()
        if (!/^\.ds-(?:check|radio|switch)\b/.test(selector)) continue
        if (!/\binput\b/.test(selector)) continue
        scanned.push(selector)
        if (decl(rule, 'display') === 'none' || decl(rule, 'visibility') === 'hidden') offenders.push(selector)
      }
    })
    // 扫描计数必须 >= 3：选择器写坏时会零匹配，`offenders` 恒为 []、本条退化成恒真。
    // 当前实测恰为 3（三处 input 共用一条规则）。
    expect(scanned.length, '没有扫到任何 input 规则，护栏已失效').toBeGreaterThanOrEqual(3)
    expect(offenders).toEqual([])
  })

  it('隐藏形态是标准 visually-hidden：1×1 绝对定位 + clip-path: inset(50%)', () => {
    for (const { input } of CONTROLS) {
      const hiding = rulesWithSelector(input).filter((rule) => decl(rule, 'clip-path') === 'inset(50%)')
      expect(hiding, `${input} 缺少 clip-path: inset(50%) 的 visually-hidden 规则`).toHaveLength(1)
      const rule = hiding[0]
      for (const pair of CONTAINER) {
        const [prop, value] = pair.split(': ') as [string, string]
        expect(decl(rule, prop), `${input} 的隐藏规则缺少 ${pair}`).toBe(value)
      }
      // 尺寸：当前取 --spacer-1（1px 的 hairline token）。这里**不锁写法**——将来把 1px 提成
      // 专用 token 或改写成别的 var() 都是进步，锁死字面量反而会拦住 token 化。
      // 真正的不变量是「是个 1px 量级的定值」：var() 形态无法在此求值，只校验形态合法。
      for (const prop of ['width', 'height'] as const) {
        const value = decl(rule, prop)
        expect(value, `${input} 的 ${prop} 缺失`).toBeDefined()
        expect(value, `${input} 的 ${prop} 必须是 1px 量级：px 字面量或 var() token`)
          .toMatch(/^(?:1px|var\(--[a-z0-9-]+\))$/)
        // 不能是 1×1 之外的值：那会让控件露出可见边缘
        expect(value, `${input} 的 ${prop} 不得是 auto/0/calc()`).not.toMatch(/^(?:auto|0)$|calc\(/)
      }
      // margin 必须为**负**同值，把 1×1 盒拉回元素原点，避免多出 1px 的布局占位
      expect(decl(rule, 'margin'), `${input} 的负 margin 缺失`).toMatch(/^calc\(-1 \* (?:1px|var\(--[a-z0-9-]+\))\)$/)
      expect(decl(rule, 'padding')).toBe('0')
    }
  })

  it('.ds-check / .ds-radio 是绝对定位 input 的包含块：基础规则带 position: relative', () => {
    for (const selector of ['.ds-check', '.ds-radio']) {
      const rules = rulesWithSelector(selector)
      expect(rules.length, `${selector} 规则不存在`).toBeGreaterThan(0)
      // 必须是**基础规则**（display: inline-flex 的那条）带 relative，而不是只在
      // @media (pointer: coarse) 的 min-height 覆盖里带 —— 后者在没有触摸设备时完全不生效
      const base = rules.filter((rule) => decl(rule, 'display') === 'inline-flex')
      expect(base, `${selector} 基础规则不存在`).toHaveLength(1)
      expect(decl(base[0], 'position'), `${selector} 缺少 position: relative（绝对定位 input 会飞到别的祖先上）`)
        .toBe('relative')
    }
  })
})

describe('#379 焦点环：画在视觉控件上，且不依赖 :has()', () => {
  it('三处焦点环都走相邻兄弟选择器（Firefox 78 可用），并给出可见 outline', () => {
    for (const { input, visual } of CONTROLS) {
      const selector = `${input}:focus-visible + ${visual}`
      const rules = rulesDeclaring(selector, 'outline')
      expect(rules, `${selector} 缺少 outline 规则`).toHaveLength(1)
      expect(decl(rules[0], 'outline')).toBe('auto')
      expect(decl(rules[0], 'outline-offset')).toBe('var(--spacer-2)')
    }
  })

  it('焦点指示不得依赖 :has()：任何 :focus-visible 选择器里都不出现 :has(', () => {
    // 本仓无 browserslist，Vite 5 默认 target 到 firefox78，而 :has() 需要 121+。
    // 用 :has() 做焦点环 = 在老 Firefox 上焦点完全不可见，且**没有任何报错**。
    // （`:has(input:checked)` 用于选中态不在此限：那条降级后只是少个配色，控件仍可操作。）
    const offenders: string[] = []
    let focusRules = 0
    root.walkRules((rule) => {
      for (const raw of rule.selectors ?? []) {
        const selector = raw.trim()
        if (!selector.includes(':focus-visible')) continue
        focusRules++
        if (selector.includes(':has(')) offenders.push(selector)
      }
    })
    expect(focusRules, '没有任何 :focus-visible 规则，本段护栏已失效').toBeGreaterThanOrEqual(3)
    expect(offenders).toEqual([])
  })

  it('焦点环不自造第二套颜色：不写 outline-color / box-shadow（#332 定稿后再统一收敛）', () => {
    // 写死颜色会与 #332 的 token 化方案打架，也会在浅色主题下退化成不可见的近色描边
    for (const { input, visual } of CONTROLS) {
      const selector = `${input}:focus-visible + ${visual}`
      for (const rule of rulesWithSelector(selector)) {
        expect(decl(rule, 'outline-color'), `${selector} 不得写 outline-color`).toBeUndefined()
        expect(decl(rule, 'box-shadow'), `${selector} 不得用 box-shadow 代替 outline`).toBeUndefined()
      }
    }
  })

  it('.ds-check 的焦点态恢复 color 继承：否则 UA 的 auto 环取 --text-onbrand 的白，浅色主题看不见', () => {
    // 浏览器实测：.ds-check__box 的 color 是 #FFFFFF（为画白勾），outline: auto 的环色跟着
    // color 走 → 白底白环，等于没有焦点指示。这一条锁住修复不被「精简掉看起来没用的一行」。
    const selector = '.ds-check input:focus-visible + .ds-check__box'
    const colored = rulesDeclaring(selector, 'color')
    expect(colored.length, `${selector} 缺少 color: inherit`).toBeGreaterThan(0)
    expect(decl(colored[colored.length - 1], 'color')).toBe('inherit')
    // 单选圆点不需要：.ds-radio__dot 的 color 未被改写，本就继承 --text-default
    expect(decl(rulesWithSelector('.ds-radio input:focus-visible + .ds-radio__dot')[0], 'color')).toBeUndefined()
  })

  it('开关的环画在 __track 上，且该 track 焦点态是胶囊形（否则 36×20 的胶囊开关套一圈直角框）', () => {
    // `.ds-switch` 本身是 border-radius: var(--radius-full) 的胶囊，但兄弟选择器只能选中
    // input 的兄弟（= __track），而 track 自身没有圆角 —— UA 会画直角环。浏览器实测：
    // 不补这一行，环就是一个 34×18 的直角矩形（截图 379v2-switch-sibling-ring.png）。
    const selector = '.ds-switch input:focus-visible + .ds-switch__track'
    const rules = rulesWithSelector(selector)
    expect(rules.length, `${selector} 规则不存在（环 + 胶囊圆角是两条）`).toBeGreaterThanOrEqual(2)
    const radius = rules.map((rule) => decl(rule, 'border-radius')).filter(Boolean)
    expect(radius, `${selector} 缺少胶囊圆角（环会画成直角框）`).toEqual(['var(--radius-full)'])
    // 未聚焦时 track 不带圆角/outline：这两条只在画环时有意义，常驻会改变视觉
    const trackBase = rulesWithSelector('.ds-switch__track')
    expect(trackBase).toHaveLength(1)
    expect(decl(trackBase[0], 'border-radius')).toBeUndefined()
  })

  it('反向：未聚焦的视觉控件不带 clip-path / outline', () => {
    // 防「把 visually-hidden 的 clip-path 或焦点环误贴到可见控件上」——那会让开关/勾选框
    // 直接消失或常驻一圈环。这条同时证明上面的断言确实锚定在 input 的规则上。
    const switchBase = rulesWithSelector('.ds-switch').filter((rule) => decl(rule, 'display') === 'inline-flex')
    expect(switchBase).toHaveLength(1)
    expect(decl(switchBase[0], 'clip-path')).toBeUndefined()
    expect(decl(switchBase[0], 'outline')).toBeUndefined()
    // 开关是定宽定高的 36×20（px 或 token 都可）：不能是 1×1，也不能是 auto。
    // **不锁 `36px` 字面量**：把尺寸提成 token 是 dz/token-literal 鼓励的方向，锁字面量会拦它。
    for (const prop of ['width', 'height'] as const) {
      const value = decl(switchBase[0], prop)
      expect(value, `.ds-switch 的 ${prop} 缺失`).toBeDefined()
      expect(value, `.ds-switch 的 ${prop} 必须是 px 或 var() token`).toMatch(/^(?:\d+(?:\.\d+)?px|var\(--[a-z0-9-]+\))$/)
      expect(value, `.ds-switch 的 ${prop} 不得是 1×1 的隐藏尺寸`).not.toMatch(/^(?:1px|var\(--spacer-1\))$/)
    }
  })
})

// ── DOM 前提：焦点环的兄弟选择器依赖「视觉块是 input 的紧邻下一兄弟」 ──────────────
//
// 这一节守的是**静默失效**：有人把 `<span class="ds-check__box">` 挪到 input 前面、或中间插
// 一个 wrapper，兄弟选择器就一条都不命中，焦点环消失、无障碍树照旧健康、其余断言全绿。
// 扫描全部 `.vue`（不含 __tests__），对每个三处控件的 `<label>` 检查其内部结构。

/** 递归收集 src 下全部 .vue（跳过 node_modules/dist 与测试目录） */
function collectVueFiles(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (name === 'node_modules' || name === 'dist' || name === '__tests__') continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) collectVueFiles(full, out)
    else if (extname(name) === '.vue') out.push(full)
  }
  return out
}

/** 从 `<` 位置起找到标签结束的 `>`，跳过引号内的内容。
 *  必须跳过引号：模板属性里有 `@change="(v: boolean) => …"` 这种含 `>` 的值。 */
function tagEnd(text: string, from: number): number {
  let quote: string | null = null
  for (let i = from; i < text.length; i++) {
    const ch = text[i]
    if (quote) {
      if (ch === quote) quote = null
    } else if (ch === '"' || ch === "'") {
      quote = ch
    } else if (ch === '>') {
      return i
    }
  }
  return -1
}

/** 取标签上的静态 class 属性值（`(?<![:\w.-])` 排除 `:class` / `class-foo`） */
function staticClass(tag: string): string | null {
  const match = /(?<![:\w.-])class="([^"]*)"/.exec(tag)
  return match ? match[1] : null
}

const CONTROL_LABEL_TOKENS = [
  { token: 'ds-check', visual: 'ds-check__box' },
  { token: 'ds-radio', visual: 'ds-radio__dot' },
  { token: 'ds-switch', visual: 'ds-switch__track' },
] as const

describe('#379 焦点环的 DOM 前提：视觉块是 input 的紧邻下一兄弟', () => {
  const vueFiles = collectVueFiles(SRC_ROOT)

  it('全部 .vue 的三处控件 label 都满足「input 在前、视觉块紧随其后」', () => {
    const violations: string[] = []
    let labels = 0
    for (const file of vueFiles) {
      const text = readFileSync(file, 'utf8')
      const rel = file.slice(SRC_ROOT.length + 1)
      for (const match of text.matchAll(/<label\b/g)) {
        const openEnd = tagEnd(text, match.index)
        if (openEnd < 0) continue
        const cls = staticClass(text.slice(match.index, openEnd + 1))
        if (!cls) continue
        const words = cls.split(/\s+/)
        const control = CONTROL_LABEL_TOKENS.find((c) => words.includes(c.token))
        if (!control) continue
        labels++
        const line = text.slice(0, match.index).split('\n').length

        // label 的第一个子元素必须是 <input
        const first = /<[a-zA-Z]/.exec(text.slice(openEnd + 1))
        if (!first || !text.slice(openEnd + 1 + first.index).startsWith('<input')) {
          violations.push(`${rel}:${line} 第一个子元素不是 <input>`)
          continue
        }
        const inputEnd = tagEnd(text, openEnd + 1 + first.index)
        if (inputEnd < 0) continue
        // input 之后的下一个元素必须是该控件的视觉块
        const next = /<[a-zA-Z]/.exec(text.slice(inputEnd + 1))
        if (!next) {
          violations.push(`${rel}:${line} input 之后没有视觉块`)
          continue
        }
        const nextStart = inputEnd + 1 + next.index
        const nextEnd = tagEnd(text, nextStart)
        const nextCls = staticClass(text.slice(nextStart, nextEnd + 1)) ?? ''
        if (!nextCls.split(/\s+/).includes(control.visual)) {
          violations.push(`${rel}:${line} input 的紧邻下一兄弟不是 .${control.visual}（实际 class="${nextCls}"）`)
        }
      }
    }
    // 扫描计数：三处控件的 label 合计 19 个（.ds-check 16 + .ds-radio 1 + .ds-switch 2，
    // 其中 .ds-switch 的 2 个是 SyncSwitch 组件自身与 LogViewerTab 的手写副本）。
    // 数字变小时先确认是调用点被删了，而不是扫描逻辑写坏。
    expect(labels, '没有扫到任何控件 label，扫描逻辑已失效').toBeGreaterThanOrEqual(19)
    expect(violations).toEqual([])
  })
})
