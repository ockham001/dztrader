import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, extname, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { navItems } from '@/router/routes'

// 图标名一致性（#207 A1）：`Icon name` 的字面量必须与 `public/icons/builtin/<Name>.svg`
// **精确匹配**（大小写敏感）。useIcon.spec.ts 只验证 URL 拼接，因此 `name="close"` 在
// Linux（大小写敏感文件系统）下 404、Toast 关闭按钮呈空白却没有任何测试报警 —— 本 spec
// 补上这条不变量。
//
// 为什么是「精确匹配」而不是「小写归一」：Vite 的 publicDir 是文件系统直出，Linux 上
// `/icons/builtin/close.svg` 与 `Close.svg` 是两个不同资源；把匹配放宽成大小写无关只会把
// 404 藏起来。
//
// 动态 name（`:name="expr"`）无法静态求值，但必须**显式登记**在 DYNAMIC_ICON_NAME_SOURCES
// 里并给出图标名来源，避免新增动态用法时悄悄绕过检查。可求值的来源（路由 meta）直接取用。

// 本文件位于 src/components/shared/__tests__：向上 4 层 = src，5 层 = 前端根
const SRC_ROOT = join(fileURLToPath(import.meta.url), '..', '..', '..', '..')
const FRONTEND_ROOT = join(SRC_ROOT, '..')
const ICON_DIR = join(FRONTEND_ROOT, 'public', 'icons', 'builtin')

/** 递归收集 src 下的 .vue / .ts（跳过 node_modules / dist / 测试目录） */
function collectSources(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (name === 'node_modules' || name === 'dist' || name === '__tests__') continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) collectSources(full, out)
    else if (['.vue', '.ts'].includes(extname(name))) out.push(full)
  }
  return out
}

/**
 * 图标是否存在于 public/icons/builtin，**大小写精确**。
 *
 * 不能用 `existsSync` 判大小写：NTFS 默认大小写不敏感，`existsSync('close.svg')` 会命中
 * `Close.svg` 返回 true——于是 Windows 上既无法复现该 404（运行时行为确实不同），
 * 也会让本 spec 的「close 必须不存在」断言直接变红（CI 的 Windows job 会跑 npm test）。
 * 这里改成对目录项做**精确字符串集合**比较：Linux 与 Windows 得到同一结论。
 * 注意不要把集合落成小写（那会把大小写错配重新藏起来）。
 */
const ICON_FILES = new Set(readdirSync(ICON_DIR))

function iconExists(name: string): boolean {
  return ICON_FILES.has(`${name}.svg`)
}

/**
 * 动态 name 的登记表：文件 → 该文件里图标名的来源说明。
 * 新增 `<Icon :name="...">` 而没登记，下面的「动态用法必须登记」用例会失败。
 */
const DYNAMIC_ICON_NAME_SOURCES: Record<string, string> = {
  'components/shared/ToastContainer.vue': 'ICON_MAP（toast level → 图标名）与模板兜底 Tips',
  'components/layout/Sidebar.vue': 'router/routes 的 navItems[].icon',
  'components/layout/ThemeDropdown.vue': 'buttonIcon（当前主题 → Sun / Moon / Switch）',
  'views/LoginView.vue': 'passwordIcon（密码可见性 → Eye / EyeInvisible）',
  'views/PlaceholderView.vue': 'route.meta.icon（缺省回退 tool）',
}

interface IconUsage {
  file: string
  names: string[]
}

/**
 * 从一段模板文本里抽 `<Icon ...>` 标签：
 * - 静态 `name="X"` 的字面量 X；
 * - 任何标签里的兜底字面量 `<expr> ?? 'X'`（动态 name 的兜底名同样必须存在）。
 *
 * 兜底抽取必须放在「动态 name → 跳过」之前：动态写法正是兜底名出现的地方，
 * 先 continue 会让这段逻辑变成永不执行的死代码。
 */
function literalIconNames(text: string): string[] {
  const names: string[] = []
  for (const tag of text.match(/<Icon\b[^>]*>/g) ?? []) {
    // 兜底字面量（如 `ICON_MAP[item.level] ?? 'Tips'`）无论静态/动态写法都要收
    for (const fallback of tag.match(/\?\?\s*'([^']+)'/g) ?? []) {
      names.push(fallback.replace(/^\?\?\s*'|'$/g, ''))
    }
    if (/\s(?::|v-bind:)name=/.test(tag)) continue  // 动态 name 本身由登记表负责
    const literal = tag.match(/\sname="([^"]*)"/)
    if (literal) names.push(literal[1])
  }
  return names
}

/** 抽取某个 const 声明的源码块（单行声明只取该行；多行取到下一个仅含 `}`/`})` 的行） */
function declarationBlock(source: string, marker: string): string {
  const lines = source.split('\n')
  const start = lines.findIndex(line => line.includes(marker))
  if (start === -1) return ''
  const head = lines[start]
  const balanced = (head.match(/[{([]/g) ?? []).length === (head.match(/[})\]]/g) ?? []).length
  if (balanced) return head
  const out = [head]
  for (let i = start + 1; i < lines.length; i++) {
    out.push(lines[i])
    if (/^\s*[)}]+\s*;?\s*$/.test(lines[i])) break
  }
  return out.join('\n')
}

