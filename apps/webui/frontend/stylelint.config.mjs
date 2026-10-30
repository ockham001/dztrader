// stylelint 配置：标准规则 + 项目自定义 token 红线插件
// 当前阶段设置 warning（摸清存量规模），P4 设计系统完成后再升 error。
//
// customSyntax 必须**按文件类型**限定（见底部 overrides），不能全局设成 postcss-html：
// stylelint 的 customSyntax 是「所有被 lint 的文件都用这个语法解析」，把 .css 也交给
// postcss-html 会让每个 .css 解析出 0 个节点、0 个 warning —— lint:css 对全部独立
// 样式文件静默空转（#198：theme.css 头注释提前闭合导致浅色主题整体失效，潜伏两个月，
// 期间 lint:css 一直 exit 0）。.vue 的 <style> 块才需要 postcss-html；.css 用默认
// postcss 解析器，语法错误（含注释吞规则）才会如实报 CssSyntaxError。
export default {
  extends: ['stylelint-config-standard'],
  plugins: ['./stylelint-plugin-tokens.mjs'],
  rules: {
    // ----- 项目 token 红线（P4-T3 升 error：任何字面量违规即失败，CI 门禁）-----
    // 注：插件的 `if (actual !== true) return` 要求 primary 必须为 true；
    // severity 用 secondary options 设置（P4-T1 曾为 warning 摸清存量）。
    'dz/token-literal': [true, { severity: 'error' }],
    // 组件三层（P4 T4）：dz/no-ds-namespace 仅对 domain 目录生效（见底部 overrides）

    // ----- 标准规则收敛 -----
    // 允许嵌套空规则等，避免过度限制
    'no-empty-source': null,
    'declaration-empty-line-before': null,
    'block-no-empty': null,
    // 属性顺序不强排（防重构噪音），交给统一风格
    'declaration-block-no-redundant-longhand-properties': null,
    // 低信号标准规则（P4 T4）：组件库全局 CSS 刻意用 vendor 前缀做跨浏览器，
    // no-descending-specificity / no-duplicate-selectors 是继承来的存量风格，非 P4 目标，
    // 关闭以免 lint:css 的 --max-warnings 0 把存量资产 CSS 误判为红线失败。
    'property-no-vendor-prefix': null,
    'no-descending-specificity': null,
    'no-duplicate-selectors': null,
    // 纯格式/命名类规则（P4 T4）：项目未上 Prettier，存量 CSS 为紧凑风格，
    // 换行/空行/大小写/hex 长度属"格式归 P4"范畴，非本阶段红线，关闭以免误伤。
    'rule-empty-line-before': null,
    'comment-empty-line-before': null,
    'at-rule-empty-line-before': null,
    'value-keyword-case': null,
    'color-hex-length': null,
    // 项目采用 BEM（block__element--modifier），放宽默认 kebab-case
    'selector-class-pattern': [
      '^[a-z0-9]+(-[a-z0-9]+)*(__[a-z0-9]+(-[a-z0-9]+)*)?(--[a-z0-9]+(-[a-z0-9]+)*)?$',
      { resolveNestedSelectors: true },
    ],
    // 旧/新 media range 写法都合法，统一写法属 P4 风格分内事
    'media-feature-range-notation': null,
    // 单行声明数放宽：紧凑声明块是既有风格
    'declaration-block-single-line-max-declarations': null,
    // Vue SFC 的 :deep() / :global() 是合法伪类，标准库不认识属误报
    'selector-pseudo-class-no-unknown': [
      true,
      { ignorePseudoClasses: ['deep', 'global', 'slotted'] },
    ],
  },
  overrides: [
    // .vue 的 <style> 块需要 postcss-html 才能从 SFC 里剥出样式；.css 保持默认 postcss
    // 解析器（理由见文件头注释：#198 的静默空转）。
    {
      files: ['**/*.vue'],
      customSyntax: 'postcss-html',
    },
    // 全局变量定义文件例外：token 值本身必须用字面量定义
    {
      files: ['src/assets/css/**'],
      rules: {
        'dz/token-literal': null,
        'dz/no-ds-namespace': null,
        // 资产 CSS 是「存量紧凑格式」而非组件样式（#198 后 .css 首次真正被 lint）：
        // 这几条纯格式/写法偏好规则在修复后实测报 87 处存量噪音（= alpha 38 +
        // color-function 38 + custom-property 空行 8 + vendor 前缀 2 + :not 写法 1），
        // 与主题正确性无关。修复前只报 51 处，因为 theme.css 整文件解析失败被跳过。
        // 与顶部已关闭的 8 条同类规则同口径 —— 格式归 P4/Prettier，不进门禁。
        'alpha-value-notation': null,
        'color-function-notation': null,
        'value-no-vendor-prefix': null,
        'custom-property-empty-line-before': null,
        'selector-not-notation': null,
      },
    },
    // 组件三层（P4 T4）：只对 domain 业务组件目录强制「禁止定义 .ds-* 模式库类」。
    // shared/(pattern 拥有者) 与 views/ 仍有存量 .ds-* 定义，属后续渐进迁移，暂豁免；
    // assets/css 由上面 override 豁免。新 .ds-* 只准进 assets/css 或 shared/。
    {
      files: ['src/components/marketSources/**/*.vue', 'src/components/logs/**/*.vue'],
      rules: {
        'dz/no-ds-namespace': [true, { severity: 'error' }],
      },
    },
  ],
}