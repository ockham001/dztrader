import { describe, it, expect } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { join } from 'node:path'

// 根 DESIGN.md frontmatter 的真实性回归（#356 审查收敛 S-6 / B-3）
//
// 为什么需要：根 `DESIGN.md` 的 YAML frontmatter 是**机器可读的设计 token 源**，
// Impeccable 之类的设计工具会直接把它当真值生成新 UI。它是
// `apps/webui/frontend/src/assets/css/design-tokens.css` + `theme.css` 的镜像，
// 但**镜像没有机械保证**——审查实测发现同一份 frontmatter 里反复出现：
//   - button-primary 背景标成 `#171717`，实际是 `--bg-invert`（浅 `#262626` / 深 `#e5e5e5`）
//   - button-secondary / input 标成纯白，实际是 `--bg-overlay-l1`（8% 灰透明）
//   - tag 标成 `#E5E5E5`，实际是 `--bg-overlay-l2`
//   - card 与 panel 深色档混用同一个值，实际 `--bg-base-secondary`(`#262626`)
//     与 `--bg-card`(`#202020`) 是两个不同值
// 这些错**不会让任何页面报错**，只会让下游按错的色值生成界面。
//
// 设计原则（第一版只硬校验了 7 个键 → 36 个变异体里 28 个静默通过，包括把
// 第一轮的原 bug 原样重放也检测不到。本版改为**数据驱动 + 全量覆盖**）：
//   1. 期望值从 CSS 源文件读取，不是硬编码——改 token 会连带要求改文档；
//   2. 每条断言覆盖一组键，新增键时只要加进映射表就自动被校验；
//   3. **组件绑定**单独校验（组件键引用了哪个色键、该色键的值是否等于对应 CSS token）——
//      这是第一轮错误的实际形态，第一版恰好漏掉；
//   4. 组存在性断言，防止整组被删后静默通过。
//
// 边界（有意不做）：不校验描述性文字；不校验 sidecar（需浏览器渲染，成本高）。

// 仓库根 = 本 spec 所在的 src/assets/css/__tests__ 往上 7 层：
//   __tests__(1) → css(2) → assets(3) → src(4) → frontend(5) → webui(6) → apps(7) → <root>
const ROOT = join(__dirname, '../../../../../../..')
const DESIGN_MD = join(ROOT, 'DESIGN.md')
const DESIGN_TOKENS_CSS = join(__dirname, '../design-tokens.css')
const THEME_CSS = join(__dirname, '../theme.css')

const ALLOWED_TOP_KEYS = new Set([
  'name', 'description', 'colors', 'typography', 'rounded', 'spacing', 'components',
])
const REQUIRED_SCALARS = ['name', 'description'] as const
const REQUIRED_GROUPS = ['colors', 'typography', 'rounded', 'spacing', 'components'] as const

