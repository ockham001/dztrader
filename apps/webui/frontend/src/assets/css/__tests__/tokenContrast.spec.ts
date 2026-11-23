import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Root } from 'postcss'
import { parseCssFile, tokenDeclarationsOf } from '@/test-utils/cssTheme'

// 主题验收门禁（#348，母票 #345 T0-c）
//
// 为什么需要：`themeTokens.spec.ts` 验证的是 token 的**结构**（三段对称、别名目标存在），
// 不验证**可读性**。取证实测的 4 条缺陷全部从它眼皮底下通过 —— 两个 token 都完整定义在
// 三段里（结构对称 ✅），但同值（#525252 on #525252 = 1.00:1，文字完全消失）。
//
// 本 spec 两条**可读性**不变量（纯静态分析，不起浏览器，与 themeTokens.spec.ts 同一做法）：
//   检查 1：显式声明的前景/背景 token 对，在浅色 / 深色两套主题下都要达到各自的最低对比度。
//           **不是**全自动笛卡尔积 —— 语义搭配只有人知道（见 CONTRAST_PAIRS）。
//   检查 2：`var(--x)` 引用的 token 必须真的被某个 CSS 定义（或登记在运行时注入白名单）。
//
// HEAD 上确实存在不达标的 token 对。门禁不能因为「现状有缺陷」就恒红（那样第一天就会被
// 跳过），也不能靠不断放宽阈值来「变绿」。所以基线化：
//   - 每个不达标对**逐条登记**在 KNOWN_VIOLATIONS（带 ticket 与 why），并 pin 住实测比值；
//   - 实测集合必须与登记集合**完全相等**：新缺陷 → 红；缺陷被修复而登记没删 → 也红。
//     基线只会收缩，不会静默腐化。
//   - 「门禁不是恒绿」由自证用例保证：把取证时的缺陷字面值喂给同一个纯函数
//     `evaluateContrast`，断言它报出的正是那几条、且比值与取证一致（#309 恒真断言防范）。
//
// 关键设计：**本文件里没有任何一条用例断言「HEAD 现在有缺陷」**。那种用例是一次性脚手架
// ——它会在修复票（#349）落地时变红，逼着人回来删用例，而删用例恰恰是门禁被削弱的常见
// 起点。取而代之的是两条永久不变量：
//   (a)「实测不达标集合 == 基线」：评估逻辑一旦失效返回空集，全部基线立刻「过期」→ 红；
//   (b)「取证缺陷快照必须报红」：把当年 buggy 的字面 token 喂进同一个纯函数，断言它抓出
//      的正是取证点名的那两条、且比值对得上。
// 修好之后的动作固定为两步：把对应 pair **改指**修复后的 token（若拆了新 token），
// 再删掉该缺陷的 KNOWN_VIOLATIONS 登记。#349 就是这么做的；自证用例只做加强、不削弱。
//
// disabled 态的豁免口径（#345 拍板）：WCAG 明确豁免 disabled 控件，但本次缺陷是
// **1.00:1** —— 文字与底色完全相同，视觉上是一个无字灰块。豁免条款是给「可辨识但不可用」
// 的，不覆盖「完全消失」。所以项目口径：disabled 文字仍要求 ≥1.5:1。
// 可复算依据（不依赖任何 gitignore 截图）：components.css 的 `.ds-btn--primary[disabled]`
// 同时消费 --bg-invert-disabled（底）与 --text-disabled（字），深色段两者都是 #525252。

const CSS_DIR = join(fileURLToPath(import.meta.url), '..', '..')
const SRC_DIR = join(CSS_DIR, '..', '..')

const SKIP_DIRS = new Set(['node_modules', 'dist', '__tests__'])

function collectFiles(dir: string, suffix: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (SKIP_DIRS.has(name)) continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) collectFiles(full, suffix, out)
    else if (extname(name) === suffix) out.push(full)
  }
  return out
}

const cssFiles = collectFiles(SRC_DIR, '.css')
const vueFiles = collectFiles(SRC_DIR, '.vue')

const themePath = join(CSS_DIR, 'theme.css')
const designTokensPath = join(CSS_DIR, 'design-tokens.css')

function relPath(file: string): string {
  return file.slice(SRC_DIR.length + 1).split('\\').join('/')
}

function parseCss(text: string, from: string): Root {
  try {
    return postcss.parse(text, { from })
  } catch (err) {
    throw new Error(`${from} 无法被标准 CSS 解析器解析：${(err as Error).message}`)
  }
}

// ---------------------------------------------------------------------------
// 三段主题的 token 作用域
// ---------------------------------------------------------------------------

type ThemeName = 'light' | 'dark' | 'system'
const THEMES: readonly ThemeName[] = ['light', 'dark', 'system'] as const
type TokenScope = Map<string, string>

const themeRoot = parseCssFile(themePath)
const lightTokens = tokenDeclarationsOf(themeRoot, ':root', false)
const darkTokens = tokenDeclarationsOf(themeRoot, '.dark', false)
const systemTokens = tokenDeclarationsOf(themeRoot, 'html:not(.light):not(.dark)', true)

// design-tokens.css 定义非主题色 token（radius/spacer/font/status/brand…），三段共用。
// theme.css 在它之后加载（src/main.ts 的导入顺序），同名 token 主题段胜 —— 合并顺序照此。
const designTokens = tokenDeclarationsOf(parseCssFile(designTokensPath), ':root', false)

function scopeOf(themeSegment: TokenScope): TokenScope {
  return new Map([...designTokens, ...themeSegment])
}

const SCOPES: Record<ThemeName, TokenScope> = {
  light: scopeOf(lightTokens),
  dark: scopeOf(darkTokens),
  system: scopeOf(systemTokens),
}

// ---------------------------------------------------------------------------
// 颜色解析 / alpha 合成 / WCAG 相对亮度
// ---------------------------------------------------------------------------

type Rgba = [number, number, number, number]

