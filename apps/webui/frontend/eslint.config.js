// eslint flat config（Vue3 + TypeScript）
// 规则优先级：typescript-eslint（逻辑）> eslint-plugin-vue（SFC 结构）> eslint:recommended
import js from '@eslint/js'
import tsPlugin from 'typescript-eslint'
import vuePlugin from 'eslint-plugin-vue'
import vueParser from 'vue-eslint-parser'
import a11yPlugin from 'eslint-plugin-vuejs-accessibility'

export default tsPlugin.config(
  // 全局忽略
  {
    ignores: ['dist/**', 'node_modules/**', 'coverage/**'],
  },
  // JS/TS 基础
  js.configs.recommended,
  ...tsPlugin.configs.recommended,
  {
    files: ['**/*.{ts,vue}'],
    languageOptions: {
      parserOptions: {
        parser: tsPlugin.parser,
        extraFileExtensions: ['.vue'],
      },
    },
  },
  // SFC（.vue 模板/脚本）
  ...vuePlugin.configs['flat/recommended'],
  // 无障碍（#206）：组件可键盘操作与对话框语义的规则门禁。
  // 采用插件 recommended 全集，再按下述存量清单**显式**关闭两类——显式 off 而非降级 warning，
  // 因为 lint 脚本是 `--max-warnings 0`，warning 与关闭对门禁等价，写 off 才看得出是"有意豁免"。
  ...a11yPlugin.configs['flat/recommended'],
  {
    files: ['**/*.vue'],
    languageOptions: {
      parser: vueParser,
      parserOptions: {
        parser: tsPlugin.parser,
        extraFileExtensions: ['.vue'],
      },
    },
    rules: {
      // vue 推荐中偏格式化的规则：项目未上 Prettier，格式类交给后续统一风格，
      // 此处关闭避免刷屏淹没真逻辑问题（P4 设计系统/风格统一时再定格式规范）
      'vue/multi-word-component-names': 'off',
      'vue/max-attributes-per-line': 'off',
      'vue/html-self-closing': 'off',
      'vue/singleline-html-element-content-newline': 'off',
      'vue/multiline-html-element-content-newline': 'off',
      'vue/attributes-order': 'off',
      'vue/first-attribute-linebreak': 'off',
      'vue/html-indent': 'off',
      'vue/html-closing-bracket-newline': 'off',
      'vue/html-closing-bracket-spacing': 'off',
      'vue/mustache-interpolation-spacing': 'off',
      'vue/no-multi-spaces': 'off',
      'vue/max-len': 'off',
      'vue/html-button-has-type': 'warn',
      // 故意保留空 catch（前端多处吞异常容忍乱码不崩溃，见项目约定）
      'no-empty': 'off',
      // --- 无障碍：表单控件与 <label> 的程序化关联（#233 起恢复为 error）---
      // #206 曾把这两条显式关闭（当时 97 条存量），本票已按「真实配对」修完并重新打开，
      // 回归基线见 #233。口径说明：label-has-for 配 some:['nesting','id']——
      // 「控件嵌套在 label 内」**或**「label 有 for 且控件有 id」满足其一即可。
      // 为什么不用插件默认的 every：every 要求 nesting 与 for/id **同时**满足，
      // 会把正确写法判红（LoginView 的 <label for>+<input id> 兄弟结构在 every 下报错），
      // 等于强迫把控件塞进 label 里，与本票验收「for/id 配对或 aria-labelledby」相悖，
      // 也与 WCAG / HTML 规范 / axe-core 的口径不一致。
      'vuejs-accessibility/label-has-for': ['error', { required: { some: ['nesting', 'id'] } }],
      'vuejs-accessibility/form-control-has-label': 'error',
    },
  },
  // LogViewerTab 的 v-html 是受控高亮：highlightMessage 已先行转义 &<> 无注入路径
  // （代码内已注释说明），豁免本文件避免每回刷屏
  {
    files: ['**/LogViewerTab.vue'],
    rules: {
      'vue/no-v-html': 'off',
    },
  },
  // 代码风格：优先 Prettier 语义，禁用与格式冲突的 styl 规则
  {
    rules: {
      'no-unused-vars': 'off', // 由 @typescript-eslint/no-unused-vars 统一管（支持类型导入）
      '@typescript-eslint/no-unused-vars': ['warn', { args: 'none' }],
      '@typescript-eslint/no-explicit-any': 'warn',
    },
  },
)