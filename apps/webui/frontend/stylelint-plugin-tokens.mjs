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
        if ((isSpacing || isRadius) && NUMBER_PX.test(value)) {
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

export default [ruleTokenLiteral, ruleNoDsNamespace, ruleNoInlineStyleLiteral]