function expandHexDigit(ch: string): number {
  return Number.parseInt(ch + ch, 16)
}

function parseHex(hex: string): Rgba | null {
  switch (hex.length) {
    case 3:
      return [expandHexDigit(hex[0]), expandHexDigit(hex[1]), expandHexDigit(hex[2]), 1]
    case 4:
      return [
        expandHexDigit(hex[0]),
        expandHexDigit(hex[1]),
        expandHexDigit(hex[2]),
        expandHexDigit(hex[3]) / 255,
      ]
    case 6:
      return [
        Number.parseInt(hex.slice(0, 2), 16),
        Number.parseInt(hex.slice(2, 4), 16),
        Number.parseInt(hex.slice(4, 6), 16),
        1,
      ]
    case 8:
      return [
        Number.parseInt(hex.slice(0, 2), 16),
        Number.parseInt(hex.slice(2, 4), 16),
        Number.parseInt(hex.slice(4, 6), 16),
        Number.parseInt(hex.slice(6, 8), 16) / 255,
      ]
    default:
      return null
  }
}

const RGB_RE = /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)$/

/** 解析 token 的字面色值：只接受本项目 token 实际使用的形态（hex / rgb / rgba）。 */
function parseColor(value: string): Rgba | null {
  const text = value.trim()
  if (text.startsWith('#')) return parseHex(text.slice(1))
  const m = RGB_RE.exec(text)
  if (!m) return null
  return [Number(m[1]), Number(m[2]), Number(m[3]), m[4] === undefined ? 1 : Number(m[4])]
}

const VAR_REDIRECT_RE = /^var\(\s*(--[a-z0-9-]+)\s*(?:,\s*(.+))?\)$/

/** 解析 `var()` 别名链，返回终值（字面色）。无法解析返回 null（调用方必须显式处理）。 */
function resolveTokenValue(name: string, scope: TokenScope, seen = new Set<string>()): Rgba | null {
  if (seen.has(name)) return null // 别名环
  seen.add(name)
  const raw = scope.get(name)
  if (raw === undefined) return null
  const redirect = VAR_REDIRECT_RE.exec(raw)
  if (redirect) {
    const target = resolveTokenValue(redirect[1], scope, seen)
    if (target !== null) return target
    return redirect[2] === undefined ? null : parseColor(redirect[2])
  }
  return parseColor(raw)
}

function compositeOver(fg: Rgba, bg: Rgba): Rgba {
  const a = fg[3]
  return [fg[0] * a + bg[0] * (1 - a), fg[1] * a + bg[1] * (1 - a), fg[2] * a + bg[2] * (1 - a), 1]
}

function channelLuminance(c: number): number {
  const s = c / 255
  return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4
}

function relativeLuminance(color: Rgba): number {
  return (
    0.2126 * channelLuminance(color[0]) +
    0.7152 * channelLuminance(color[1]) +
    0.0722 * channelLuminance(color[2])
  )
}

/** WCAG 2.x 对比度：(亮 + 0.05) / (暗 + 0.05) */
function contrastRatio(a: Rgba, b: Rgba): number {
  const la = relativeLuminance(a)
  const lb = relativeLuminance(b)
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
}

// ---------------------------------------------------------------------------
// 检查 1：token 对对比度
// ---------------------------------------------------------------------------

interface ContrastPair {
  /** 稳定 id：出现在失败信息与 KNOWN_VIOLATIONS 里 */
  id: string
  /** 用途（人读；说明这对为什么该达标） */
  usage: string
  /** 前景 token */
  fg: string
  /** 背景 token；带 alpha 时合成到 base 上 */
  bg: string
  /** 合成基准（bg 带 alpha 时的实际底色） */
  base: string
  /** 最低对比度 */
  min: number
}

/**
 * 显式声明的前景/背景搭配清单。
 *
 * 为什么不是笛卡尔积：并不是任意两个 token 会同屏（`--text-onbrand` 只坐在 `--bg-brand`
 * 上，不会坐在 `--bg-card` 上）。全组合会产出大量现实中不存在的搭配，逼出一堆假豁免把
 * 门禁稀释掉。语义搭配只有人知道，所以手写。
 *
 * 新增会承载文字的 token 时，在这里加一行 —— 否则新 token 没有可读性护栏。
 */
