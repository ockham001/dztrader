// 自定义 stylelint 插件：强制使用设计 token，禁写 CSS 字面量
// 为 P4 设计系统铺路：颜色 / 间距(px) / 圆角(px) 字面量一律进 token 表。
// 允许例外：currentColor / transparent 是语义色非字面量，放行。
// 级别在 stylelint.config.mjs 中设置（当前 warning 用于摸清存量规模）。
import stylelint from 'stylelint'

const { report: utilsReport } = stylelint.utils

export default {
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

        // 1) 颜色字面量
        let colorHint = null
        if (COLOR_HEX.test(value)) colorHint = 'hex 颜色字面量'
        else if (COLOR_FUNC.test(value)) colorHint = 'rgb/hsl 颜色函数'
        else if (COLOR_NAMED.test(value)) colorHint = '颜色命名关键词'
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