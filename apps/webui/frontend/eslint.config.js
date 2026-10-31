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
      // --- 无障碍存量豁免（#206 决策：先关，存量修完再打开）---
      // 表单控件与 <label> 的关联（label-has-for 58 条 + form-control-has-label 39 条 = 97 条，
      // 分布在 14 个 .vue 文件）属"label↔input 关联"另一类问题，与键盘可达正交。
      // #206 只修键盘可达与对话框语义；这两条在此显式关闭，清单与口径见 #233，
      // **#233 修完后必须把这两行改回 'error'**（否则该票无法验收）。
      'vuejs-accessibility/label-has-for': 'off',
      'vuejs-accessibility/form-control-has-label': 'off',
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