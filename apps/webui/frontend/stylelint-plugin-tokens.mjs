// 自定义 stylelint 插件（P4 设计系统）：
//   1) dz/token-literal   —— 强制使用设计 token，禁写 CSS 字面量（颜色/间距/圆角）
//   2) dz/no-ds-namespace —— 组件三层强制：feature/domain 的 scoped 样式禁止「定义」.ds-* 模式类
//                            （.ds-* 组件库命名空间只应在 assets/css/components.css 定义，
//                             feature 不得重写组件库原子，须用本地前缀类。上下文后代消费放行）。
import stylelint from 'stylelint'

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

export default [ruleTokenLiteral, ruleNoDsNamespace]