/** 从文本中按一条正则抽取捕获组（每处匹配取第 `group` 组） */
function capture(text: string, pattern: RegExp, group = 1): string[] {
  return [...text.matchAll(pattern)].map(m => m[group])
}

/**
 * 各动态来源的图标名提取规则：file → 声明 marker → 提取正则。
 * 只提取**值**（图标名）而非条件里的枚举值（如 'light' / 'dark'）。
 */
const DYNAMIC_ICON_EXTRACTORS: Record<string, { marker: string; pattern: RegExp; min: number }> = {
  'components/shared/ToastContainer.vue': {
    marker: 'const ICON_MAP', pattern: /:\s*'([^']+)'/g, min: 3,
  },
  'components/layout/ThemeDropdown.vue': {
    marker: 'const buttonIcon', pattern: /\breturn\s+'([^']+)'/g, min: 3,
  },
  'views/LoginView.vue': {
    // `showPassword.value ? 'EyeInvisible' : 'Eye'`：三元两侧都是图标名
    marker: 'const passwordIcon', pattern: /'([^']+)'/g, min: 2,
  },
  'views/PlaceholderView.vue': {
    marker: 'const iconName', pattern: /:\s*'([^']+)'/g, min: 1,
  },
}

const sources = collectSources(SRC_ROOT)

const literalUsages: IconUsage[] = sources
  .map(file => ({ file, names: literalIconNames(readFileSync(file, 'utf8')) }))
  .filter(usage => usage.names.length > 0)

const dynamicFiles = sources
  .filter(file => /<Icon\b[^>]*\s(?::|v-bind:)name=/.test(readFileSync(file, 'utf8')))
  .map(file => relative(SRC_ROOT, file).split(sep).join('/'))

describe('图标名与 public/icons/builtin 精确一致（#207 A1）', () => {
  it('存在名为 Close（大写）的图标、不存在 close（小写）——检查本身必须能发现大小写错配', () => {
    expect(iconExists('Close')).toBe(true)
    expect(iconExists('close')).toBe(false)
  })

  it('模板中每个字面量 Icon name 都有同名 svg 文件', () => {
    const missing = literalUsages.flatMap(usage =>
      usage.names
        .filter(name => !iconExists(name))
        .map(name => `${usage.file}: name="${name}" → 缺少 ${name}.svg`),
    )
    expect(missing, `图标字面量与 public/icons/builtin 不一致：\n${missing.join('\n')}`).toEqual([])
  })

  it('扫描确实覆盖到模板（不是空集合假绿）', () => {
    const all = literalUsages.flatMap(usage => usage.names)
    expect(all.length).toBeGreaterThan(10)
    // Toast 关闭图标是本次事故点，必须在扫描结果里
    expect(all).toContain('Close')
  })

  it('动态 name 用法必须登记在 DYNAMIC_ICON_NAME_SOURCES', () => {
    const unregistered = dynamicFiles.filter(file => !(file in DYNAMIC_ICON_NAME_SOURCES))
    expect(
      unregistered,
      `新增的 <Icon :name="..."> 用法未登记图标名来源：\n${unregistered.join('\n')}`,
    ).toEqual([])
    // 反向：登记表里的文件必须仍含动态用法，避免登记表随代码演进静默失效
    const stale = Object.keys(DYNAMIC_ICON_NAME_SOURCES).filter(file => !dynamicFiles.includes(file))
    expect(stale, `登记表条目已不存在动态用法：\n${stale.join('\n')}`).toEqual([])
  })

  it('路由导航图标（navItems）都有同名 svg 文件', () => {
    const missing = navItems.filter(item => !iconExists(item.icon)).map(item => item.icon)
    expect(missing, `路由图标缺少 svg：${missing.join(', ')}`).toEqual([])
  })

  it('可静态求值的动态来源（主题 / 密码可见性 / toast 级别 / 占位页兜底）图标都存在', () => {
    for (const [file, { marker, pattern, min }] of Object.entries(DYNAMIC_ICON_EXTRACTORS)) {
      const block = declarationBlock(readFileSync(join(SRC_ROOT, file), 'utf8'), marker)
      // 解析数量下限：marker 改名或写法调整后不能静默变成空集假绿
      const names = capture(block, pattern)
        .flatMap(name => (name === undefined ? [] : name.split(',')))
        .map(name => name.trim())
        .filter(name => name !== '')
      expect(names.length, `${file} 未能从 ${marker} 提取图标名`).toBeGreaterThanOrEqual(min)
      const missing = names.filter(name => !iconExists(name))
      expect(missing, `${file} 动态图标名缺少 svg：${missing.join(', ')}`).toEqual([])
    }
  })
})
