// #346 内联样式哨兵：解析 .vue 模板里的 `:style` 绑定，判定「静态声明」还是「动态值」。
//
// 为什么需要单独一个解析器（不是直接用 postcss-html）：
//   postcss-html 只把 `<style>` 块交给 stylelint，模板完全不可见（AST 里只有 rule/decl），
//   `:style="{ fontSize: '10px' }"` 这种站点在 stylelint 眼里根本不存在。而 .vue 里没有
//   `<style>` 块时，stylelint 连规则都不会调用（实测 DashboardView.vue 就属于这种），
//   于是「既没有 <style> 块、又满屏内联样式」的文件成了护栏盲区。
//   本模块直接对 SFC 源码做词法级扫描，被两处共用（同一实现，两个入口）：
//     - stylelint 插件 `dz/no-inline-style-literal`（编辑器/CI 的 lint:css 入口）
//     - `src/__tests__/noInlineStyleLiteral.spec.ts`（全仓零违规守卫，覆盖无 style 块文件）
//
// 判定口径（3 类，与票面 AC3 对齐）：
//   1) 拦截「静态声明」：对象字面量里某个属性的值是静态的 —— 字面量字符串/数字，
//      或含字面量的复合表达式（三元、字符串拼接等）。静态值不属于内联，须提为语义类。
//      另拦截**静态 `style="…"` 属性**（没有冒号的形态）：它没有 JS 表达式，逐条声明
//      必然是静态的，同样绕过 `dz/token-literal`，属同一类旁路。
//   2) 放行「纯动态值」：值里没有任何字面量（标识符 / 成员访问 / 函数调用 / 运算），
//      或整体是带 `${}` 插值的模板串且插值外的静态片段不含颜色与带单位长度
//      （如 `` `${positionFlex} 1 0%` ``、`` `${pct}px` ``、`` `url('${url}')` ``）。
//      CSS 自定义属性赋值同样按此口径判，不做整体豁免。
//   3) 放行「显式豁免」：元素开标签前紧邻注释 `<!-- dz-allow-inline-style: <理由> -->`。
//      理由必填，空理由不算豁免（防止变成万能开关）。
//
// 不看的东西（有意取舍）：非对象字面量的属性值（`:style="panelStyle"` 这类整体绑定）无法
// 静态判定，规则放行；它在 `<script>` 里构造的对象不在本扫描范围（票面 AC1 的口径是
// 「模板里字面量形态降到 0」，这类站点由人工在重构时处理）。
//
// 用法（CLI 自查，退出码 1 = 有违规）：
//   node scripts/inline-style-scan.mjs src/views/DashboardView.vue src/views/LoginView.vue
import { readFileSync } from 'node:fs'
import { pathToFileURL } from 'node:url'

/** 豁免注释：理由必填（非空），只作用于紧邻的下一个开标签。
 *  理由允许换行（多行注释），便于在注释里写清楚「为什么不提类」而不挤成一行。 */
const EXEMPT_RE = /<!--\s*dz-allow-inline-style:([\s\S]*?)-->/g

