import { describe, it, expect } from 'vitest'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Rule } from 'postcss'

// #379：三个自绘表单控件的 <input> 曾用 `display: none` 隐藏，等于把控件连同无障碍树
// 一起摘掉 —— CDP 实测 `input.focus()` 后 activeElement 退回 BODY、`getClientRects()` 为 0、
// 整棵 AXTree 里没有 checkbox 角色节点。键盘与读屏用户完全无法操作 `.ds-switch`
// （「显示全部网卡」等 4 处调用）、`.ds-check`（36 处）、`.ds-radio`（4 处）。
//
// 本 spec 锁三件不变量：
//   1. 三处 input 规则里不得再出现 `display: none`（以及 `visibility: hidden` 这类同样
//      摘掉无障碍节点的写法）—— 这是**回归**的唯一机器信号，改回去必须变红；
//   2. 隐藏形态是标准的 visually-hidden：1×1 + 绝对定位 + clip-path: inset(50%)，
//      控件仍在无障碍树里、仍可聚焦，人眼不可见且不占布局；
//   3. `.ds-check` / `.ds-radio` 是绝对定位 input 的包含块（`position: relative`），
//      否则 1×1 盒会挂到最近的 positioned 祖先上，可能撑出滚动条或布局偏移；
//   4. 焦点环画在**视觉控件**上（1×1 且被 clip 的 input 上画环等于没画）。
//
// 断言全部锚定到「具体选择器的规则体」（同 MonitorViewTrend.spec 的 ruleBody 口径），
// 不写「文件里有没有 position: absolute」这类任何规则都满足的恒真断言。
// 反向自证见本票报告：把 `display: none` 加回去，本文件必须变红。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const CSS_DIR = join(SPEC_DIR, '..')
const COMPONENTS_CSS = join(CSS_DIR, 'components.css')
const source = readFileSync(COMPONENTS_CSS, 'utf8')
const root = postcss.parse(source, { from: COMPONENTS_CSS })

/** 选择器列表中**精确包含**该选择器（按逗号拆分后 trim 相等）的全部规则。
 *
 *  必须精确相等而不是 `includes`：`.ds-switch input` 是 `.ds-switch input:checked + …`
 *  的子串，用子串匹配会拿错规则、断言的完全是另一条声明（恒真/假红两种都有）。
 *  同一选择器可能出现在多条规则里（基础规则 + 媒体查询覆盖），故返回全部。 */
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

