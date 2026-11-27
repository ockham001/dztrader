// 自定义 stylelint 插件（P4 设计系统）：
//   1) dz/token-literal   —— 强制使用设计 token，禁写 CSS 字面量（颜色/间距/圆角）
//   2) dz/no-ds-namespace —— 组件三层强制：feature/domain 的 scoped 样式禁止「定义」.ds-* 模式类
//                            （.ds-* 组件库命名空间只应在 assets/css/components.css 定义，
//                             feature 不得重写组件库原子，须用本地前缀类。上下文后代消费放行）。
//   3) dz/no-inline-style-literal —— 关闭模板 `:style` 旁路（#346）：静态样式必须提成语义类。
//                            判定逻辑在 scripts/inline-style-scan.mjs（与单测/CLI 同一实现）。
import stylelint from 'stylelint'
import { readFileSync } from 'node:fs'

import { scanInlineStyleViolations } from './scripts/inline-style-scan.mjs'

const { report: utilsReport } = stylelint.utils

const ruleTokenLiteral = {
  ruleName: 'dz/token-literal',
  rule: (actual) => {
    return (root, result) => {
      if (actual !== true) return

      const COLOR_HEX = /(?:^|[^a-z0-9#-])#(?:[0-9a-f]{3,8})(?![a-z0-9])/i
      const COLOR_FUNC = /\b(?:rgba?|hsla?)\(/i
      const COLOR_NAMED = /\b(red|green|blue|black|white|gray|grey|yellow|orange|purple|pink|brown|gold|silver|teal|cyan|magenta|violet|indigo|maroon|olive|navy|salmon|lime|aqua|fuchsia)\b/i
      const SPACING_PROPS = new Set(['margin', 'margin-top', 'margin-right', 'margin-bottom', 'margin-left',
        'padding', 'padding-top', 'padding-right', 'padding-bottom', 'padding-left',
        'gap', 'row-gap', 'column-gap', 'font-size', 'line-height'])
      const RADIUS_PROPS = new Set(['border-radius', 'border-top-left-radius', 'border-top-right-radius',
        'border-bottom-left-radius', 'border-bottom-right-radius'])
      const NUMBER_PX = /\b\d+(?:\.\d+)?px\b/
      // #354：`env()` 的 fallback 是 CSS 函数语法而**不是**设计间距字面量，典型形态
      // `calc(var(--spacer-16) + env(safe-area-inset-bottom, 0px))`（iOS 安全区）。
      // 其中的 0px 只能写成 px（env 的 fallback 不接受 token 之外的语义），且换成 token 没有
      // 任何设计收益 —— 检测前先整段剥掉 `env(...)`（含嵌套括号），避免误报。
      // 用「手写括号配对」而不是 `\([^)]*\)`：env() 里合法嵌套 var()/calc()，
      // 非贪婪正则会在第一个 `)` 处停下。剥离后若还有 px，仍照常报出。
      //
      // 必须做**标识符边界**与**大小写**判断（审查实测的两个漏网方向）：
      //   - `myenv(a, 0px)` 会从 `e` 起命中 `env(`，把真字面量整段吃掉 → 静默放宽红线；
      //   - `ENV(...)` 是合法 CSS（函数名不区分大小写），不按小写找会误报。
      // 故改用带 `(?<![\w-])` 前瞻边界的正则定位起点，并加 `i` 标志。
      const ENV_CALL = /(?<![\w-])env\(/gi
      const stripEnvFunctions = (input) => {
        let out = ''
        let i = 0
        while (i < input.length) {
          ENV_CALL.lastIndex = i
          const hit = ENV_CALL.exec(input)
          if (!hit) { out += input.slice(i); break }
          const start = hit.index
          // `(` 的位置 = 匹配起点 + 函数名长度（env 三字母，大小写无关）
          let depth = 0
          let j = hit.index + 3
          for (; j < input.length; j++) {
            if (input[j] === '(') depth++
            else if (input[j] === ')') { depth--; if (depth === 0) { j++; break } }
          }
          out += input.slice(i, start)
          i = j
        }
        return out
      }

      root.walkDecls((decl) => {
        const value = decl.value
        const prop = decl.prop.toLowerCase()

        // P4-T1：颜色检测前剥离 var(...) 段（含 token 名与 fallback），
        // 防 token 名误报（如 var(--brand-grey-500) 被当 "grey" 颜色关键词）。
        // 剥离后剩余的才是真正需转 token 的裸颜色字面量（如独立 rgb(...)、#fff）。
        // 间距/圆角 px 仍按原始 value 检测——var(--spacer-8, 10px) 的 fallback px 同样需清理。
        const strippedColors = value.replace(/var\([^)]*\)/g, '')

        // 1) 颜色字面量（在剥离 var() 后的文本上检测）
        let colorHint = null
        if (COLOR_HEX.test(strippedColors)) colorHint = 'hex 颜色字面量'
        else if (COLOR_FUNC.test(strippedColors)) colorHint = 'rgb/hsl 颜色函数'
        else if (COLOR_NAMED.test(strippedColors)) colorHint = '颜色命名关键词'
        if (colorHint) {
          utilsReport({
            result,
            ruleName: 'dz/token-literal',
            message: `${colorHint} 违禁，须改用 CSS 变量 token（当前仅 currentColor/transparent 可用）: "${value}"`,
            node: decl,
            word: value,
          })
          return
        }

        // 2) 间距/圆角 px 字面量
        const isSpacing = SPACING_PROPS.has(prop)
        const isRadius = RADIUS_PROPS.has(prop)
        if ((isSpacing || isRadius) && NUMBER_PX.test(stripEnvFunctions(value))) {
          utilsReport({
            result,
            ruleName: 'dz/token-literal',
            message: `px 字面量须改用设计 token（${isSpacing ? 'spacer-*' : ''}${isSpacing && isRadius ? ' / ' : ''}${isRadius ? 'radius-*' : ''}）: "${prop}: ${value}"`,
            node: decl,
            word: value,
          })
        }
      })
    }
  },
}

const ruleNoDsNamespace = {
  ruleName: 'dz/no-ds-namespace',
  // feature/domain 的 scoped <style> 禁止「定义」.ds-* 模式库类（绕过 pattern）。
  // 判定：某条规则的首选择器以 .ds- 开头 → 该处在重写 .ds-* 组件库原子。
  // 放行：上下文后代消费（如 .log-browser__filter .ds-dropdown）、:deep(.ds-*)、模板 class 引用。
  rule: (primary) => {
    return (root, result) => {
      if (primary !== true) return

      root.walkRules((rule) => {
        for (const sel of rule.selectors ?? []) {
          if (/^\s*\.ds-[a-z]/.test(sel)) {
            utilsReport({
              result,
              ruleName: 'dz/no-ds-namespace',
              message: `组件内禁止定义 .ds-* 模式库类（绕过 pattern，属三层越权）: "${sel}" —— .ds-* 只应在 assets/css/components.css 定义，feature 须用本地前缀类（如 .log-browser__*、.ms-*）`,
              node: rule,
              word: sel,
            })
            break
          }
        }
      })
    }
  },
}

/* postcss-html 会把一个 `.vue` 拆成多个 postcss root（`<style>` 块一个、其余内容一个），
   stylelint 因而对**同一个 result** 调用本规则多次；而本规则扫的是整份 SFC 原文，
   重复调用会产出逐条重复的告警（实测 1 条 finding → 2 条 warning）。
   用 result 对象去重：每组 root 共享同一个 result，扫一次即可。 */
const scannedResults = new WeakSet()

const ruleNoInlineStyleLiteral = {
  ruleName: 'dz/no-inline-style-literal',
  // 模板 `:style` 是 dz/token-literal 的旁路：stylelint 只扫 `<style>` 块，模板里的
  // `:style="{ fontSize: '10px', color: '#fff' }"` 永远不会被检查。本规则从**源代码**读
  // SFC 原文（postcss-html 不把模板暴露成 AST 节点，见 scripts/inline-style-scan.mjs 头注释），
  // 用同一套判定逻辑报出「静态声明」站点。
  //
  // 报点位置用显式行号：模板不在 postcss AST 里，没有可挂的 node。
  //
  // 除模板 `:style` 外，静态 `style="…"` 属性（无冒号形态）同样由本规则报出 —— 它没有
  // JS 表达式、逐条声明必然静态，是 `dz/token-literal` 的同一类盲区。
  //
  // 已知边界：postcss-html 对**不含** `<style>` 块的 SFC 产出 0 个 postcss root，
  // stylelint 因而不会调用任何规则 —— 「无 style 块 + 满屏内联样式」的文件（重构前的
  // DashboardView / PlaceholderView 就是）仍然只能由 `npm test` 里的
  // `noInlineStyleLiteral.spec.ts` 全仓守卫兜住。lint:css 覆盖含 style 块的文件。
  rule: (primary) => {
    return (root, result) => {
      if (primary !== true) return
      const file = result.opts?.from
      if (!file || !file.endsWith('.vue')) return
      if (scannedResults.has(result)) return
      scannedResults.add(result)
      // 原文优先从 postcss-html 的 document 取（programmatic lint 传 code 时磁盘上没有文件，
      // 且能保证被扫描的文本与 stylelint 实际解析的是同一份）；取不到再读磁盘。
      let source = root.document?.source?.input?.css
      if (typeof source !== 'string' || source.length === 0) {
        try {
          source = readFileSync(file, 'utf8')
        } catch {
          return
        }
      }
      for (const finding of scanInlineStyleViolations(source, { filename: file })) {
        utilsReport({
          result,
          ruleName: 'dz/no-inline-style-literal',
          message: `内联静态样式违禁（${finding.reason}）—— 须提为语义类，或加 <!-- dz-allow-inline-style: 理由 --> 显式豁免: "${finding.prop}: ${finding.value}"`,
          // 模板不在 postcss AST 里，没有可挂的节点；只能给显式行列。
          // 仍需传一个 node 才能过 report() 的行号解析（rangeBy 只取行，不影响 start/end）。
          node: root.first ?? root,
          start: { line: finding.line, column: finding.column },
          end: { line: finding.line, column: finding.column + ':style'.length },
        })
      }
    }
  },
}

/* ===== dz/no-global-ds-shadow =====
   拦「组件 scoped 里重定义全局已存在的 .ds-* 类，且声明会实害」。

   为什么需要它（retro 2026-11-28 实测）：`dz/no-ds-namespace` 只对 domain 目录开启
   （marketSources / logs），`shared/` 与 `views/` 按设计豁免——因为 shared 是 pattern
   的拥有者。但 shared 里**重新定义全局同名类**会产生真实的样式泄漏：
   `components.css` 定义 `.ds-toast { position: fixed; top: 44px; left: 50%; ... }`，
   `shared/ToastContainer.vue` 的 scoped 又写 `.ds-toast { position: relative; }`
   → 全局的 top/left/transform/white-space 全部生效，每条 toast 下移 44px、长文本被
   `overflow: hidden` 裁掉。同名全局类 + scoped 定义 = 声明合并，不是覆盖。

   **为什么不能只判「同名」**：shared 组件是其 pattern 的合法拥有者，给全局类追加
   颜色/字号等**非定位**声明是既有约定（实测 Dropdown / DatePicker 各 1–2 处，都是
   `__trigger-icon` 追加 color/transition，无实害）。只判同名会误伤，把护栏变成存量噪音。

   判据：本文件 `.vue` 的 `<style>` 块里，首选择器为 `.ds-X`，全局 CSS 也定义 `.ds-X`，
   **且两侧声明了同一个属性**（属性交集非空）——只有交集才会发生"谁赢"的污染：
   全局 `display: flex` + scoped `width/height` 是合法追加（实测 Dropdown / DatePicker
   三处都是这种形态，无实害）；而全局 `position: fixed; top: 44px` + scoped
   `position: relative` 交集是 `position`，全局的 top 就必然漏进来（本次 bug）。

   只对**布局/定位一族**属性算交集：颜色/字号等幂等属性两侧同写不会产生实害
   （后写者胜，语义清晰），把它们纳入只会制造存量噪音。

   放行：后代消费（`.foo .ds-x`）、`:deep(.ds-*`)、全局无同名的新 `.ds-*`。

   实现说明：全局**类名 → 属性集**的映射从 components.css **现读**（不硬编码副本，
   避免漂移）；按进程缓存一次。读不到文件时整条规则静默放行（不让 lint 因 IO 失败误报）。 */
let globalDsClassesCache = null

/** 「合并会实害」的属性族：布局/定位/文本流。颜色/字号等幂等属性不在此列。 */
const LEAKY_PROPS = /^(position|top|right|bottom|left|float|display|transform|white-space|overflow|overflow-x|overflow-y|z-index|width|height|min-width|min-height|max-width|max-height|margin|margin-top|margin-right|margin-bottom|margin-left|padding|padding-top|padding-right|padding-bottom|padding-left)$/

function isLeakyProp(prop) {
  return LEAKY_PROPS.test(prop.toLowerCase())
}

/** 解析全局 CSS：返回 Map<类名, Set<泄漏型属性>>。 */
function globalDsClasses() {
  if (globalDsClassesCache !== null) return globalDsClassesCache
  const map = new Map()
  try {
    // plugin 在 frontend/ 根，全局 CSS 在 src/assets/css/
    const cssPath = new URL('./src/assets/css/components.css', import.meta.url)
    const css = readFileSync(cssPath, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '')
    // 逐条规则解析：选择器块 { 声明块 }。够用即可——components.css 是扁平写法，
    // 没有嵌套规则；@media 里的规则同样被这个正则捕获。
    for (const ruleMatch of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
      const selectors = ruleMatch[1]
      const body = ruleMatch[2]
      const props = new Set()
      for (const decl of body.split(';')) {
        const prop = decl.split(':')[0]?.trim().toLowerCase()
        if (prop && isLeakyProp(prop)) props.add(prop)
      }
      if (props.size === 0) continue
      for (const m of selectors.matchAll(/\.(ds-[a-z][a-z0-9-]*(?:__[a-z0-9-]+)?(?:--[a-z0-9-]+)?)/g)) {
        const name = m[1]
        const existing = map.get(name) ?? new Set()
        for (const p of props) existing.add(p)
        map.set(name, existing)
      }
    }
    globalDsClassesCache = map
  } catch {
    globalDsClassesCache = new Map()
  }
  return globalDsClassesCache
}

const ruleNoGlobalDsShadow = {
  ruleName: 'dz/no-global-ds-shadow',
  rule: (primary) => {
    return (root, result) => {
      if (primary !== true) return
      const file = result.opts?.from
      if (!file || !file.endsWith('.vue')) return
      const globals = globalDsClasses()
      if (globals.size === 0) return

      root.walkRules((rule) => {
        // 本规则声明的「泄漏型属性」——没有则不可能是实害
        const leaky = new Set()
        rule.walkDecls((decl) => {
          const prop = decl.prop.toLowerCase()
          if (isLeakyProp(prop)) leaky.add(prop)
        })
        if (leaky.size === 0) return

        for (const sel of rule.selectors ?? []) {
          // 只判「首选择器就是 .ds-X」——后代消费（.foo .ds-x）不算重定义
          const m = /^\s*\.(ds-[a-z][a-z0-9-]*(?:__[a-z0-9-]+)?(?:--[a-z0-9-]+)?)/.exec(sel)
          if (!m) continue
          const name = m[1]
          const globalProps = globals.get(name)
          if (!globalProps) continue
          // 只有**属性交集非空**才会发生"谁赢"的污染；单纯追加是合法的
          const overlap = [...leaky].filter((p) => globalProps.has(p))
          if (overlap.length === 0) continue
          utilsReport({
            result,
            ruleName: 'dz/no-global-ds-shadow',
            message: `组件 scoped 样式与全局 .${name} 声明了同一批布局属性（${overlap.join(', ')}）—— 全局声明会与 scoped 声明**合并**（不是覆盖），全局的其余布局属性（如 position/top）会漏进来。请改用本地前缀类，或把该样式移入 assets/css/components.css`,
            node: rule,
            word: sel,
          })
          break
        }
      })
    }
  },
}

export default [ruleTokenLiteral, ruleNoDsNamespace, ruleNoInlineStyleLiteral, ruleNoGlobalDsShadow]
