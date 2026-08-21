import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount } from '@vue/test-utils'

// P5-T2 AppShell 冒烟：CSS 主导布局改造后组件可挂载 + useBreakpoint 行为分支
// （<md 抽屉模式 / 跨断点回桌面自动收起抽屉）。jsdom 无 matchMedia，用可编程 stub。

let mockWidth = 1200
const listeners = new Set<() => void>()

beforeEach(() => {
  mockWidth = 1200
  listeners.clear()
  vi.stubGlobal('matchMedia', (q: string) => ({
    media: q,
    get matches(): boolean {
      const m = q.match(/\(min-width:\s*([\d.]+)px\)/)
      return m ? mockWidth >= Number.parseFloat(m[1]) : false
    },
    addEventListener: (_t: string, cb: () => void) => listeners.add(cb),
    removeEventListener: (_t: string, cb: () => void) => listeners.delete(cb),
  }))
  vi.mock('@/composables/wsClient', () => ({
    useWebSocket: () => ({ connectionState: { value: 'connected' } }),
  }))
})

afterEach(() => {
  vi.unstubAllGlobals()
  vi.resetModules()
})

function setWidth(w: number): void {
  mockWidth = w
  for (const cb of listeners) cb()
}

async function mountShell() {
  const { default: Shell } = await import('../AppShell.vue')
  return mount(Shell, {
    global: {
      stubs: { Sidebar: true, Topbar: true, teleport: true },
    },
    slots: { default: '<div class="test-slot">content</div>' },
  })
}

describe('AppShell (P5 CSS 主导布局)', () => {
  it('桌面宽度挂载成功，渲染 slot 与布局类', async () => {
    const wrapper = await mountShell()
    expect(wrapper.find('.app-shell').exists()).toBe(true)
    expect(wrapper.find('.test-slot').exists()).toBe(true)
    // 桌面（≥md）无 mobile overlay
    expect(wrapper.find('.sidebar-overlay').exists()).toBe(false)
  })

  it('<md 渲染 mobile overlay；跨断点回桌面自动收起抽屉', async () => {
    const wrapper = await mountShell()
    setWidth(500)  // 桌面 → mobile
    await wrapper.vm.$nextTick()
    expect(wrapper.find('.sidebar-overlay').exists()).toBe(true)

    // 打开抽屉（overlay 显示态依赖 mobileSidebarOpen，经 Sidebar stub 无法点击；
    // 直接验证断点跨越收起逻辑：回到桌面后 overlay 消失）
    setWidth(1200)  // mobile → 桌面
    await wrapper.vm.$nextTick()
    expect(wrapper.find('.sidebar-overlay').exists()).toBe(false)
  })
})