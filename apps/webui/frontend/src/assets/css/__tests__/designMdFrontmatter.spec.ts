import { describe, it, expect } from 'vitest'
import { readFileSync, existsSync } from 'node:fs'
import { join } from 'node:path'

// 根 DESIGN.md frontmatter 的真实性回归（#356 审查收敛 S-6）
//
// 为什么需要：根 `DESIGN.md` 的 YAML frontmatter 是**机器可读的设计 token 源**，
// Impeccable 之类的设计工具会直接把它当真值生成新 UI。它是
// `apps/webui/frontend/src/assets/css/design-tokens.css` + `theme.css` 的镜像，
// 但**镜像没有机械保证**——审查时实测发现同一份 frontmatter 里：
//   - button-primary 背景标成 `#171717`，实际是 `--bg-invert`（浅 `#262626` / 深 `#e5e5e5`）
//   - button-secondary / input 标成纯白，实际是 `--bg-overlay-l1`（8% 灰透明）
//   - card 与 panel 深色档混用同一个值，实际 `--bg-base-secondary`(`#262626`)
//     与 `--bg-card`(`#202020`) 是两个不同值
// 这类错误**不会被任何现有门禁拦下**（design-tokens.css 有 themeTokens/tokenContrast
// 守着，frontmatter 一条都没有），而一旦被下游当真值消费就会静默生成错色界面。
//
// 本 spec 只校验"能机械判定"的部分，不追求覆盖 frontmatter 的全部语义：
//   1. 文件存在且 frontmatter 可解析；
//   2. 顶层键只允许 Stitch schema 的 5 组 + name/description（多出的键 Stitch 会拒收）；
//   3. 所有 `{colors.X}` / `{rounded.X}` / `{spacing.X}` 引用都能在对应组内解析
//      （悬空引用在面板渲染时是静默空值）；
//   4. 关键取值与真实 token 文件一致——不是硬编码期望值，而是**回查 CSS 源文件**，
//      所以改 token 时本 spec 会跟着要求 frontmatter 一起改。
//
// 边界（有意不做）：不校验描述性文字、不校验 sidecar（那需要浏览器渲染，成本高
// 且已由人工渲染验证）。新增 token 组时必须同步本 spec。

// 仓库根 = 本 spec 所在的 src/assets/css/__tests__ 往上 6 层
//   __tests__ → css → assets → src → frontend → webui → apps → <root>
const ROOT = join(__dirname, '../../../../../../..')
const DESIGN_MD = join(ROOT, 'DESIGN.md')
const DESIGN_TOKENS_CSS = join(__dirname, '../design-tokens.css')
const THEME_CSS = join(__dirname, '../theme.css')

const ALLOWED_TOP_KEYS = new Set([
  'name', 'description', 'colors', 'typography', 'rounded', 'spacing', 'components',
])

/** 极简 YAML 读取：只支持本项目 frontmatter 用到的「缩进映射 + 标量」两三种形态。
 *  引入 yaml 依赖只为这一处不值当，且失败模式明确（解析不出就报错）。 */
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
    // 键可能是带引号的数字（rounded / spacing 用 `"8": "8px"` 形态），去引号后再入表，
    // 否则 frontmatter 内 `{rounded.8}` 的引用会被判成悬空。
    const key = km[1].replace(/^["']|["']$/g, '')
    const rest = km[2]
    while (stack.length > 1 && stack[stack.length - 1].indent >= indent) stack.pop()
    const parent = stack[stack.length - 1].node
    if (rest === '') {
      const child: Record<string, unknown> = {}
      parent[key] = child
      stack.push({ indent, node: child })
    } else {
      // 去引号（保留内部内容原样）
      let v: unknown = rest
      const q = rest.match(/^"(.*)"$/) || rest.match(/^'(.*)'$/)
      if (q) v = q[1]
      else if (/^-?\d+(\.\d+)?$/.test(rest)) v = Number(rest)
      parent[key] = v
    }
  }
  return { raw, data }
}

/** 从 CSS 源码取某个自定义属性的值（取第一个匹配，浅色 :root 优先）。
 *  注意 CSS 里的 token 声明常带缩进，故不能用 `^` 锚行首。 */
function cssToken(css: string, name: string): string | undefined {
  const re = new RegExp(`--${name}:\\s*([^;]+);`)
  return re.exec(css)?.[1].trim()
}

/**
 * 按「真实规则块」切段，不能用 `indexOf('.dark')` —— theme.css 的头部说明注释里就
 * 出现了 `.dark` 字样（讲主题机制），按字符串切会切到注释中间导致整段取值落空。
 * 这里定位**行首**的 `.dark {` 规则起始。
 */
