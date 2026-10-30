import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Root, type Rule } from 'postcss'

// 主题 token 完整性回归（#198）：
// theme.css 头注释里的 `bg-*/` 会让块注释在 `*/` 处提前闭合，注释后的说明文字变成非法
// 选择器，浏览器按 CSS 规范丢弃其后的整条规则（含浅色 :root 声明块）—— 浅色/默认主题
// token 全空、页面破相。当时 stylelint 对 .css 文件完全空转（全局 customSyntax 设为
// postcss-html，.css 解析出 0 个节点、0 警告，见 stylelint.config.mjs），于是潜伏两个月。
//
// 本 spec 不依赖浏览器运行时：纯文本 + postcss AST 断言，CI 成本可忽略。
//   1. src 下每个 .css 都能被标准 CSS 解析器解析（注释吞规则/语法错 → 抛错即红）；
//   2. light(:root) / dark(.dark) / system(@media) 三段各自定义关键主题 token；
//   3. 选择器里不残留提前闭合的痕迹（`*/` / 非 ASCII）；
//   4. .dark 的字面量 token 必须在 system 段以同值重复（theme.css 头声明的「三段各自独立」）。

const CSS_ROOT = join(fileURLToPath(import.meta.url), '..', '..')
// 扫描起点是 src（而不是 assets/css）：本次事故的形态在任何 .css 里都可能复发，
// 未来新增样式目录无需回来改本 spec。
const SRC_ROOT = join(CSS_ROOT, '..', '..')

/** 递归收集 src 下全部 .css（跳过 node_modules/dist/测试目录） */
function collectCssFiles(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (name === 'node_modules' || name === 'dist' || name === '__tests__') continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) collectCssFiles(full, out)
    else if (extname(name) === '.css') out.push(full)
  }
  return out
}

/** 惰性解析 theme.css：解析失败时由调用方转成断言失败，而不是炸掉整个测试文件 */
function parseTheme(): { root: Root } | { error: string } {
  try {
    return { root: postcss.parse(readFileSync(join(CSS_ROOT, 'theme.css'), 'utf8'), { from: 'theme.css' }) }
  } catch (err) {
    return { error: (err as Error).message }
  }
}

function declarationsOf(root: Root, predicate: (rule: Rule) => boolean): Map<string, string> {
  const out = new Map<string, string>()
  root.walkRules((rule) => {
    if (!predicate(rule)) return
    rule.walkDecls((decl) => {
      if (decl.prop.startsWith('--')) out.set(decl.prop, decl.value.trim())
    })
  })
  return out
}

const parsed = parseTheme()
const themeRoot = 'root' in parsed ? parsed.root : null
const parseError = 'error' in parsed ? parsed.error : null

/** 需要 themeRoot 的断言统一入口：解析失败时给出明确失败信息 */
function requireThemeRoot(): Root {
  expect(parseError, `theme.css 无法被标准 CSS 解析器解析：${parseError}`).toBeNull()
  return themeRoot as Root
}

const lightOf = () => declarationsOf(requireThemeRoot(), (r) => r.selector.trim() === ':root')
const darkOf = () => declarationsOf(requireThemeRoot(), (r) => r.selector.trim() === '.dark')
const systemOf = () =>
  declarationsOf(requireThemeRoot(), (r) => /html:not\(\.light\):not\(\.dark\)/.test(r.selector))

describe('#198 主题 token 回归', () => {
  it('src 下全部 .css 可被标准 CSS 解析器解析（注释提前闭合会在此变红）', () => {
    const files = collectCssFiles(SRC_ROOT)
    // 采集健全性护栏：assets/css 现有 5 个文件（theme/design-tokens/components/layout/dashboard）
    expect(files.length).toBeGreaterThanOrEqual(5)

    const failures: string[] = []
    for (const file of files) {
      try {
        postcss.parse(readFileSync(file, 'utf8'), { from: file })
      } catch (err) {
        failures.push(`${file}: ${(err as Error).message}`)
      }
    }
    expect(failures, failures.join('\n')).toEqual([])
  })

  it('规则选择器不含提前闭合残留（`*/` 或非 ASCII 文本）', () => {
    const root = requireThemeRoot()
    const offenders: string[] = []
    root.walkRules((rule) => {
      // 非 ASCII 判定不用正则转义区间（会触发 no-control-regex）：逐字符码点比较
      const hasNonAscii = [...rule.selector].some((ch) => ch.codePointAt(0)! > 0x7f)
      if (rule.selector.includes('*/') || hasNonAscii) {
        offenders.push(`line ${rule.source?.start?.line ?? '?'}: ${rule.selector.slice(0, 80)}`)
      }
    })
    expect(offenders, offenders.join('\n')).toEqual([])
  })

  // 验收最低面：这三个 token 是 body/侧栏/顶栏/卡片背景与文字的最终来源。
  // 别名（--color-* = var(--bg-* / --text-*)）只需在 :root 定义一次：var() 在计算值
  // 阶段替换，.dark / system 段覆盖被引用的本体 token 后别名自然跟着变，无需逐段重复。
  const REQUIRED = ['--bg-base-default', '--color-background', '--color-foreground']

  /** 某段内该 token 是否「有值」：本段直接定义，或是 :root 定义的 var() 别名 */
  function resolvesIn(section: Map<string, string>, token: string): string | undefined {
    const own = section.get(token)
    if (own !== undefined) return own
    const rootValue = lightOf().get(token)
    if (rootValue?.startsWith('var(')) return `alias(${rootValue})`
    return undefined
  }

  it.each(REQUIRED)('light(:root) 定义 %s', (token) => {
    expect(lightOf().get(token), `${token} 在 :root 段缺失`).toBeTruthy()
  })

  it.each(REQUIRED)('dark(.dark) 可解析 %s（本段定义或 :root 别名）', (token) => {
    expect(resolvesIn(darkOf(), token), `${token} 在 .dark 段无值也不算 :root 别名`).toBeTruthy()
  })

  it.each(REQUIRED)('system(@media prefers-color-scheme:dark) 可解析 %s', (token) => {
    expect(resolvesIn(systemOf(), token), `${token} 在 system 段无值也不算 :root 别名`).toBeTruthy()
  })

  it('.dark 的字面量 token 在 system 段以同值重复（三段各自独立）', () => {
    const dark = darkOf()
    const system = systemOf()
    const light = lightOf()
    const problems: string[] = []
    for (const [token, value] of dark) {
      // 别名（var() 引用）在 :root 定义一次即可随主题解析，不要求重复
      if (value.startsWith('var(')) continue
      const systemValue = system.get(token)
      if (systemValue === undefined) {
        const fallback = light.get(token)
        problems.push(
          `${token} 在 system 段缺失：.dark=${value}` +
            (fallback === undefined ? '（:root 亦无，system 深色下该 token 无值）' : `，system 深色下回退到浅色值 ${fallback}`),
        )
      } else if (systemValue !== value) {
        problems.push(`${token} 值不一致：.dark=${value} system=${systemValue}`)
      }
    }
    expect(problems, problems.join('\n')).toEqual([])
  })
})
