import { readFileSync } from 'node:fs'
import postcss, { type Root, type Rule } from 'postcss'

// 主题 token 静态分析的共享事实源（#348）。
//
// 为什么单独成文件：`themeTokens.spec.ts`（#198）与 `tokenContrast.spec.ts`（#348）都要
// 判断「这条规则是否位于 system 主题段（@media prefers-color-scheme: dark 内）」并取各主题段
// 的 token 集合。两处各写一份会漂移 —— 审查实测第一版在 tokenContrast 里放宽成「含 dark
// 字样即算」，会把 `(prefers-color-scheme: dark), (min-width: 0px)` 这类**恒真媒体列表**
// 误认成 system 段。按 review-checklist §2.4「同一事实两处定义必然漂移」，口径收在这里
// 一份，两处都从这里取。
//
// theme.css 的三段结构（#198 定的不变量）：
//   `:root`                       —— 浅色（裸规则）
//   `.dark`                       —— 显式深色（裸规则，不包在 @media 里）
//   `html:not(.light):not(.dark)` —— 跟随系统深色（必须位于裸深色 @media 内）
// 注意 `.dark` **不是** @media 内的规则，所以「深色段」不等于「在深色媒体查询内」。

/** system 主题段的选择器（跟随系统，无 .light/.dark class） */
export const SYSTEM_SELECTOR = 'html:not(.light):not(.dark)'

/**
 * 规则是否位于**裸的**深色媒体条件内。
 *
 * 必须沿祖先链找 @media（system 段可能合法嵌套），且条件只能是正条件：
 *   - 接受 `(prefers-color-scheme: dark)` / `screen and (...)` / 附加 `and (min-width…)`
 *   - 拒绝 `not (...)`:语义反转，深色时反而不生效
 *   - 拒绝 `@supports`:不是媒体查询
 *   - 拒绝媒体列表 `(prefers-color-scheme: dark), (min-width: 0px)`:含恒真分支，
 *     会在任何宽度下都生效，不能代表「仅系统深色时」
 */
export function isInsideBareDarkMedia(rule: Rule): boolean {
  // postcss 的 parent 类型含 Document_，这里只需 type/name/params/parent 四件事，
  // 用结构类型避免其类型联合的噪音。
  type Ancestor = { type: string; name?: string; params?: string; parent?: Ancestor | undefined }
  let node = rule.parent as unknown as Ancestor | undefined
  while (node) {
    if (node.type === 'atrule' && node.name?.toLowerCase() === 'media') {
      const params = (node.params ?? '').trim()
      const isBareDarkQuery =
        !params.includes(',') && !/\bnot\b/i.test(params) && /prefers-color-scheme:\s*dark/i.test(params)
      if (isBareDarkQuery) return true
    }
    node = node.parent
  }
  return false
}

/** 解析一个 CSS 文件为 postcss AST（解析失败抛出带文件名的错误） */
export function parseCssFile(file: string): Root {
  try {
    return postcss.parse(readFileSync(file, 'utf8'), { from: file })
  } catch (err) {
    throw new Error(`${file} 无法被标准 CSS 解析器解析：${(err as Error).message}`)
  }
}

/**
 * 取某个选择器在指定媒体上下文里的全部自定义属性声明（`--x` → 原始值）。
 *
 * 选择器必须单选择器精确匹配：`:root, .foo` 这类列表不算（它同时作用于多处，
 * 当成主题基线会让「某主题缺 token」的判定失真）。
 *
 * @param insideDarkMedia `true` = 只取位于裸深色 @media 内的同名规则（system 段）；
 *                        `false` = 只取裸规则（`:root` / `.dark`）
 */
export function tokenDeclarationsOf(
  root: Root,
  selector: string,
  insideDarkMedia: boolean,
): Map<string, string> {
  const out = new Map<string, string>()
  root.walkRules((rule) => {
    if (rule.selectors.length !== 1 || rule.selectors[0].trim() !== selector) return
    if (isInsideBareDarkMedia(rule) !== insideDarkMedia) return
    rule.walkDecls((decl) => {
      if (decl.prop.startsWith('--')) out.set(decl.prop.trim(), decl.value.trim())
    })
  })
  return out
}