function themeScopes(css: string): { light: string; dark: string } {
  const darkRule = /^\.dark\s*\{/m.exec(css)
  if (!darkRule) throw new Error('theme.css 找不到行首 .dark 规则块')
  return { light: css.slice(0, darkRule.index), dark: css.slice(darkRule.index) }
}

const designMd = existsSync(DESIGN_MD) ? readFileSync(DESIGN_MD, 'utf8') : ''
const designTokensCss = readFileSync(DESIGN_TOKENS_CSS, 'utf8')
const themeCss = readFileSync(THEME_CSS, 'utf8')
const scopes = themeScopes(themeCss)

describe('DESIGN.md frontmatter 与 token 源真实一致（#356）', () => {
  it('文件存在且 frontmatter 可解析', () => {
    expect(existsSync(DESIGN_MD), `${DESIGN_MD} 不存在`).toBe(true)
    const { data } = parseFrontmatter(designMd)
    expect(data.name).toBeTruthy()
    expect(data.colors).toBeTypeOf('object')
  })

  it('顶层键只包含 Stitch schema 允许的 5 组 + name/description', () => {
    const { data } = parseFrontmatter(designMd)
    const extra = Object.keys(data).filter((k) => !ALLOWED_TOP_KEYS.has(k))
    expect(
      extra,
      `frontmatter 出现 Stitch schema 不接受的顶层键：${extra.join(', ')}。` +
        'motion / shadows / breakpoints 等必须放进 .impeccable/design.json 的 extensions。',
    ).toEqual([])
  })

  it('所有 {group.key} token 引用都能解析（无悬空引用）', () => {
    const { raw, data } = parseFrontmatter(designMd)
    for (const group of ['colors', 'rounded', 'spacing', 'typography'] as const) {
      const defined = new Set(Object.keys((data[group] as object) ?? {}))
      const refs = new Set(
        [...raw.matchAll(new RegExp(`\\{${group}\\.([\\w-]+)\\}`, 'g'))].map((m) => m[1]),
      )
      const dangling = [...refs].filter((r) => !defined.has(r))
      expect(dangling, `${group} 组存在悬空引用：${dangling.join(', ')}`).toEqual([])
    }
  })

  it('真实取值回查：invert / overlay / card / panel 与 CSS token 一致', () => {
    const { data } = parseFrontmatter(designMd)
    const colors = data.colors as Record<string, string>

    const lightScope = scopes.light
    const bgInvert = cssToken(lightScope, 'bg-invert')
    const overlayL1 = cssToken(lightScope, 'bg-overlay-l1')
    const overlayL2 = cssToken(lightScope, 'bg-overlay-l2')
    const bgCard = cssToken(lightScope, 'bg-card')
    const bgBaseSecondary = cssToken(lightScope, 'bg-base-secondary')

    expect(bgInvert, 'theme.css 浅色段缺 --bg-invert').toBeTruthy()
    expect(overlayL1).toBeTruthy()
    expect(overlayL2).toBeTruthy()
    expect(bgCard).toBeTruthy()
    expect(bgBaseSecondary).toBeTruthy()

    // button-primary 的背景必须是 --bg-invert，不是 ink-strongest
    expect(colors['invert-light']?.toUpperCase()).toBe(bgInvert!.toUpperCase())
    // secondary 按钮与 input 的地色是 overlay-l1 / overlay-l2，不是纯白
    expect(colors['overlay-l1']).toBe(overlayL1)
    expect(colors['overlay-l2']).toBe(overlayL2)
    // card 走 --bg-base-secondary，panel 走 --bg-card，二者深色不同值
    expect(colors['surface-card-light']?.toUpperCase()).toBe(bgBaseSecondary!.toUpperCase())
    expect(colors['surface-panel-light']?.toUpperCase()).toBe(bgCard!.toUpperCase())
  })

  it('深色档：card 与 panel 不得同值（--bg-base-secondary ≠ --bg-card）', () => {
    const { data } = parseFrontmatter(designMd)
    const colors = data.colors as Record<string, string>

    const darkScope = scopes.dark
    const darkBaseSecondary = cssToken(darkScope, 'bg-base-secondary')
    const darkCard = cssToken(darkScope, 'bg-card')

    expect(darkBaseSecondary).toBeTruthy()
    expect(darkCard).toBeTruthy()
    // 前提断言：CSS 里这两个值确实不同（若将来统一了，本 spec 应随之删除该断言）
    expect(darkBaseSecondary!.toUpperCase()).not.toBe(darkCard!.toUpperCase())

    expect(colors['surface-card-dark']?.toUpperCase()).toBe(darkBaseSecondary!.toUpperCase())
    expect(colors['surface-panel-dark']?.toUpperCase()).toBe(darkCard!.toUpperCase())
  })

  it('状态色族数量为 5（success/alert/warning/error/primary）', () => {
    const families = ['success', 'alert', 'warning', 'error', 'primary']
    for (const f of families) {
      expect(
        cssToken(designTokensCss, `status-${f}-default`),
        `design-tokens.css 缺 --status-${f}-default`,
      ).toBeTruthy()
    }
    // 反向：不应存在第 6 个 status-*-default 族而不被本文档记录
    const all = [...designTokensCss.matchAll(/--status-([a-z]+)-default:/g)].map((m) => m[1])
    expect(new Set(all).size, `实际 status 族：${[...new Set(all)].join(', ')}`).toBe(5)
  })

  it('涨跌色红涨绿跌（--color-rise 偏红、--color-fall 偏绿）', () => {
    const rise = cssToken(designTokensCss, 'color-rise')
    const fall = cssToken(designTokensCss, 'color-fall')
    expect(rise).toBeTruthy()
    expect(fall).toBeTruthy()
    // rise 的 R 通道必须显著大于 G/B（红），fall 的 G 通道必须显著大于 R（绿）
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

  it('spacing / rounded 的键在 token 文件中都有同号定义（防名字漂移）', () => {
    const { data } = parseFrontmatter(designMd)
    const rounded = data.rounded as Record<string, string>
    const spacing = data.spacing as Record<string, string>

    for (const key of Object.keys(rounded)) {
      expect(
        cssToken(designTokensCss, `radius-${key}`),
        `rounded.${key} 在 design-tokens.css 无 --radius-${key}`,
      ).toBeTruthy()
    }
    for (const key of Object.keys(spacing)) {
      expect(
        cssToken(designTokensCss, `spacer-${key}`),
        `spacing.${key} 在 design-tokens.css 无 --spacer-${key}`,
      ).toBeTruthy()
    }
  })
})
