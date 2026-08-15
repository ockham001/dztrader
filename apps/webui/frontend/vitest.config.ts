import { defineConfig } from 'vitest/config'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

// Vitest 配置：复用 vite.config.ts 中的 Vue 插件与 @ 别名，
// 使用 jsdom 环境以支持组件 DOM 渲染。
export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: { '@': resolve(__dirname, 'src') }
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.{test,spec}.ts'],
  },
})
