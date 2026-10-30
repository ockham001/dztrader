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
// 本 spec 不依赖浏览器运行时：纯文本 + postcss AST 断言，CI 成本可忽略（9 例 ~40ms）。
// 不变量（任一失守都会让某个主题下缺色，且浏览器不报错）：
//   1. src 下每个 .css 都能被标准 CSS 解析器解析；
//   2. 任何文件的规则选择器都不残留提前闭合痕迹（`*/` / 非 ASCII / 选择器跨行）——
//      注意这类缺陷**不一定**让 postcss 抛错：残缺文字会被并进下一条规则的选择器；
//   3. system 段的 `html:not(.light):not(.dark)` 必须位于 @media (prefers-color-scheme:
//      dark) 内，且该条件是**裸的**正条件（`not` / `@supports` / 媒体列表都会让它失效或反转）；
//   4. 三段对称：.dark 与 system 的 token 集合必须相等（`.dark⇒system` 同值 + system
//      不得有独有 token），`:root` 字面量 ⊆ `.dark`/system，`:root` 每个 var() 别名的
//      **目标 token** 在三段都被覆盖（别名靠 var() 在计算值阶段替换，只查"名字出现过"
//      会把悬空目标放过去）。

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

const cssFiles = collectCssFiles(SRC_ROOT)

/** 解析单个文件，失败返回错误文本（不抛，便于聚合成一条断言消息） */
function parseFile(file: string): { root: Root } | { error: string } {
  try {
    return { root: postcss.parse(readFileSync(file, 'utf8'), { from: file }) }
  } catch (err) {
    return { error: (err as Error).message }
  }
}

/** 收集一组规则里的自定义属性声明（--prop → 原始值） */
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

const themePath = join(CSS_ROOT, 'theme.css')
const parsedTheme = parseFile(themePath)
const themeRoot = 'root' in parsedTheme ? parsedTheme.root : null
const themeParseError = 'error' in parsedTheme ? parsedTheme.error : null

/** 需要 themeRoot 的断言统一入口：解析失败时给出明确失败信息 */
function requireThemeRoot(): Root {
  expect(themeParseError, `theme.css 无法被标准 CSS 解析器解析：${themeParseError}`).toBeNull()
  return themeRoot as Root
}

const SYSTEM_SELECTOR = /html:not\(\.light\):not\(\.dark\)/

/**
 * system 段是否受「深色」媒体条件保护。
 * 必须沿祖先链找 @media（system 段可能合法嵌套），且条件必须是**裸的正条件**：
 * 接受 `(prefers-color-scheme: dark)` / `screen and (...)` / 附加 `and (min-width…)`；
 * 拒绝 `not (...)`（语义反转）、`@supports`（不是媒体查询）、媒体列表
 * `(prefers-color-scheme: dark), (min-width: 0px)`（含恒真分支）。
 */
function systemRuleIsGuarded(rule: Rule): boolean {
  // 沿祖先链找 @media（system 段可能合法嵌套）。postcss 的 parent 类型含 Document_，
  // 这里只需 type/name/params/parent 四件事，用结构类型避免其类型联合的噪音。
  type Ancestor = { type: string; name?: string; params?: string; parent?: Ancestor | undefined }
  let node = rule.parent as unknown as Ancestor | undefined
  while (node) {
    if (node.type === 'atrule' && node.name?.toLowerCase() === 'media') {
      const params = (node.params ?? '').trim()
      // 媒体列表（顶层逗号）会让 dark 条件与别的条件并列 → 不是"仅深色时生效"
      const isBareDarkQuery =
        !params.includes(',') && !/\bnot\b/i.test(params) && /prefers-color-scheme:\s*dark/i.test(params)
      if (isBareDarkQuery) return true
    }
    node = node.parent
  }
  return false
}