const CONTRAST_PAIRS: readonly ContrastPair[] = [
  // 正文与次级文字
  { id: 'body-on-base', usage: '正文文字 on 页面底色', fg: '--text-default', bg: '--bg-base-default', base: '--bg-base-default', min: 4.5 },
  { id: 'body-on-card', usage: '正文文字 on 卡片', fg: '--text-default', bg: '--bg-card', base: '--bg-card', min: 4.5 },
  { id: 'secondary-on-base', usage: '次级文字 on 页面底色', fg: '--text-secondary', bg: '--bg-base-default', base: '--bg-base-default', min: 4.5 },
  { id: 'secondary-on-card', usage: '次级文字 on 卡片', fg: '--text-secondary', bg: '--bg-card', base: '--bg-card', min: 4.5 },
  { id: 'tertiary-on-base', usage: '三级文字 on 页面底色', fg: '--text-tertiary', bg: '--bg-base-default', base: '--bg-base-default', min: 4.5 },
  { id: 'tertiary-on-card', usage: '三级文字 on 卡片（密集表格的次级信息）', fg: '--text-tertiary', bg: '--bg-card', base: '--bg-card', min: 4.5 },

  // 按钮 / 品牌
  { id: 'primary-button-label', usage: 'primary 按钮文字 on 反色底', fg: '--bg-base-default', bg: '--bg-invert', base: '--bg-base-default', min: 4.5 },
  { id: 'onbrand-on-brand', usage: '品牌底上的文字', fg: '--text-onbrand', bg: '--bg-brand', base: '--bg-base-default', min: 4.5 },
  { id: 'brand-text-on-base', usage: '品牌色文字（链接/强调）on 页面底色', fg: '--text-brand', bg: '--bg-base-default', base: '--bg-base-default', min: 4.5 },

  // disabled 态（项目口径 ≥1.5:1，见文件头）
  { id: 'disabled-text-on-base', usage: 'disabled 控件文字 on 页面底色', fg: '--text-disabled', bg: '--bg-base-default', base: '--bg-base-default', min: 1.5 },
  // #349：disabled 按钮在反色底上需要**独立的**文字 token（深色段 --text-disabled 与
  // --bg-invert-disabled 同值 1.00:1；浅色段 --text-disabled 在 #525252 的 20% alpha 底上
  // 只有 2.01:1）。拆开建模后目标 3:1（WCAG 对 disabled 的豁免不覆盖「完全消失」，
  // 但项目口径仍要求可辨识，见文件头）。
  { id: 'primary-button-disabled-label', usage: 'disabled primary 按钮文字 on 禁用底', fg: '--text-oninvert-disabled', bg: '--bg-invert-disabled', base: '--bg-base-default', min: 3 },

  // 状态标签族：components.css 的 `.ds-tag--*`「状态色文字 + 同色低透明底」
  { id: 'tag-brand', usage: '品牌标签（.ds-tag--brand）', fg: '--text-brand', bg: '--bg-brand-popup', base: '--bg-card', min: 4.5 },
  // #349：状态标签拆「文字」与「同色底」两档 —— 底继续用 -surface-l1，文字改用 -on-surface
  // （同色相 + 透明度在浅色段必然字太亮/底太浅：取证实测 2.46–3.08:1）。深色段同样按
  // 4.5:1 收敛（门禁在 HEAD 上测得 #15A877 4.43 / #E8463A 3.70）。
  { id: 'tag-success', usage: '成功标签（.ds-tag--success：运行中/全部成交）', fg: '--status-success-on-surface', bg: '--status-success-surface-l1', base: '--bg-card', min: 4.5 },
  { id: 'tag-warning', usage: '警告标签（.ds-tag--warning：开发中/演示）', fg: '--status-warning-on-surface', bg: '--status-warning-surface-l1', base: '--bg-card', min: 4.5 },
  { id: 'tag-danger', usage: '危险标签（.ds-tag--danger：废单/已停止）', fg: '--status-error-on-surface', bg: '--status-error-surface-l1', base: '--bg-card', min: 4.5 },
  { id: 'tag-partial', usage: '「部分成交」标签（面板内 .ds-tag--alert，蓝色，历史类名）', fg: '--status-primary-on-surface', bg: '--status-primary-surface-l1', base: '--bg-card', min: 4.5 },
  // #354：中性委托标签（无 ds-tag-- 修饰）的白叠底。原先这是 dashboard.css 里的裸写
  // `rgba(255,255,255,.08)`，按 #354 的纯 token 化要求收敛为 `--tag-muted-surface`（同值）。
  // 按 development-checklist A3「新增会承载文字的 token 要在 CONTRAST_PAIRS 加一行」，
  // 此处显式登记：它承载 `--text-tertiary`。实测不达标（浅 4.38 / 深 2.71），但**值与原
  // 裸写逐字相同，不是本票引入的回归**，故进 KNOWN_VIOLATIONS 而非改色（改色会违反
  // #354 AC3 视觉零变化；该缺陷的修法与 tertiary 族同源，归 #324 专项）。
  { id: 'tag-muted-on-card', usage: '中性委托标签文字（.ds-tag 无修饰）on 白叠底', fg: '--text-tertiary', bg: '--tag-muted-surface', base: '--bg-card', min: 4.5 },

  // 涨跌色（#349 收编进 token）：它们承载**表格里的数字**（11–12px 正文字号），按 4.5:1 要求。
  // 现状不达标（浅色 3.75 / 3.17、深色 rise 3.99）—— 达标必须重设涨跌色相，而本票 AC6
  // 要求「除既有 4 项外视觉零变化」、票面亦明确「不改涨跌色语义方向」，故先登记为已知项。
  { id: 'rise-on-card', usage: '涨（红）文字 on 卡片', fg: '--color-rise', bg: '--bg-card', base: '--bg-card', min: 4.5 },
  { id: 'fall-on-card', usage: '跌（绿）文字 on 卡片', fg: '--color-fall', bg: '--bg-card', base: '--bg-card', min: 4.5 },

  // 非文字分隔线：口径不是 WCAG 1.4.11 的 3:1（那会让整族 border token 变红，等于重做
  // 边框系统，不在本波范围），而是「必须与背景可区分」≥1.10 —— 1.00 等于线消失。
  // 这条钉的是**分隔线实际使用的 token**。#349 之前是 `--bg-base-secondary`（缺陷本体：
  // 浅色段与 --bg-card 同值 1.00:1）；#349 之后改用语义正确的 `--border-neutral-l1`。
  // 与下面的 `border-on-card` 数值相同属故意（一个是「分隔线用途」，一个是「描边用途」，
  // 各自独立登记，任一处改走别的 token 都会被单独立即发现）。
  { id: 'row-separator-on-card', usage: '表格行分隔线 on 卡片底', fg: '--border-neutral-l1', bg: '--bg-card', base: '--bg-card', min: 1.1 },
  { id: 'border-on-card', usage: '通用描边 on 卡片底（#349 的目标 token，不得与背景同值）', fg: '--border-neutral-l1', bg: '--bg-card', base: '--bg-card', min: 1.1 },
]

interface ContrastViolation {
  theme: ThemeName
  id: string
  ratio: number
  min: number
  usage: string
}

/**
 * 纯函数：给定作用域与 token 对清单，返回全部不达标项。
 *
 * 无副作用、不读文件 —— 自证用例把「取证时的缺陷快照」直接喂进来，证明门禁真的会红。
 * token 解析失败的对在这里跳过：由「每对 token 可解析」用例单独报（否则一次改名就能让
 * 这条检查静默失效）。
 */
