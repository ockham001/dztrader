// stylelint 配置：标准规则 + 项目自定义 token 红线插件
// 当前阶段设置 warning（摸清存量规模），P4 设计系统完成后再升 error。
export default {
  extends: ['stylelint-config-standard'],
  plugins: ['./stylelint-plugin-tokens.mjs'],
  customSyntax: 'postcss-html', // 支持 .vue <style> 块
  rules: {
    // ----- 项目 token 红线（P4 前为 warning）-----
    'dz/token-literal': 'warn',

    // ----- 标准规则收敛 -----
    // 允许嵌套空规则等，避免过度限制
    'no-empty-source': null,
    'declaration-empty-line-before': null,
    'block-no-empty': null,
    // 属性顺序不强排（防重构噪音），交给统一风格
    'declaration-block-no-redundant-longhand-properties': null,
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
    // 全局变量定义文件例外：token 值本身必须用字面量定义
    {
      files: ['src/assets/css/**'],
      rules: {
        'dz/token-literal': null,
      },
    },
  ],
}