/** 颜色字面量：与 stylelint 插件 dz/token-literal 同一份口径（hex / rgb-hsl / 命名色）。 */
const COLOR_RE = /(?:^|[^a-z0-9#-])#(?:[0-9a-f]{3,8})(?![a-z0-9])|\b(?:rgba?|hsla?)\(|\b(?:red|green|blue|black|white|gray|grey|yellow|orange|purple|pink|brown|gold|silver|teal|cyan|magenta|violet|indigo|maroon|olive|navy|salmon|lime|aqua|fuchsia)\b/i

/** 带单位的长度字面量（px/rem/em/vh/vw/pt）；`%`、`fr`、`ms` 等纯单位片段不算静态长度。 */
const LENGTH_RE = /\b\d+(?:\.\d+)?(?:px|rem|em|vh|vw|pt)\b/

/** 静态 CSS 值证据：模板串静态片段里出现 var() 也说明该片段在写死样式。 */
const VAR_RE = /\bvar\(/

/**
 * 读一个带引号的片段（`'` / `"` / `` ` ``），支持反斜杠转义。
 * @param {string} source
 * @param {number} start 起始引号下标
 * @returns {{ end: number, content: string, quote: string }}
 */
function readQuoted(source, start) {
  const quote = source[start]
  let i = start + 1
  let content = ''
  while (i < source.length) {
    const ch = source[i]
    if (ch === '\\') {
      content += source.slice(i, i + 2)
      i += 2
      continue
    }
    if (ch === quote) return { end: i + 1, content, quote }
    content += ch
    i++
  }
  return { end: source.length, content, quote }
}

/**
 * 找出一段表达式里所有「对象字面量」区间（在引号外、按花括号配对）。
 * 覆盖 `:style="{…}"` 与 `:style="cond ? {…} : {…}"` 两种形态；
 * 模板串里的 `${…}` 落在引号内，自然不会被当对象字面量。
 * @param {string} expr
 * @returns {Array<{ start: number, end: number, body: string }>}
 */
function findObjectLiterals(expr) {
  const found = []
  let i = 0
  while (i < expr.length) {
    const ch = expr[i]
    if (ch === "'" || ch === '"' || ch === '`') {
      i = readQuoted(expr, i).end
      continue
    }
    if (ch === '{') {
      let depth = 0
      let j = i
      let quoted = null
      while (j < expr.length) {
        const c = expr[j]
        if (quoted) {
          if (c === '\\') {
            j += 2
            continue
          }
          if (c === quoted) quoted = null
          j++
          continue
        }
        if (c === "'" || c === '"' || c === '`') {
          quoted = c
          j++
          continue
        }
        if (c === '{') depth++
        else if (c === '}') {
          depth--
          if (depth === 0) break
        }
        j++
      }
      found.push({ start: i, end: j, body: expr.slice(i + 1, j) })
      i = j + 1
      continue
    }
    i++
  }
  return found
}

/**
 * 按顶层逗号切分对象字面量里的属性（引号/括号内的逗号不切）。
 * @param {string} body
 * @returns {string[]}
 */
function splitTopLevelProps(body) {
  const props = []
  let i = 0
  let depth = 0
  let quoted = null
  let current = ''
  while (i < body.length) {
    const ch = body[i]
    if (quoted) {
      current += ch
      if (ch === '\\') {
        current += body[i + 1] ?? ''
        i += 2
        continue
      }
      if (ch === quoted) quoted = null
      i++
      continue
    }
    if (ch === "'" || ch === '"' || ch === '`') {
      quoted = ch
      current += ch
      i++
      continue
    }
    if (ch === '{' || ch === '(' || ch === '[') depth++
    if (ch === '}' || ch === ')' || ch === ']') depth--
    if (ch === ',' && depth === 0) {
      props.push(current)
      current = ''
      i++
      continue
    }
    current += ch
    i++
  }
  props.push(current)
  return props.map((p) => p.trim()).filter(Boolean)
}

/**
 * 拆 `key: value`（顶层冒号；引号内的冒号不切）。
 * @param {string} prop
 * @returns {{ key: string, value: string } | null}
 */
function splitKeyValue(prop) {
  let i = 0
  let depth = 0
  let quoted = null
  while (i < prop.length) {
    const ch = prop[i]
    if (quoted) {
      if (ch === '\\') {
        i += 2
        continue
      }
      if (ch === quoted) quoted = null
      i++
      continue
    }
    if (ch === "'" || ch === '"' || ch === '`') {
      quoted = ch
      i++
      continue
    }
    if (ch === '{' || ch === '(' || ch === '[') depth++
    if (ch === '}' || ch === ')' || ch === ']') depth--
    if (ch === ':' && depth === 0) {
      return { key: prop.slice(0, i).trim(), value: prop.slice(i + 1).trim() }
    }
    i++
  }
  return null
}

/**
 * 把表达式切成「字面量 / 其他」两类片段，供动态性判定使用。
 * @param {string} expr
 * @returns {{ literals: string[], templates: Array<{ chunks: string[], interpolated: boolean }>, bare: string }}
 */
function tokenizeExpression(expr) {
  const literals = []
  const templates = []
  let bare = ''
  let i = 0
  while (i < expr.length) {
    const ch = expr[i]
    if (ch === "'" || ch === '"') {
      const { end, content } = readQuoted(expr, i)
      literals.push(content)
      bare += ' LITERAL '
      i = end
      continue
    }
    if (ch === '`') {
      const { end, content } = readQuoted(expr, i)
      const chunks = []
      let interpolated = false
      let cursor = 0
      let staticPart = ''
      let depth = 0
      while (cursor < content.length) {
        if (content[cursor] === '$' && content[cursor + 1] === '{') {
          interpolated = true
          if (depth === 0) staticPart += ' '
          depth++
          cursor += 2
          continue
        }
        if (depth > 0) {
          if (content[cursor] === '}') depth--
          cursor++
          continue
        }
        if (content[cursor] === '{') depth++
        staticPart += content[cursor]
        cursor++
      }
      chunks.push(staticPart)
      templates.push({ chunks, interpolated })
      bare += ' TEMPLATE '
      i = end
      continue
    }
    if (/[0-9]/.test(ch)) {
      let j = i
      let num = ''
      while (j < expr.length && /[0-9._]/.test(expr[j])) {
        num += expr[j]
        j++
      }
      literals.push(num)
      bare += ' NUMBER '
      i = j
      continue
    }
    bare += ch
    i++
  }
  return { literals, templates, bare }
}

/**
 * 判定单个属性值表达式：放行返回 null，拦截返回原因文案。
 * @param {string} raw
 * @returns {string | null}
 */
export function classifyInlineStyleValue(raw) {
  const expr = raw.trim()
  if (!expr) return '值为空'

  // 值本身是对象/数组字面量时不是合法 CSS 值（`:style` 里只有 CSS 声明），
  // 判不了也不该判 —— 交给类型检查/人工，避免对正常写法误报。
  if (expr.startsWith('{') || expr.startsWith('[')) return null

  const { literals, templates, bare } = tokenizeExpression(expr)

  // 2) 纯动态：值里没有任何字面量，只剩标识符/成员/调用/运算符
  if (literals.length === 0 && templates.length === 0) return null

  // 2') 整体是一个带插值的模板串：只有插值外的静态片段是「纯胶水」（单位、分隔符）
  //     才放行；片段里出现颜色/带单位长度/var() 说明静态常量写死在内联里。
  const isSingleTemplate = templates.length === 1 && literals.length === 0 && bare.trim() === 'TEMPLATE'
  if (isSingleTemplate) {
    if (!templates[0].interpolated) return `模板串没有插值，等于静态字面量「${expr}」`
    const badChunk = templates[0].chunks.find(
      (chunk) => COLOR_RE.test(chunk) || LENGTH_RE.test(chunk) || VAR_RE.test(chunk),
    )
    if (badChunk) return `模板串插值外含静态样式片段「${badChunk.trim()}」`
    return null
  }

  return `静态字面量「${expr}」`
}

/**
 * 站点是否被紧邻的 `<!-- dz-allow-inline-style: 理由 -->` 豁免。
 * 只认「注释结束到开标签之间只有空白」的紧邻形态，理由为空不算豁免。
 * @param {string} source
 * @param {number} tagStart
 * @returns {boolean}
 */
function isExempted(source, tagStart) {
  const before = source.slice(0, tagStart)
  let match = null
  EXEMPT_RE.lastIndex = 0
  let m
  while ((m = EXEMPT_RE.exec(before)) !== null) match = m
  if (!match) return false
  const commentEnd = match.index + match[0].length
  if (before.slice(commentEnd).trim() !== '') return false
  return match[1].trim() !== ''
}

/**
 * 取出 SFC 的 `<template>` 顶层块区间（只扫模板，`<script>`/`<style>` 里的字符串不算内联样式）。
 * 顶层块在本仓一律从第 0 列开始，嵌套的 `<template v-if>` 都在缩进内，按「列 0」配对即可。
 * @param {string} source
 * @returns {Array<{ start: number, end: number }>}
 */
function templateRegions(source) {
  const regions = []
  const openRe = /^<template[^>]*>\s*$/gm
  const closeRe = /^<\/template>\s*$/gm
  for (;;) {
    if (openRe.exec(source) === null) break
    closeRe.lastIndex = openRe.lastIndex
    const close = closeRe.exec(source)
    if (!close) break
    regions.push({ start: openRe.lastIndex, end: close.index })
    openRe.lastIndex = closeRe.lastIndex
  }
  return regions
}

/**
 * 遍历 `<template>` 块里的所有开标签及其属性（引号感知，不用正则匹配标签）。
 *
 * 为什么不用 `/<[a-zA-Z][^>]*?\sstyle\s*=/`：属性值里出现 `>` 很常见
 * （`@click="() => f()"`、`v-if="a > b"`），`[^>]*` 会在那个 `>` 处停下，
 * 把其后的 `style="…"` 整段漏掉 —— 实测漏判（真实回归会从护栏下溜过去）。
 * 这里按字符走：遇到引号就把引号内整段跳过，只有引号外的 `>` 才结束标签。
 * @param {string} source
 * @returns {Array<{ tagName: string, tagStart: number, attrs: Array<{ name: string, valueStart: number, valueEnd: number, value: string, attrStart: number }> }>}
 */
function templateTags(source) {
  const tags = []
  for (const region of templateRegions(source)) {
    let i = region.start
    while (i < region.end) {
      const lt = source.indexOf('<', i)
      if (lt < 0 || lt >= region.end) break
      // 注释整段跳过
      if (source.startsWith('<!--', lt)) {
        const end = source.indexOf('-->', lt + 4)
        i = end < 0 ? region.end : end + 3
        continue
      }
      const after = source[lt + 1] ?? ''
      // 只把 `<字母…` 当开标签；`</`、`<!`、文本里的 `<` 都跳过
      if (!/[a-zA-Z]/.test(after)) { i = lt + 1; continue }

      const nameMatch = /^[a-zA-Z][\w-]*/.exec(source.slice(lt + 1, region.end))
      const tagName = nameMatch ? nameMatch[0] : ''
      let j = lt + 1 + tagName.length
      const attrs = []
      for (;;) {
        while (j < region.end && /\s/.test(source[j])) j++
        if (j >= region.end) break
        if (source[j] === '>') { j++; break }
        if (source[j] === '/' && source[j + 1] === '>') { j += 2; break }
        // 属性名
        const attrStart = j
        while (j < region.end && !/[\s=>]/.test(source[j]) && !(source[j] === '/' && source[j + 1] === '>')) j++
        const name = source.slice(attrStart, j)
        if (!name) { j++; continue }
        while (j < region.end && /\s/.test(source[j])) j++
        if (source[j] !== '=') { attrs.push({ name, valueStart: j, valueEnd: j, value: '', attrStart }); continue }
        j++
        while (j < region.end && /\s/.test(source[j])) j++
        const quote = source[j]
        if (quote === '"' || quote === "'") {
          const valueStart = j + 1
          let k = valueStart
          while (k < region.end && source[k] !== quote) {
            if (source[k] === '\\') k++
            k++
          }
          attrs.push({ name, valueStart, valueEnd: k, value: source.slice(valueStart, k), attrStart })
          j = Math.min(k + 1, region.end)
        } else {
          // 无引号值（少见）：读到空白或 `>` 为止
          let k = j
          while (k < region.end && !/[\s>]/.test(source[k])) k++
          attrs.push({ name, valueStart: j, valueEnd: k, value: source.slice(j, k), attrStart })
          j = k
        }
      }
      tags.push({ tagName, tagStart: lt, attrs })
      i = j
    }
  }
  return tags
}

/**
 * 扫描一份 SFC 源码里的内联样式违规。
 * @param {string} source
 * @param {{ filename?: string }} [options]
 * @returns {Array<{ file: string, line: number, column: number, offset: number, tag: string, prop: string, value: string, reason: string }>}
 */
export function scanInlineStyleViolations(source, options = {}) {
  const file = options.filename ?? '<source>'
  const findings = []
  const push = (attrStart, tag, prop, value, reason) => findings.push({
    file,
    line: source.slice(0, attrStart).split('\n').length,
    column: attrStart - source.lastIndexOf('\n', attrStart),
    offset: attrStart,
    tag,
    prop,
    value,
    reason,
  })

  for (const { tagName, tagStart, attrs } of templateTags(source)) {
    if (isExempted(source, tagStart)) continue
    for (const attr of attrs) {
      // ── A) 静态 `style="…"`（无冒号）：没有 JS 表达式，逐条声明都是静态的 ──
      // 与 `:style` 同样绕过 `dz/token-literal`（它只看 <style> 块），属同一类旁路。
      if (attr.name === 'style') {
        for (const rawDecl of attr.value.split(';')) {
          const decl = rawDecl.trim()
          if (!decl) continue
          const colon = decl.indexOf(':')
          if (colon <= 0) continue
          push(
            attr.attrStart,
            tagName,
            decl.slice(0, colon).trim(),
            decl.slice(colon + 1).trim(),
            '静态 style 属性（无冒号形态同样绕过 stylelint，须提为语义类）',
          )
        }
        continue
      }
      // ── B) `:style` / `v-bind:style` 绑定：逐属性判定静态/动态 ──
      if (attr.name !== ':style' && attr.name !== 'v-bind:style') continue
      for (const object of findObjectLiterals(attr.value)) {
        for (const rawProp of splitTopLevelProps(object.body)) {
          const pair = splitKeyValue(rawProp)
          if (!pair) continue
          const reason = classifyInlineStyleValue(pair.value)
          if (!reason) continue
          push(
            attr.attrStart,
            tagName,
            pair.key.replace(/^['"]|['"]$/g, ''),
            pair.value.replace(/\s+/g, ' '),
            reason,
          )
        }
      }
    }
  }
  return findings
}

const invokedDirectly =
  process.argv[1] !== undefined && import.meta.url === pathToFileURL(process.argv[1]).href

if (invokedDirectly) {
  const files = process.argv.slice(2)
  if (files.length === 0) {
    console.error('usage: node scripts/inline-style-scan.mjs <file.vue>...')
    process.exit(2)
  }
  let total = 0
  for (const file of files) {
    const findings = scanInlineStyleViolations(readFileSync(file, 'utf8'), { filename: file })
    total += findings.length
    for (const f of findings) {
      console.log(`${f.file}:${f.line}:${f.column}  <${f.tag}> ${f.prop} — ${f.reason}`)
    }
  }
  if (total === 0) console.log(`OK 无静态内联样式（${files.length} 个文件）`)
  else console.log(`FAIL 静态内联声明 ${total} 处`)
  process.exit(total === 0 ? 0 : 1)
}