function evaluateContrast(
  pairs: readonly ContrastPair[],
  scopes: Record<ThemeName, TokenScope>,
  themes: readonly ThemeName[] = ['light', 'dark'],
): ContrastViolation[] {
  const violations: ContrastViolation[] = []
  for (const theme of themes) {
    const scope = scopes[theme]
    for (const pair of pairs) {
      const fgRaw = resolveTokenValue(pair.fg, scope)
      const bgRaw = resolveTokenValue(pair.bg, scope)
      const base = resolveTokenValue(pair.base, scope)
      if (fgRaw === null || bgRaw === null || base === null) continue
      const bg = bgRaw[3] < 1 ? compositeOver(bgRaw, base) : bgRaw
      const fg = fgRaw[3] < 1 ? compositeOver(fgRaw, bg) : fgRaw
      const ratio = contrastRatio(fg, bg)
      if (ratio < pair.min) violations.push({ theme, id: pair.id, ratio, min: pair.min, usage: pair.usage })
    }
  }
  return violations
}

/**
 * HEAD 上已知不达标的项（基线）。键 = `${theme}/${pairId}`。
 *
 * 规则（由用例强制）：实测不达标集合必须与本表**完全相等** —— 多一项（新缺陷）红，
 * 少一项（修好了）也红。比值 pin 到两位小数：token 值一变就红，逼着确认「变好还是变坏」。
 * 每条必须有 ticket / why：谁负责、为什么现在不修。
 */
interface KnownViolation {
  ratio: number
  ticket: number | null
  why: string
}

const KNOWN_VIOLATIONS: Record<string, KnownViolation> = {
  // #349 已修并删除的登记（留档，便于回溯基线收缩过什么）：
  //   dark/primary-button-disabled-label 1.00 → 拆出 --text-oninvert-disabled（浅 3.34 / 深 3.48）
  //   light/row-separator-on-card 1.00、dark/… 1.08 → 分隔线改用 --border-neutral-l1（1.15/1.31）
  //   light/tag-{success,warning,danger} 2.52/2.46/3.08、dark/tag-{success,danger} 4.43/3.70
  //     → 拆出 --status-*-on-surface（浅 5.18–5.62，深 4.82–6.54）
  // 品牌标签：修法在 --bg-brand-popup 的透明度上，不属 #349「只改状态族」的范围，
  // 登记为已知项等后续票；若 #349 顺手修好，删掉这条登记即可。
  'dark/tag-brand': {
    ratio: 3.33,
    ticket: null,
    why: '深色段 #6a6fff on rgba(106,111,255,0.18) 合成底；修法在 --bg-brand-popup，不属 #349 状态标签族范围',
  },
  // --- 涨跌色（#349 收编进 token；色值本身未改，见 CONTRAST_PAIRS 处说明）---
  // 达标需重设涨跌色相（浅色要压暗到 6:1 才能两底色都过），那会把「红涨绿跌」的观感
  // 改掉 —— 属全站色彩重设计，不在 #349（纯 token 化 + 4 条缺陷）范围。基线可收缩：
  // 将来若做涨跌色重设，删掉这三条即可。
  'light/rise-on-card': { ratio: 3.75, ticket: null, why: '#d4543e on #F5F5F5；承载表格数字，需 4.5:1' },
  'light/fall-on-card': { ratio: 3.17, ticket: null, why: '#2a9d5c on #F5F5F5；同上' },
  'dark/rise-on-card': { ratio: 3.99, ticket: null, why: '#d4543e on #202020；同上' },

  // --- 三级文字（#324 第 16 条已拍板「记录现状不拆 token」）---
  // 影响全站 136 处，须独立专项；本波只把它变成机器可见的已知项，不在 #349 修复范围。
  'light/tertiary-on-card': {
    ratio: 4.35,
    ticket: 324,
    why: '#324 第 16 条已拍板：tertiary 影响全站 136 处，须独立专项（浅色差 0.15）',
  },
  'dark/tertiary-on-base': {
    ratio: 3.78,
    ticket: 324,
    why: '#324 第 16 条已拍板：tertiary 影响全站 136 处，须独立专项',
  },
  'dark/tertiary-on-card': {
    ratio: 3.44,
    ticket: 324,
    why: '#324 第 16 条已拍板：tertiary 影响全站 136 处，须独立专项',
  },

  // --- 中性委托标签底（#354 新增 token）---
  // `--tag-muted-surface` = rgba(255,255,255,.08)，与它替代的裸写**逐字同值**，所以这两条
  // 是既有缺陷被搬进 token 表后「现形」，不是 #354 引入的回归（#354 是纯 token 化，AC3
  // 要求视觉零变化）。根因与上面的 tertiary 族同源：`--text-tertiary` 在浅色差 0.12、
  // 深色差 1.79，修法（压暗 tertiary 或改白叠底）都会动观感，归 #324 专项。
  'light/tag-muted-on-card': {
    ratio: 4.38,
    ticket: 324,
    why: '白 8% 叠在 #F5F5F5 上承载 #737373；与 tertiary 族同源，须随 #324 一并收敛',
  },
  'dark/tag-muted-on-card': {
    ratio: 2.71,
    ticket: 324,
    why: '白 8% 叠在 #202020 上承载 #737373；深色差 1.79，同上',
  },
}

const keyOf = (v: { theme: ThemeName; id: string }): string => `${v.theme}/${v.id}`

