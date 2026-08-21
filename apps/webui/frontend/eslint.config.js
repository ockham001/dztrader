// eslint flat config（Vue3 + TypeScript）
// 规则优先级：typescript-eslint（逻辑）> eslint-plugin-vue（SFC 结构）> eslint:recommended
import js from '@eslint/js'
import tsPlugin from 'typescript-eslint'
import vuePlugin from 'eslint-plugin-vue'
import vueParser from 'vue-eslint-parser'

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