// ── frontmatter 解析（只支持本项目用到的形态；解析失败即报错，不静默跳过） ──────────
function parseFrontmatter(text: string): { raw: string; data: Record<string, unknown> } {
  const m = text.match(/^---\n([\s\S]*?)\n---\n/)
  if (!m) throw new Error('DESIGN.md 缺少 YAML frontmatter')
  const raw = m[1]
  const data: Record<string, unknown> = {}
  const stack: Array<{ indent: number; node: Record<string, unknown> }> = [
    { indent: -1, node: data },
  ]
  for (const line of raw.split('\n')) {
    if (!line.trim() || line.trim().startsWith('#')) continue
    const indent = line.length - line.trimStart().length
    const km = line.trim().match(/^("[^"]*"|'[^']*'|[\w-]+):\s*(.*)$/)
    if (!km) continue
    // 键可能是带引号的数字（rounded / spacing 用 `"8": "8px"` 形态）
    const key = km[1].replace(/^["']|["']$/g, '')
    const rest = km[2]
    while (stack.length > 1 && stack[stack.length - 1].indent >= indent) stack.pop()
    const parent = stack[stack.length - 1].node
    if (rest === '') {
      const child: Record<string, unknown> = {}
      parent[key] = child
      stack.push({ indent, node: child })
    } else {
      let v: unknown = rest
      const q = rest.match(/^"(.*)"$/) || rest.match(/^'(.*)'$/)
      if (q) v = q[1]
      else if (/^-?\d+(\.\d+)?$/.test(rest)) v = Number(rest)
      parent[key] = v
    }
  }
  return { raw, data }
}

/**
 * 取 CSS 里某个自定义属性的值。**锚行首**，避免误取注释中提到的 token 名。
 * `--x:` 在 CSS 中总是语句开头（允许前导空白），故 `^` + `m` 是安全的。
 */
function cssToken(css: string, name: string): string | undefined {
  const re = new RegExp(`^\\s*--${name}:\\s*([^;]+);`, 'm')
  return re.exec(css)?.[1].trim()
}

/** 按「行首规则块」切浅色 / 深色两段（theme.css 的头部注释里就出现了 `.dark` 字样）。 */
function themeScopes(css: string): { light: string; dark: string } {
  const darkRule = /^\.dark\s*\{/m.exec(css)
  if (!darkRule) throw new Error('theme.css 找不到行首 .dark 规则块')
  return { light: css.slice(0, darkRule.index), dark: css.slice(darkRule.index) }
}


/**
 * 解析 token 值里的 `var(--x)` 别名链（token 常写成 `var(--font-family-default)` 这样）。
 * 最多递归 10 层，避免循环引用时死循环。
 */
function resolveVar(value: string, css: string, depth = 0): string {
  if (depth > 10) return value
  const m = /^var\((--[\w-]+)\)$/.exec(value.trim())
  if (!m) return value
  const inner = cssToken(css, m[1].slice(2))
  if (!inner) return value
  return resolveVar(inner, css, depth + 1)
}

/** 归一化颜色值以便比较：hex 统一大写、rgba 去空白、允许 8 位 hex。 */
function normColor(v: string | undefined): string {
  if (!v) return ''
  return v.replace(/\s+/g, '').toUpperCase()
}

/**
 * frontmatter 色键 → CSS token 的**全量映射**。新增色键时必须加进这张表，
 * 否则下面的 `completeness` 用例会失败（防止新键绕过校验）。
 */
const COLOR_TO_TOKEN: Record<string, { token: string; scope: 'light' | 'dark' | 'both' | 'tokens-dark' }> = {
  'ink-strongest': { token: 'text-default', scope: 'light' },
  'ink-secondary': { token: 'text-secondary', scope: 'light' },
  'ink-tertiary': { token: 'text-tertiary', scope: 'both' },
  'ink-disabled': { token: 'text-disabled', scope: 'light' },
  'ink-strongest-dark': { token: 'text-default', scope: 'dark' },
  'ink-secondary-dark': { token: 'text-secondary', scope: 'dark' },
  'paper-base': { token: 'bg-base-default', scope: 'light' },
  'paper-secondary': { token: 'bg-base-secondary', scope: 'light' },
  'paper-tertiary': { token: 'bg-base-tertiary', scope: 'light' },
  'paper-base-dark': { token: 'bg-base-default', scope: 'dark' },
  'paper-secondary-dark': { token: 'bg-base-secondary', scope: 'dark' },
  'paper-tertiary-dark': { token: 'bg-base-tertiary', scope: 'dark' },
  'invert-light': { token: 'bg-invert', scope: 'light' },
  'invert-dark': { token: 'bg-invert', scope: 'dark' },
  'overlay-l1': { token: 'bg-overlay-l1', scope: 'light' },
  'overlay-l2': { token: 'bg-overlay-l2', scope: 'light' },
  'overlay-l1-dark': { token: 'bg-overlay-l1', scope: 'dark' },
  'overlay-l2-dark': { token: 'bg-overlay-l2', scope: 'dark' },
  'surface-card-light': { token: 'bg-base-secondary', scope: 'light' },
  'surface-card-dark': { token: 'bg-base-secondary', scope: 'dark' },
  'surface-panel-light': { token: 'bg-card', scope: 'light' },
  'surface-panel-dark': { token: 'bg-card', scope: 'dark' },
  'on-invert-light': { token: 'bg-base-default', scope: 'light' },
  'on-invert-dark': { token: 'bg-base-default', scope: 'dark' },
  'brand-violet': { token: 'bg-brand', scope: 'light' },
  'brand-violet-hover': { token: 'bg-brand-hover', scope: 'light' },
  'brand-violet-active': { token: 'bg-brand-active', scope: 'light' },
  'rise-crimson': { token: 'color-rise', scope: 'both' },
  'fall-jade': { token: 'color-fall', scope: 'both' },
  'status-success': { token: 'status-success-default', scope: 'both' },
  'status-alert': { token: 'status-alert-default', scope: 'both' },
  'status-warning': { token: 'status-warning-default', scope: 'both' },
  'status-error': { token: 'status-error-default', scope: 'both' },
  'status-primary': { token: 'status-primary-default', scope: 'both' },
  // contract gold 定义在 design-tokens.css（其末段是 `.dark` 覆盖块），不在 theme.css
  'contract-gold': { token: 'bg-contract-main', scope: 'light' },
  'contract-gold-dark': { token: 'bg-contract-main', scope: 'tokens-dark' },
}

/**
 * 组件键 → 应绑定的色键（**这是第一轮错误的实际形态**：组件绑错颜色）。
 * 断言：组件.backgroundColor/textColor 引用的色键 == 这里声明的色键。
 */
const COMPONENT_BINDING: Record<
  string,
  { backgroundColor: string; textColor?: string }
> = {
  'button-primary': { backgroundColor: 'invert-light', textColor: 'on-invert-light' },
  'button-secondary': { backgroundColor: 'overlay-l1', textColor: 'ink-strongest' },
  'button-brand': { backgroundColor: 'brand-violet', textColor: 'paper-base' },
  'button-danger': { backgroundColor: 'status-error', textColor: 'paper-base' },
  input: { backgroundColor: 'overlay-l1', textColor: 'ink-strongest' },
  card: { backgroundColor: 'surface-card-light', textColor: 'ink-strongest' },
  panel: { backgroundColor: 'surface-panel-light', textColor: 'ink-strongest' },
  tag: { backgroundColor: 'overlay-l2', textColor: 'ink-secondary' },
}


/**
 * typography 键 → 每个属性**完整的 CSS token 名**（不拼接，避免前缀歧义）。
 * 多数条目四个属性同前缀；`metric` 是混合来源（字体取 --font-family-metric，
 * 字号/行高取 --body-xs-*），所以按属性映射。
 * 未登记的键由下面的 completeness 用例拦住。
 */
type TypeProp = 'fontSize' | 'fontWeight' | 'lineHeight' | 'fontFamily'
type TypeMap = Record<TypeProp, string>

const TYPE_TO_PREFIX: Record<string, TypeMap> = {
  display: {
    fontSize: 'heading-display-font-size',
    fontWeight: 'heading-display-font-weight',
    lineHeight: 'heading-display-line-height',
    fontFamily: 'heading-display-font-family',
  },
  'headline-md': {
    fontSize: 'heading-md-font-size',
    fontWeight: 'heading-md-font-weight',
    lineHeight: 'heading-md-line-height',
    fontFamily: 'heading-md-font-family',
  },
  'title-sm': {
    fontSize: 'heading-sm-font-size',
    fontWeight: 'heading-sm-font-weight',
    lineHeight: 'heading-sm-line-height',
    fontFamily: 'heading-sm-font-family',
  },
  'body-base': {
    fontSize: 'body-base-font-size',
    fontWeight: 'body-base-font-weight',
    lineHeight: 'body-base-line-height',
    fontFamily: 'body-base-font-family',
  },
  'body-sm': {
    fontSize: 'body-sm-font-size',
    fontWeight: 'body-sm-font-weight',
    lineHeight: 'body-sm-line-height',
    fontFamily: 'body-sm-font-family',
  },
  'body-xs': {
    fontSize: 'body-xs-font-size',
    fontWeight: 'body-xs-font-weight',
    lineHeight: 'body-xs-line-height',
    fontFamily: 'body-xs-font-family',
  },
  // label-md 的 500 来自 --body-md-strong-*（普通 --body-md-* 是 400）
  'label-md': {
    fontSize: 'body-md-strong-font-size',
    fontWeight: 'body-md-strong-font-weight',
    lineHeight: 'body-md-strong-line-height',
    fontFamily: 'body-md-strong-font-family',
  },
  metric: {
    fontSize: 'body-xs-font-size',
    fontWeight: 'body-xs-font-weight',
    lineHeight: 'body-xs-line-height',
    fontFamily: 'font-family-metric',
  },
  code: {
    fontSize: 'code-editor-font-size',
    fontWeight: 'code-editor-font-weight',
    lineHeight: 'code-editor-line-height',
    fontFamily: 'code-editor-font-family',
  },
}

/**
 * 组件键 → 必须与真实 CSS 相等的尺寸属性。
 * height / padding 是 Stitch 8-prop 里能表达的，且都是有据可查的实现值。
 */
const COMPONENT_SIZE: Record<string, { height?: string; padding?: string }> = {
  'button-primary': { height: '28px', padding: '0 14px' },
  'button-secondary': { height: '28px', padding: '0 14px' },
  'button-brand': { height: '28px', padding: '0 14px' },
  'button-danger': { height: '28px', padding: '0 14px' },
  input: { height: '32px', padding: '0 14px' },
  card: { padding: '24px' },
  tag: { height: '22px', padding: '0 10px' },
}

const designMd = readFileSync(DESIGN_MD, 'utf8')
const designTokensCss = readFileSync(DESIGN_TOKENS_CSS, 'utf8')
const themeCss = readFileSync(THEME_CSS, 'utf8')
const scopes = themeScopes(themeCss)
const fm = parseFrontmatter(designMd)
const colors = fm.data.colors as Record<string, string>
const components = fm.data.components as Record<string, Record<string, string>>

describe('DESIGN.md frontmatter 与 token 源真实一致（#356）', () => {
  it('文件存在且必需的顶层标量与组都在（整组被删不得静默通过）', () => {
    expect(existsSync(DESIGN_MD), `${DESIGN_MD} 不存在`).toBe(true)
    for (const key of REQUIRED_SCALARS) {
      expect(typeof fm.data[key], `frontmatter 缺少 ${key}`).toBe('string')
      expect(fm.data[key] as string).toBeTruthy()
    }
    for (const key of REQUIRED_GROUPS) {
      expect(fm.data[key], `frontmatter 缺少顶层组 ${key}`).toBeTypeOf('object')
      expect(
        Object.keys(fm.data[key] as object).length,
        `frontmatter 的 ${key} 组为空`,
      ).toBeGreaterThan(0)
    }
  })

  it('顶层键只包含 Stitch schema 允许的 5 组 + name/description', () => {
    const extra = Object.keys(fm.data).filter((k) => !ALLOWED_TOP_KEYS.has(k))
    expect(
      extra,
      `frontmatter 出现 Stitch schema 不接受的顶层键：${extra.join(', ')}。` +
        'motion / shadows / breakpoints 等必须放进 .impeccable/design.json 的 extensions。',
    ).toEqual([])
  })

  it('所有 {group.key} token 引用都能解析（无悬空引用）', () => {
    for (const group of ['colors', 'rounded', 'spacing', 'typography'] as const) {
      const defined = new Set(Object.keys((fm.data[group] as object) ?? {}))
      const refs = new Set(
        [...fm.raw.matchAll(new RegExp(`\\{${group}\\.([\\w-]+)\\}`, 'g'))].map((m) => m[1]),
      )
      const dangling = [...refs].filter((r) => !defined.has(r))
      expect(dangling, `${group} 组存在悬空引用：${dangling.join(', ')}`).toEqual([])
    }
  })

  it('每个色键都与 CSS token 真实值相符（数据驱动，全量）', () => {
    const mismatches: string[] = []
    for (const [key, { token, scope }] of Object.entries(COLOR_TO_TOKEN)) {
      const expected =
        scope === 'dark'
          ? cssToken(scopes.dark, token)
          : scope === 'tokens-dark'
            ? cssToken(designTokensCss.slice(designTokensCss.indexOf('.dark')), token)
            : cssToken(scopes.light, token) ?? cssToken(designTokensCss, token)
      if (!expected) {
        mismatches.push(`${key}: CSS 里找不到 --${token}`)
        continue
      }
      const actual = colors[key]
      if (!actual) {
        mismatches.push(`${key}: frontmatter 缺该键`)
        continue
      }
      if (normColor(actual) !== normColor(expected)) {
        mismatches.push(`${key}: frontmatter=${actual} 但 --${token}=${expected}`)
      }
    }
    expect(mismatches, `色键与 CSS 不符：\n${mismatches.join('\n')}`).toEqual([])
  })

  it('没有未登记的色键（新增色键必须进 COLOR_TO_TOKEN 才被校验）', () => {
    const uncharted = Object.keys(colors).filter((k) => !(k in COLOR_TO_TOKEN))
    expect(
      uncharted,
      `以下色键未纳入 COLOR_TO_TOKEN 映射，等于绕过了取值校验：${uncharted.join(', ')}`,
    ).toEqual([])
  })

  it('组件绑定：每个组件引用的色键正确，且该色键的值等于对应 CSS token', () => {
    const problems: string[] = []
    for (const [name, expected] of Object.entries(COMPONENT_BINDING)) {
      const comp = components[name]
      if (!comp) {
        problems.push(`${name}: frontmatter 缺该组件`)
        continue
      }
      for (const prop of ['backgroundColor', 'textColor'] as const) {
        const wantKey = expected[prop]
        if (!wantKey) continue
        const ref = comp[prop] ?? ''
        const gotKey = /\{colors\.([\w-]+)\}/.exec(ref)?.[1]
        if (gotKey !== wantKey) {
          problems.push(`${name}.${prop}: 期望引用 {colors.${wantKey}}，实际 ${ref || '（缺失）'}`)
        }
      }
    }
    expect(problems, `组件绑错颜色：\n${problems.join('\n')}`).toEqual([])
  })


  it('typography 的字号/字重/行高与 token 相等（数据驱动）', () => {
    const typography = fm.data.typography as Record<string, Record<string, unknown>>
    const problems: string[] = []
    for (const [key, mapping] of Object.entries(TYPE_TO_PREFIX)) {
      const entry = typography[key]
      if (!entry) {
        problems.push(`${key}: frontmatter 缺该条`)
        continue
      }
      const pairs: Array<[TypeProp, string]> = [
        ['fontSize', mapping.fontSize],
        ['fontWeight', mapping.fontWeight],
        ['lineHeight', mapping.lineHeight],
        ['fontFamily', mapping.fontFamily],
      ]
      for (const [prop, token] of pairs) {
        const raw = cssToken(designTokensCss, token)
        if (!raw) {
          problems.push(`${key}.${prop}: design-tokens.css 找不到 --${token}`)
          continue
        }
        // font-family 常写成 var(--font-family-x) 别名，需解析别名链后再比
        const want = prop === 'fontFamily' ? resolveVar(raw, designTokensCss) : raw
        // YAML 双引号串里的 \" 与 CSS 里的裸 " 语义等价，比较前归一
        const norm = (s: string) => s.replace(/\\"/g, '"').replace(/\s+/g, ' ').trim()
        if (norm(String(entry[prop])) !== norm(want)) {
          problems.push(`${key}.${prop}: frontmatter=${String(entry[prop])} 但 --${token}=${want}`)
        }
      }
    }
    expect(problems, `typography 与 token 不符：\n${problems.join('\n')}`).toEqual([])
  })

  it('没有未登记的 typography 键（新增条目必须进 TYPE_TO_PREFIX）', () => {
    const typography = fm.data.typography as Record<string, unknown>
    const uncharted = Object.keys(typography).filter((k) => !(k in TYPE_TO_PREFIX))
    expect(
      uncharted,
      `以下 typography 键未纳入 TYPE_TO_PREFIX，等于绕过校验：${uncharted.join(', ')}`,
    ).toEqual([])
  })

  it('组件的 height / padding 与真实实现相符（数据驱动）', () => {
    const problems: string[] = []
    for (const [name, expected] of Object.entries(COMPONENT_SIZE)) {
      const comp = components[name]
      if (!comp) {
        problems.push(`${name}: frontmatter 缺该组件`)
        continue
      }
      for (const prop of ['height', 'padding'] as const) {
        const want = expected[prop]
        if (want === undefined) continue
        if (comp[prop] !== want) {
          problems.push(`${name}.${prop}: frontmatter=${comp[prop] ?? '（缺失）'}，应为 ${want}`)
        }
      }
    }
    expect(problems, `组件尺寸不符：\n${problems.join('\n')}`).toEqual([])
  })

  it('深色档：card 与 panel 不得同值（--bg-base-secondary ≠ --bg-card）', () => {
    const darkBaseSecondary = cssToken(scopes.dark, 'bg-base-secondary')
    const darkCard = cssToken(scopes.dark, 'bg-card')
    expect(darkBaseSecondary).toBeTruthy()
    expect(darkCard).toBeTruthy()
    // 前提断言：CSS 里这两个值确实不同（若将来统一了，本 spec 应随之删除该断言）
    expect(normColor(darkBaseSecondary)).not.toBe(normColor(darkCard))
    expect(normColor(colors['surface-card-dark'])).toBe(normColor(darkBaseSecondary))
    expect(normColor(colors['surface-panel-dark'])).toBe(normColor(darkCard))
  })

  it('状态色族数量为 5（success/alert/warning/error/primary）', () => {
    for (const f of ['success', 'alert', 'warning', 'error', 'primary']) {
      expect(
        cssToken(designTokensCss, `status-${f}-default`),
        `design-tokens.css 缺 --status-${f}-default`,
      ).toBeTruthy()
    }
    const all = [...designTokensCss.matchAll(/--status-([a-z]+)-default:/g)].map((m) => m[1])
    expect(new Set(all).size, `实际 status 族：${[...new Set(all)].join(', ')}`).toBe(5)
  })

  it('涨跌色红涨绿跌（--color-rise 偏红、--color-fall 偏绿）', () => {
    const rise = cssToken(designTokensCss, 'color-rise')
    const fall = cssToken(designTokensCss, 'color-fall')
    expect(rise).toBeTruthy()
    expect(fall).toBeTruthy()
    const rgb = (h: string) => {
      const s = h.replace('#', '')
      return [0, 2, 4].map((i) => parseInt(s.slice(i, i + 2), 16))
    }
    const [rr, rg, rb] = rgb(rise!)
    expect(rr, `--color-rise=${rise} 必须是红色系（红涨）`).toBeGreaterThan(rg + 30)
    expect(rr).toBeGreaterThan(rb + 30)
    const [fr, fg] = rgb(fall!)
    expect(fg, `--color-fall=${fall} 必须是绿色系（绿跌）`).toBeGreaterThan(fr + 30)
  })

  it('spacing / rounded 的值与 token 文件逐值相等（不只校验存在）', () => {
    const rounded = fm.data.rounded as Record<string, string>
    const spacing = fm.data.spacing as Record<string, string>
    const problems: string[] = []

    for (const [key, val] of Object.entries(rounded)) {
      const token = cssToken(designTokensCss, `radius-${key}`)
      if (!token) problems.push(`rounded.${key}: 无 --radius-${key}`)
      else if (token !== val) problems.push(`rounded.${key}: frontmatter=${val} 但 --radius-${key}=${token}`)
    }
    for (const [key, val] of Object.entries(spacing)) {
      const token = cssToken(designTokensCss, `spacer-${key}`)
      if (!token) problems.push(`spacing.${key}: 无 --spacer-${key}`)
      else if (token !== val) problems.push(`spacing.${key}: frontmatter=${val} 但 --spacer-${key}=${token}`)
    }
    expect(problems, `rounded/spacing 值不符：\n${problems.join('\n')}`).toEqual([])
  })
})
