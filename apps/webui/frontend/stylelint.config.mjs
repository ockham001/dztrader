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
    // #346：关闭模板 :style 旁路。存量已清零，直接 error（不走 #233 的 warning 过渡）。
    'dz/no-inline-style-literal': [true, { severity: 'error' }],
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
    // 资产 CSS 的**格式类**规则豁免（不含设计正确性规则，见下）。
    //
    // #354 收窄：本 override 原先把 `dz/token-literal` 一并关掉，理由是「资产 CSS 是存量
    // 紧凑格式，实测 87 处噪音」—— 但那 87 处全部属于本段列举的**格式/写法偏好**，与设计
    // 正确性无关。设计正确性规则被连带豁免，等价于 CSS 层留着一条与模板 `:style`（#346 已关）
    // 同构的旁路：components/layout/dashboard.css 里写裸 px 与裸色不会被拦。
    //
    // 存量噪音构成（供将来接 Prettier 时核对，与红线无关）：alpha 38 + color-function 38 +
    // custom-property 空行 8 + vendor 前缀 2 + :not 写法 1 = 87；修复前只报 51，因为
    // theme.css 整文件解析失败被跳过（#198）。
    {
      files: ['src/assets/css/**'],
      rules: {
        'alpha-value-notation': null,
        'color-function-notation': null,
        'value-no-vendor-prefix': null,
        'custom-property-empty-line-before': null,
        'selector-not-notation': null,
      },
    },
    // token **定义**文件必须保留 `dz/token-literal` 豁免：这两个文件的正文就是 token 的
    // `--x: <字面量>` 声明，字面量是它们的产物而非违规。规则的颜色检测按 value 判定（与属性名
    // 无关），一旦纳入实测报 357 条（design-tokens 175 + theme 182；一条声明最多报一次，
    // 因为颜色命中后即 return），全部是 token 的字面量产物。
    //
    // 判据是文件性质而不是目录：design-tokens.css 382 条声明**全部**是自定义属性、0 条普通
    // 声明；theme.css 209 条自定义属性 + 3 条 `color-scheme`（值不是字面量，不报）。改这两个
    // 文件不会引入裸色，所以豁免不降低护栏强度。反过来，components/dashboard/layout 三个
    // **消费方**文件（1345/386/366 条普通声明）不再豁免 —— 它们才是 #354 要关的那条旁路。
    //
    // 新增 token 文件时须逐个登记到本 glob；新增消费方 CSS 文件则自动落入红线。
    {
      files: ['src/assets/css/design-tokens.css', 'src/assets/css/theme.css'],
      rules: {
        'dz/token-literal': null,
      },
    },
    // 组件三层（P4 T4）：只对 domain 业务组件目录强制「禁止定义 .ds-* 模式库类」。
    // shared/(pattern 拥有者) 与 views/ 仍有存量 .ds-* 定义，属后续渐进迁移，暂豁免。
    // 新 .ds-* 只准进 assets/css 或 shared/。
    //
    // #354 更正：本 override 原先还写着 `assets/css 由上面 override 豁免` —— 但那只是
    // `dz/no-ds-namespace: null` 的空声明：该规则**只在下面这条 override 里被启用**，
    // 顶层 rules 并未开启，所以在 assets/css 下写 `.ds-*` 本来就不会报（实测探针
    // `.ds-probe-x { color: red }` 只报 token-literal，no-ds-namespace 零命中）。
    // 那行 null 已删除（纯空操作，删了行为不变）；真正的护栏是这条 override 本身。
    {
      files: ['src/components/marketSources/**/*.vue', 'src/components/logs/**/*.vue'],
      rules: {
        'dz/no-ds-namespace': [true, { severity: 'error' }],
      },
    },
    // retro 2026-11-28：拦「scoped 重定义全局已存在的 .ds-* 类」。
    // 与上一条互补：no-ds-namespace 拦「在 domain 目录定义任何 .ds-*」（shared 豁免），
    // 本规则拦「任何目录重定义全局同名 .ds-*」——shared 新增独有的 pattern 类仍放行。
    // 实测成因：components.css 的 `.ds-toast { position:fixed; top:44px }` 与
    // shared/ToastContainer.vue 的 scoped `.ds-toast { position:relative }` 合并，
    // 使 toast 下移 44px 且长文本被裁。
    {
      files: ['**/*.vue'],
      rules: {
        'dz/no-global-ds-shadow': [true, { severity: 'error' }],
      },
    },
  ],
}