/** 三处控件的隐藏契约：选择器 → 视觉控件（焦点环归属） */
const CONTROLS = [
  { input: '.ds-check input', owner: '.ds-check__box' },
  { input: '.ds-radio input', owner: '.ds-radio__dot' },
  { input: '.ds-switch input', owner: '.ds-switch__track' },
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

  it('变体选择器同样受管：任何 .ds-{check,radio,switch} 下的 input 规则都不得 display: none', () => {
    // 上一条只覆盖**精确**三写法；`.ds-check input[type="checkbox"]`、`.ds-switch > input`
    // 这类变体会绕过它。按「选择器起始 + 含 input」的宽口径再扫一遍整份文件。
    const pattern = /(^|[,{\s])\.ds-(?:check|radio|switch)(?:[^\s,{]*)?\s[^{}]*\binput\b[^{}]*\{([^}]*)\}/g
    const offenders: string[] = []
    let scanned = 0
    for (const match of source.matchAll(pattern)) {
      scanned++
      // 报 offender 用整段匹配的选择器部分（`match[1]` 只是行首/逗号边界，为空串）
      const selector = match[0].slice(0, match[0].indexOf('{')).trim()
      const body = match[2]
      if (/display\s*:\s*none/i.test(body) || /visibility\s*:\s*hidden/i.test(body)) {
        offenders.push(selector)
      }
    }
    // 扫描计数必须 > 0：正则写坏时会零匹配，`offenders` 恒为 []、本条退化成恒真
    expect(scanned, '正则没有匹配到任何 input 规则，护栏已失效').toBeGreaterThanOrEqual(3)
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
      // 尺寸：走 --spacer-1（1px 的 hairline token），不写裸 px（dz/token-literal 红线）
      expect(decl(rule, 'width')).toBe('var(--spacer-1)')
      expect(decl(rule, 'height')).toBe('var(--spacer-1)')
      // margin 必须为**负**同值，把 1×1 盒拉回元素原点，避免多出 1px 的布局占位
      expect(decl(rule, 'margin')).toBe('calc(-1 * var(--spacer-1))')
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

describe('#379 焦点环：画在视觉控件上（#332 全站环未落地时的最小可用指示）', () => {
  it('.ds-switch:has(input:focus-visible) 规则存在且给出可见 outline', () => {
    const rules = rulesWithSelector('.ds-switch:has(input:focus-visible)')
    expect(rules).toHaveLength(1)
    expect(decl(rules[0], 'outline')).toBe('auto')
    expect(decl(rules[0], 'outline-offset')).toBe('var(--spacer-2)')
  })

  it('.ds-check / .ds-radio 的焦点环落在 .ds-check__box / .ds-radio__dot 上', () => {
    for (const selector of [
      '.ds-check:has(input:focus-visible) .ds-check__box',
      '.ds-radio:has(input:focus-visible) .ds-radio__dot',
    ]) {
      const rules = rulesWithSelector(selector)
      expect(rules, `${selector} 规则不存在`).toHaveLength(1)
      expect(decl(rules[0], 'outline')).toBe('auto')
      expect(decl(rules[0], 'outline-offset')).toBe('var(--spacer-2)')
    }
  })

  it('焦点环不自造第二套颜色：不写 outline-color，交给 UA（#332 定稿后再统一收敛）', () => {
    // 写死颜色会与 #332 的 token 化方案打架，也会在浅色主题下退化成不可见的近色描边
    for (const selector of [
      '.ds-switch:has(input:focus-visible)',
      '.ds-check:has(input:focus-visible) .ds-check__box',
      '.ds-radio:has(input:focus-visible) .ds-radio__dot',
    ]) {
      const [rule] = rulesWithSelector(selector)
      expect(decl(rule, 'outline-color')).toBeUndefined()
      expect(decl(rule, 'box-shadow')).toBeUndefined()
    }
  })

  it('.ds-check 的焦点态恢复 color 继承：否则 UA 的 auto 环取 --text-onbrand 的白，浅色主题看不见', () => {
    // 浏览器实测：.ds-check__box 的 color 是 #FFFFFF（为画白勾），outline: auto 的环色跟着
    // color 走 → 白底白环，等于没有焦点指示。这一条锁住修复不被「精简掉看起来没用的一行」。
    const [boxRule] = rulesWithSelector('.ds-check:has(input:focus-visible) .ds-check__box')
    expect(decl(boxRule, 'color')).toBe('inherit')
    // 单选圆点不需要：.ds-radio__dot 的 color 未被改写，本就继承 --text-default
    const [dotRule] = rulesWithSelector('.ds-radio:has(input:focus-visible) .ds-radio__dot')
    expect(decl(dotRule, 'color')).toBeUndefined()
  })

  it('反向：焦点环只在 :focus-visible 时出现，未聚焦的视觉控件不带 clip-path/outline', () => {
    // 防「把 visually-hidden 的 clip-path 误贴到可见控件上」——那会让开关/勾选框直接消失。
    // 这条同时证明上面的断言确实锚定在 input 规则上，而不是任何规则都满足。
    const switchBase = rulesWithSelector('.ds-switch').filter((r) => decl(r, 'display') === 'inline-flex')
    expect(switchBase).toHaveLength(1)
    expect(decl(switchBase[0], 'clip-path')).toBeUndefined()
    expect(decl(switchBase[0], 'outline')).toBeUndefined()
    expect(decl(switchBase[0], 'width')).toBe('36px')
    expect(decl(switchBase[0], 'height')).toBe('20px')
  })
})