describe('#348 检查 1：token 对对比度', () => {
  it('采集健全性：三段主题与 design-tokens 都解析出足够 token（防扫描根收窄后静默空转）', () => {
    expect(lightTokens.size).toBeGreaterThan(60)
    expect(darkTokens.size).toBeGreaterThan(50)
    expect(systemTokens.size).toBeGreaterThan(50)
    expect(designTokens.size).toBeGreaterThan(300)
    // 显式钉住本 spec 依赖的两个文件确实在扫描集里
    expect(cssFiles).toContain(themePath)
    expect(cssFiles).toContain(designTokensPath)
  })

  it('每对声明的 token 在三套主题下都可解析（改名/删 token 时必须回来改本 spec）', () => {
    const broken: string[] = []
    for (const theme of THEMES) {
      const scope = SCOPES[theme]
      for (const pair of CONTRAST_PAIRS) {
        for (const token of [pair.fg, pair.bg, pair.base]) {
          if (resolveTokenValue(token, scope) === null) broken.push(`${theme}/${pair.id}: 无法解析 ${token}`)
        }
      }
    }
    expect(broken, broken.join('\n')).toEqual([])
  })

  it('system 段与 .dark 段的结论一致（三段手工复制的漂移护栏）', () => {
    const darkIds = evaluateContrast(CONTRAST_PAIRS, SCOPES, ['dark']).map((v) => v.id)
    const systemIds = evaluateContrast(CONTRAST_PAIRS, SCOPES, ['system']).map((v) => v.id)
    expect(systemIds).toEqual(darkIds)
  })

  it('不达标项与 KNOWN_VIOLATIONS 基线完全一致（新缺陷红 / 修好了也红）', () => {
    const actual = evaluateContrast(CONTRAST_PAIRS, SCOPES)
    const problems: string[] = []
    for (const v of actual) {
      const key = keyOf(v)
      const known = KNOWN_VIOLATIONS[key]
      if (known === undefined) {
        problems.push(
          `新增不达标：${key} = ${v.ratio.toFixed(2)}:1（要求 ≥${v.min}）｜${v.usage}\n` +
            '  修好它，或在 KNOWN_VIOLATIONS 登记（必须写 ticket 与 why）',
        )
      } else if (Math.abs(known.ratio - v.ratio) > 0.01) {
        problems.push(
          `基线数值变化：${key} 基线 ${known.ratio.toFixed(2)}:1 → 实测 ${v.ratio.toFixed(2)}:1\n` +
            '  变好就删掉这条登记并同步对应票；变坏先查 token 改动',
        )
      }
    }
    for (const key of Object.keys(KNOWN_VIOLATIONS)) {
      if (!actual.some((v) => keyOf(v) === key)) {
        problems.push(`基线过期：${key} 现在已达标（或该对已不存在），请删掉这条登记`)
      }
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  // AC2 的载体：门禁必须能抓出取证点名的「完全不可见」缺陷。
  // 判据不是「HEAD 现在红」（那会与「修复后门禁必须绿」冲突），而是**同一个求值函数**对
  // 取证时的字面 token 值报红、且比值对得上。这样一来 #349 修好之后本用例依然成立
  // （历史事实不会因为 token 被修好而改变），不需要删任何用例。
  it('AC2 自证：缺陷快照会让门禁红，且比值与取证一致（防恒绿断言）', () => {
    // **合成回归探针**，不是「#345 取证字面值复刻」：这里把每个 pair 的 token 都填成
    // 缺陷态值 —— 黑色字（#525252）、与卡片同色的分隔线、同色相低透明的标签、
    // 与禁用底同值的禁用文字。其中分隔线（`--border-neutral-l1` 填 #F5F5F5）与
    // #349 新拆的 token 都是**合成**值（#345 取证时它们还不存在），目的是锁住
    // 「若有人把建模改回去，门禁必须报出同样的红与同样的比值」。
    const light: TokenScope = new Map([
      ['--bg-base-default', '#FFFFFF'],
      ['--bg-card', '#F5F5F5'],
      ['--bg-base-secondary', '#F5F5F5'], // 与 bg-card 同值 → 分隔线不可见
      ['--bg-invert', '#262626'],
      ['--bg-invert-disabled', 'rgba(115, 115, 115, 0.20)'],
      ['--text-disabled', '#A1A1A1'],
      ['--text-default', '#171717'],
      ['--text-secondary', '#404040'],
      ['--text-tertiary', '#737373'],
      ['--text-onbrand', '#FFFFFF'],
      ['--text-brand', '#4B3FE3'],
      ['--bg-brand', '#4B3FE3'],
      ['--bg-brand-popup', 'rgba(170, 183, 255, 0.36)'],
      ['--status-success-default', '#15A877'],
      ['--status-success-surface-l1', 'rgba(64, 176, 139, 0.12)'],
      ['--status-warning-default', '#E27900'],
      ['--status-warning-surface-l1', 'rgba(226, 121, 0, 0.12)'],
      ['--status-error-default', '#E8463A'],
      ['--status-error-surface-l1', 'rgba(232, 70, 58, 0.12)'],
      // 浅色缺陷态：边框 token 与卡片同色 → 分隔线不可见（1.00:1）
      ['--border-neutral-l1', '#F5F5F5'],
      // #349 拆出来的 token：这里填的仍是**缺陷态**的值（修复前的那套），
      // 用来证明「若有人把建模改回去，门禁会复现同样的红与同样的比值」。
      ['--text-oninvert-disabled', '#A1A1A1'],
      ['--status-success-on-surface', '#15A877'],
      ['--status-warning-on-surface', '#E27900'],
      ['--status-error-on-surface', '#E8463A'],
      ['--status-primary-on-surface', '#5b8def'],
      ['--status-primary-surface-l1', 'rgba(91, 141, 239, 0.15)'],
      // 涨跌色在缺陷快照里就是当前实现值（本票不改色值，只收编进 token）
      ['--color-rise', '#D4543E'],
      ['--color-fall', '#2A9D5C'],
    ])
    const dark: TokenScope = new Map([
      ...light,
      ['--bg-base-default', '#171717'],
      ['--bg-card', '#202020'],
      ['--bg-base-secondary', '#262626'],
      ['--bg-invert', '#e5e5e5'],
      ['--bg-invert-disabled', '#525252'], // 与 text-disabled 同值
      ['--text-disabled', '#525252'],
      ['--text-default', '#e5e5e5'],
      ['--text-secondary', '#a1a1a1'],
      ['--text-tertiary', '#737373'],
      ['--text-brand', '#6a6fff'],
      ['--bg-brand-popup', 'rgba(106, 111, 255, 0.18)'],
      // 深色缺陷态：边框 token 与卡片同色系（旧 #262626 on #202020 = 1.08）
      ['--border-neutral-l1', '#262626'],
      ['--text-oninvert-disabled', '#525252'],
      ['--status-success-on-surface', '#15A877'],
      ['--status-warning-on-surface', '#E27900'],
      ['--status-error-on-surface', '#E8463A'],
      ['--status-primary-on-surface', '#5b8def'],
      ['--status-primary-surface-l1', 'rgba(91, 141, 239, 0.15)'],
    ])
    const violations = evaluateContrast(CONTRAST_PAIRS, { light, dark, system: dark })
    const byKey = new Map(violations.map((v) => [keyOf(v), v.ratio]))

    // 缺陷 1、2：完全不可见 → 必须报红，且比值 ≈1.00（不是「报了但报错值」）
    // （比值来源：合成的缺陷态 token 值，见上方说明；与 #345 取证报告逐条一致）
    expect(byKey.get('dark/primary-button-disabled-label')).toBeCloseTo(1.0, 2)
    expect(byKey.get('light/row-separator-on-card')).toBeCloseTo(1.0, 2)
    expect(byKey.get('dark/row-separator-on-card')).toBeCloseTo(1.08, 1)
    // 缺陷 3：标签族低对比 → 必须报红，比值与取证报告同一数量级
    expect(byKey.get('light/tag-warning')).toBeCloseTo(2.46, 1)
    expect(byKey.get('light/tag-success')).toBeCloseTo(2.52, 1)
    // 缺陷 4（focus-visible 缺失）不属 token 层，由 #332 负责 —— 这里不假装覆盖。
  })

  it('自证：同一套 token 的健康快照必须一条不报（防「一律报红」的假门禁）', () => {
    const healthy: TokenScope = new Map([
      ['--bg-base-default', '#FFFFFF'],
      ['--bg-card', '#F5F5F5'],
      ['--bg-base-secondary', '#D4D4D4'],
      ['--bg-invert', '#262626'],
      ['--bg-invert-disabled', '#E5E5E5'],
      ['--text-disabled', '#6B6B6B'],
      ['--text-default', '#171717'],
      ['--text-secondary', '#404040'],
      ['--text-tertiary', '#4A4A4A'],
      ['--text-onbrand', '#FFFFFF'],
      ['--text-brand', '#3B31B8'],
      ['--bg-brand', '#4B3FE3'],
      ['--bg-brand-popup', '#E5EAFF'],
      ['--status-success-default', '#0B6B4B'],
      ['--status-success-surface-l1', '#E7F4EE'],
      ['--status-warning-default', '#7A4100'],
      ['--status-warning-surface-l1', '#FCEFE0'],
      ['--status-error-default', '#A31C10'],
      ['--status-error-surface-l1', '#FAE8E6'],
      ['--border-neutral-l1', '#D0D0D0'],
      // #349 新增 token 的健康值（让这些 pair 真正参与评估，而不是因解析不到被跳过）
      ['--text-oninvert-disabled', '#6B6B6B'],
      ['--status-success-on-surface', '#0B6B4B'],
      ['--status-warning-on-surface', '#7A4100'],
      ['--status-error-on-surface', '#A31C10'],
      ['--status-primary-on-surface', '#1D4FC4'],
      ['--status-primary-surface-l1', '#E3EBFC'],
      ['--color-rise', '#B03A28'],
      ['--color-fall', '#166B3E'],
    ])
    const violations = evaluateContrast(CONTRAST_PAIRS, { light: healthy, dark: healthy, system: healthy })
    expect(violations.map(keyOf)).toEqual([])
  })
})

// ---------------------------------------------------------------------------
// 检查 2：token 引用完整性（var(--x) 必须真的被定义）
// ---------------------------------------------------------------------------

/**
 * 运行时才存在的自定义属性（由 JS 通过 :style 注入，CSS 里不会有定义）。
 * 每条都必须注明注入点 —— 没有注入点的「运行时注入」是假豁免。
 */
const RUNTIME_INJECTED: Record<string, string> = {
  '--icon-url': 'components/shared/Icon.vue:11 与 Dropdown/DatePicker/TimePicker 等以 :style 注入图标 url',
  '--ds-status-fill': 'components/shared/StatusIndicator.vue:56 按进度注入填充百分比',
  '--core-fill': 'views/MonitorView.vue 的 coreBars computed 按每核占用率注入柱高（#435；原 coreBarStyle 已并入）——值逐核不同，只能运行时定',
  '--core-columns': 'views/MonitorView.vue 的每核条形区按容器实测宽度注入列数（#435）——宽度只能运行时测（monitorCpu.ts 的 coreBarColumns 算值）',
  '--volume-fill': 'views/MonitorView.vue 的 volumeFillStyle() 按每个卷的已用比例注入容量条填充宽度（#450）——逐卷不同，只能运行时定',
  '--disk-busy': 'views/MonitorView.vue 的 diskBusyStyle() 按每块盘的 utilization 注入忙碌条填充宽度（#450）——逐盘不同，只能运行时定',
  '--memory-seg': 'views/MonitorView.vue 的 memorySegmentStyle() 按内存组成带各段占比注入段宽（#363；#445 迁入内存卡）——逐段不同，只能运行时定',
  '--meter-fill': 'views/MonitorView.vue 的 meterFillStyle() 按交换使用率注入数字格细条宽度（#445 第四轮）——逐帧不同，只能运行时定',
}

/**
 * HEAD 上已知的悬空引用（基线）。键 = token 名，值 = 引用点清单 + 处置。
 * 与 KNOWN_VIOLATIONS 同一套语义：新悬空 → 红；修好了没删登记 → 也红。
 */
const KNOWN_DANGLING: Record<string, { files: string[]; ticket: number | null; why: string }> = {
  // #348 AC3 点名的 4 处（issue 正文逐个列出）。行号不稳定，基线以**文件**为粒度。
  '--brand-primary': {
    files: ['components/layout/Sidebar.vue'],
    ticket: null,
    why: '#349 未覆盖（原登记号已失效，改派待定）：「关于」弹窗 48px 标题色，无 fallback → 颜色退化为继承值（应改 --text-brand）',
  },
  '--bg-base-primary': {
    files: ['components/marketSources/FrontendTable.vue'],
    ticket: null,
    why: '#349 未覆盖（原登记号已失效，改派待定）：输入框 focus 背景；该 token 从未定义（疑为 --bg-base-default 笔误）',
  },
  '--border-focus-l1': {
    files: ['components/marketSources/FrontendTable.vue'],
    ticket: null,
    why: '#349 未覆盖（原登记号已失效，改派待定）：focus 边框色，靠 fallback 落到 --status-success-default（绿色焦点环，与全站不一致）',
  },
  '--icon-quaternary': {
    files: ['assets/css/dashboard.css'],
    ticket: null,
    why: '#349 未覆盖（原登记号已失效，改派待定）：滚动条滑块色，靠 fallback rgba(128,128,128,.4)；icon 体系只到 tertiary',
  },
  // 死代码形态：components.css 有一段 custom-color 覆盖块，但 StatusIndicator 从不注入
  // 这两个属性、也不加 .ds-status--custom-color 类（注释声称 JS 会加，实际无人加）。
  '--ds-color': {
    files: ['assets/css/components.css'],
    ticket: null,
    why: 'components.css 的 ds-status--custom-color 覆盖块未接线（StatusIndicator 不注入），属死代码，不在本波范围',
  },
  '--ds-color-bg': {
    files: ['assets/css/components.css'],
    ticket: null,
    why: '同上，custom-color 覆盖块的第二个变量',
  },
  // #349 已修并删除的登记：`--border-default`（topbar 分隔线）—— 该 token 从未定义，
  // 实际一直落到 fallback 的**背景** token；#349 把它直接改成 `--border-neutral-l1`，
  // 悬空引用随之消失（基线只能收缩，不能腐化）。
}

/** 从一段文本里收集 var(--x)，累积到 out（value → 引用点列表） */
function collectVarNames(text: string, out: Map<string, string[]>, where: string): void {
  const re = /var\(\s*(--[a-z0-9-]+)/g
  let m: RegExpExecArray | null
  while ((m = re.exec(text)) !== null) {
    const list = out.get(m[1]) ?? []
    list.push(where)
    out.set(m[1], list)
  }
}

/**
 * 去掉 JS/TS 注释（保留字符串内容）。用于 .vue 的 <script> 块：
 * 注释里提到 `var(--token)` 是文档，不是引用（如 SysmonHistoryChart.vue 的说明注释）。
 */
function stripJsComments(src: string): string {
  let out = ''
  let i = 0
  let quote: string | null = null
  while (i < src.length) {
    const ch = src[i]
    const next = src[i + 1]
    if (quote !== null) {
      out += ch
      if (ch === '\\') {
        out += next ?? ''
        i += 2
        continue
      }
      if (ch === quote) quote = null
      i += 1
      continue
    }
    if (ch === '"' || ch === "'" || ch === '`') {
      quote = ch
      out += ch
      i += 1
      continue
    }
    if (ch === '/' && next === '/') {
      while (i < src.length && src[i] !== '\n') i += 1
      continue
    }
    if (ch === '/' && next === '*') {
      i += 2
      while (i < src.length && !(src[i] === '*' && src[i + 1] === '/')) i += 1
      i += 2
      continue
    }
    out += ch
    i += 1
  }
  return out
}

/** .css：postcss 解析后只扫 declaration（块注释天然被跳过，不会产生假引用） */
function cssVarRefsOfCss(file: string): Map<string, string[]> {
  const out = new Map<string, string[]>()
  parseCss(readFileSync(file, 'utf8'), file).walkDecls((decl) => {
    collectVarNames(decl.value, out, `${relPath(file)}:${decl.source?.start?.line ?? '?'}`)
  })
  return out
}

/** .vue：<style> 交给 postcss；<template>/<script> 去注释后按行扫描（模板里的 :style 是真实引用） */
function cssVarRefsOfVue(file: string): Map<string, string[]> {
  const out = new Map<string, string[]>()
  const text = readFileSync(file, 'utf8')
  const rel = relPath(file)
  const lineAt = (index: number): number => text.slice(0, index).split('\n').length

  const styleRe = /<style[^>]*>([\s\S]*?)<\/style>/g
  let sm: RegExpExecArray | null
  while ((sm = styleRe.exec(text)) !== null) {
    const blockLine = lineAt(sm.index)
    parseCss(sm[1], file).walkDecls((decl) => {
      collectVarNames(decl.value, out, `${rel}:${blockLine + (decl.source?.start?.line ?? 1)}`)
    })
  }

  for (const tag of ['template', 'script']) {
    const re = new RegExp(`<${tag}[^>]*>([\\s\\S]*?)</${tag}>`, 'g')
    let m: RegExpExecArray | null
    while ((m = re.exec(text)) !== null) {
      const startLine = lineAt(m.index)
      const body =
        tag === 'script' ? stripJsComments(m[1]) : m[1].replace(/<!--[\s\S]*?-->/g, '')
      body.split('\n').forEach((line, idx) => {
        collectVarNames(line, out, `${rel}:${startLine + idx}`)
      })
    }
  }
  return out
}

function collectAllVarRefs(): Map<string, string[]> {
  const merged = new Map<string, string[]>()
  const add = (single: Map<string, string[]>): void => {
    for (const [token, refs] of single) {
      const list = merged.get(token) ?? []
      list.push(...refs)
      merged.set(token, list)
    }
  }
  for (const file of cssFiles) add(cssVarRefsOfCss(file))
  for (const file of vueFiles) add(cssVarRefsOfVue(file))
  return merged
}

/** 全部 CSS 里定义过的自定义属性（含 .vue 的 <style> 块与 :root） */
function collectDefinedTokens(): Set<string> {
  const defined = new Set<string>()
  const addFromRoot = (root: Root): void => {
    root.walkDecls((decl) => {
      if (decl.prop.startsWith('--')) defined.add(decl.prop.trim())
    })
  }
  for (const file of cssFiles) addFromRoot(parseCss(readFileSync(file, 'utf8'), file))
  for (const file of vueFiles) {
    const text = readFileSync(file, 'utf8')
    const styleRe = /<style[^>]*>([\s\S]*?)<\/style>/g
    let m: RegExpExecArray | null
    while ((m = styleRe.exec(text)) !== null) addFromRoot(parseCss(m[1], file))
  }
  return defined
}

const varRefs = collectAllVarRefs()
const definedTokens = collectDefinedTokens()

/** 悬空引用：被 var() 引用但没有任何 CSS 定义、且不在运行时注入白名单里 */
function danglingRefs(
  refs: Map<string, string[]>,
  defined: Set<string>,
  runtimeInjected: Record<string, string>,
): Map<string, string[]> {
  const out = new Map<string, string[]>()
  for (const [token, sites] of refs) {
    if (defined.has(token)) continue
    if (token in runtimeInjected) continue
    out.set(token, sites)
  }
  return out
}

/** 引用点字符串 `path:line` → 文件路径（基线以文件为粒度：行号随改动漂移，钉行号只会自伤） */
function filesOf(sites: readonly string[]): string[] {
  return [...new Set(sites.map((s) => s.replace(/:\d+$/, '')))]
}

describe('#348 检查 2：token 引用完整性', () => {
  it('采集健全性：扫到了足够的定义与引用（防正则/扫描根失效后静默空转）', () => {
    expect(definedTokens.size).toBeGreaterThan(300)
    expect(varRefs.size).toBeGreaterThan(80)
    // 抽查必须存在的定义与引用
    expect(definedTokens.has('--text-default')).toBe(true)
    expect(varRefs.has('--bg-base-default')).toBe(true)
    // .vue 的 <template> 内联 :style 也必须被扫到（286 处内联样式是主要引用来源之一）
    expect(varRefs.has('--text-tertiary')).toBe(true)
  })

  it('悬空引用与 KNOWN_DANGLING 基线完全一致（新悬空红 / 修好了也红）', () => {
    const actual = danglingRefs(varRefs, definedTokens, RUNTIME_INJECTED)
    const problems: string[] = []
    for (const [token, sites] of actual) {
      const known = KNOWN_DANGLING[token]
      if (known === undefined) {
        problems.push(
          `新增悬空 token：${token}（未定义且不在运行时注入白名单）\n  引用点：${sites.join(', ')}\n` +
            '  修好它，或在 KNOWN_DANGLING 登记（需写 ticket 与 why）',
        )
        continue
      }
      const unexpected = filesOf(sites).filter((f) => !known.files.includes(f))
      if (unexpected.length > 0) {
        problems.push(
          `基线外新增引用点：${token} → ${unexpected.join(', ')}（已登记文件：${known.files.join(', ')}；` +
            `实际引用点 ${sites.join(', ')}）`,
        )
      }
    }
    for (const [token, known] of Object.entries(KNOWN_DANGLING)) {
      const sites = actual.get(token)
      if (sites === undefined) {
        problems.push(`基线过期：${token} 不再是悬空引用，请删掉这条登记`)
        continue
      }
      const actualFiles = filesOf(sites)
      const missing = known.files.filter((f) => !actualFiles.includes(f))
      if (missing.length > 0) {
        problems.push(`基线引用点失效：${token} 登记的 ${missing.join(', ')} 已不再引用（已修或挪走）`)
      }
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it('AC3：4 处取证点名的悬空引用全部在基线内', () => {
    const required = ['--brand-primary', '--bg-base-primary', '--border-focus-l1', '--icon-quaternary']
    const actual = danglingRefs(varRefs, definedTokens, RUNTIME_INJECTED)
    const missing = required.filter((token) => !actual.has(token))
    expect(missing, `未被检出：${missing.join(', ')}`).toEqual([])
    // 且每处都有真实的引用点（不是靠空数组占位）
    for (const token of required) expect(actual.get(token)?.length).toBeGreaterThan(0)
  })

  it('自证：运行时注入白名单里的每个 token 都真的在模板里被注入（不是假豁免）', () => {
    const problems: string[] = []
    for (const token of Object.keys(RUNTIME_INJECTED)) {
      const sites = varRefs.get(token) ?? []
      if (sites.length === 0) problems.push(`${token} 在 RUNTIME_INJECTED 里但已无人引用（白名单过期）`)
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it('自证：注释里的 var(--nonexistent) 不算引用（防注释噪声逼出假豁免）', () => {
    // JS 注释：只有字符串里的 var(--real-token) 留下
    const stripped = stripJsComments('// 说明：解析不了 var(--fake-token)\nconst x = "var(--real-token)"')
    expect(stripped).toContain('var(--real-token)')
    expect(stripped).not.toContain('var(--fake-token)')
    // CSS 块注释：postcss 把注释当独立节点，不产生 declaration
    const found: string[] = []
    parseCss('a { color: var(--real); /* var(--fake) */ }', 'x.css').walkDecls((d) => {
      const m = /var\(\s*(--[a-z0-9-]+)/.exec(d.value)
      if (m) found.push(m[1])
    })
    expect(found).toEqual(['--real'])
  })

  it('自证：故意制造悬空引用会被判红（防「一律判绿」的假门禁）', () => {
    const out = danglingRefs(
      new Map([
        ['--made-up-token', ['a.vue:1']],
        ['--text-default', ['a.vue:2']],
      ]),
      new Set(['--text-default']),
      RUNTIME_INJECTED,
    )
    expect([...out.keys()]).toEqual(['--made-up-token'])
  })
})