const lightOf = () => declarationsOf(requireThemeRoot(), (r) => r.selector.trim() === ':root')
const darkOf = () => declarationsOf(requireThemeRoot(), (r) => r.selector.trim() === '.dark')
const systemOf = () =>
  declarationsOf(requireThemeRoot(), (r) => SYSTEM_SELECTOR.test(r.selector) && systemRuleIsGuarded(r))

const isAlias = (value: string | undefined): boolean => value?.startsWith('var(') === true

/** 取 var(--x, fallback) / var(--x) 的目标 token 名 */
function aliasTarget(value: string): string | null {
  const m = value.match(/var\(\s*(--[a-z0-9-]+)/)
  return m ? m[1] : null
}

describe('#198 主题 token 回归', () => {
  it('src 下全部 .css 可被标准 CSS 解析器解析（注释提前闭合会在此变红）', () => {
    // 采集健全性护栏：显式钉住必含文件，防止扫描根被意外收窄后静默空转
    expect(cssFiles).toContain(themePath)
    expect(cssFiles.length).toBeGreaterThanOrEqual(5)

    const failures: string[] = []
    for (const file of cssFiles) {
      const outcome = parseFile(file)
      if ('error' in outcome) failures.push(`${file}: ${outcome.error}`)
    }
    expect(failures, failures.join('\n')).toEqual([])
  })

  it('任何 .css 的规则选择器不含提前闭合残留（`*/` 或非 ASCII 文本）', () => {
    // 覆盖全部文件而不只是 theme.css：残缺文字常被 postcss 并进选择器而不抛错。
    // 边界：纯 ASCII 散文形态（如 "tokens for light and dark" 被并进选择器）在本条漏过
    // —— 它没有 `*/` 残留也不是中文。该形态由 lint:css 的 selector-type-no-unknown 兜住
    // （实测 exit 2）。不用"选择器含换行"来判：合法选择器列表本就常跨行书写。
    const offenders: string[] = []
    for (const file of cssFiles) {
      const outcome = parseFile(file)
      if ('error' in outcome) continue // 解析失败由上一个用例报告
      outcome.root.walkRules((rule) => {
        // 非 ASCII 判定不用正则转义区间（会触发 no-control-regex）：逐字符码点比较
        const hasNonAscii = [...rule.selector].some((ch) => ch.codePointAt(0)! > 0x7f)
        if (rule.selector.includes('*/') || hasNonAscii) {
          offenders.push(
            `${file}:${rule.source?.start?.line ?? '?'}: ${rule.selector.replace(/\s+/g, ' ').slice(0, 80)}`,
          )
        }
      })
    }
    expect(offenders, offenders.join('\n')).toEqual([])
  })

  it('system 段规则位于 @media (prefers-color-scheme: dark) 内', () => {
    const root = requireThemeRoot()
    const unguarded: string[] = []
    let found = 0
    root.walkRules((rule) => {
      if (!SYSTEM_SELECTOR.test(rule.selector)) return
      found += 1
      if (!systemRuleIsGuarded(rule)) {
        const at = rule.parent?.type === 'atrule' ? (rule.parent as { params: string }).params : '(非 @media)'
        unguarded.push(`line ${rule.source?.start?.line ?? '?'}: 位于 ${at}`)
      }
    })
    expect(found, '未找到 html:not(.light):not(.dark) 规则（system 主题段丢失）').toBeGreaterThan(0)
    expect(unguarded, unguarded.join('\n')).toEqual([])
  })

  it(':root 的每个字面量 token 在 .dark 与 system 段都被覆盖', () => {
    const light = lightOf()
    const dark = darkOf()
    const system = systemOf()
    const problems: string[] = []
    for (const [token, value] of light) {
      if (isAlias(value)) continue // 别名只看目标 token，见下一条
      if (!dark.has(token)) problems.push(`${token} 在 .dark 段缺失（:root=${value}）→ 深色下回退浅色值`)
      if (!system.has(token)) problems.push(`${token} 在 system 段缺失（:root=${value}）→ 系统深色下回退浅色值`)
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it('.dark / system 的 token 都在 :root 有浅色基线（无单边深色 token）', () => {
    // 反方向补齐：只有"深色段 ⊆ 浅色段"仍会让「删掉 :root 某个字面量 token」静默通过
    // （浅色主题缺色，深色却仍有值——恰恰是 #198 的不对称形态）。
    // 唯一豁免：日志高亮 token（--log-*）按设计只有深色取值，浅色主题刻意不定义。
    const light = lightOf()
    const dark = darkOf()
    const system = systemOf()
    const DARK_ONLY_ALLOWED = /^--log-/
    const problems: string[] = []
    for (const token of dark.keys()) {
      if (!light.has(token) && !DARK_ONLY_ALLOWED.test(token)) {
        problems.push(`${token} 只在 .dark 段定义、:root 无浅色基线 → 浅色主题下该 token 无值`)
      }
    }
    for (const token of system.keys()) {
      if (!light.has(token) && !DARK_ONLY_ALLOWED.test(token)) {
        problems.push(`${token} 只在 system 段定义、:root 无浅色基线 → 浅色主题下该 token 无值`)
      }
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it(':root 的每个 var() 别名，其目标 token 在 .dark 与 system 段都被覆盖', () => {
    const light = lightOf()
    const dark = darkOf()
    const system = systemOf()
    const problems: string[] = []
    for (const [token, value] of light) {
      if (!isAlias(value)) continue
      const target = aliasTarget(value)
      if (!target) {
        problems.push(`${token} 的 var() 目标无法解析：${value}`)
        continue
      }
      // 目标必须在 :root 有字面量定义，否则别名在任何主题下都悬空
      const rootTarget = light.get(target)
      if (rootTarget === undefined) problems.push(`${token} → ${target}：目标未在 :root 定义（别名悬空）`)
      else if (isAlias(rootTarget)) problems.push(`${token} → ${target}：目标本身是别名，链路过深`)
      if (!dark.has(target)) problems.push(`${token} → ${target}：目标在 .dark 段未覆盖（深色下别名值不随主题变化）`)
      if (!system.has(target)) problems.push(`${token} → ${target}：目标在 system 段未覆盖（系统深色下别名值不随主题变化）`)
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it('.dark 与 system 的 token 集合对称（无单边 token）', () => {
    const dark = darkOf()
    const system = systemOf()
    const problems: string[] = []
    // 反向：只在 system 定义的 token 在深色主题（.dark）下无值 → 两套深色不一致
    for (const token of system.keys()) {
      if (!dark.has(token)) problems.push(`${token} 只在 system 段定义、.dark 段缺失（切到深色主题时无值）`)
    }
    for (const token of dark.keys()) {
      if (!system.has(token)) problems.push(`${token} 只在 .dark 段定义、system 段缺失（系统深色下无值）`)
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  it('.dark 的字面量 token 在 system 段以同值重复（三段各自独立）', () => {
    const dark = darkOf()
    const system = systemOf()
    const problems: string[] = []
    for (const [token, value] of dark) {
      if (isAlias(value)) continue // 别名在 :root 定义一次即可随主题解析
      const systemValue = system.get(token)
      if (systemValue === undefined) problems.push(`${token} 在 system 段缺失：.dark=${value}`)
      else if (systemValue !== value) problems.push(`${token} 值不一致：.dark=${value} system=${systemValue}`)
    }
    expect(problems, problems.join('\n')).toEqual([])
  })

  // issue 验收明确点名的三个 token（是 body/侧栏/顶栏/卡片背景与文字的最终来源）
  const REQUIRED = ['--bg-base-default', '--color-background', '--color-foreground']

  it.each(REQUIRED)(':root 定义 %s', (token) => {
    expect(lightOf().get(token), `${token} 在 :root 段缺失`).toBeTruthy()
  })
})
