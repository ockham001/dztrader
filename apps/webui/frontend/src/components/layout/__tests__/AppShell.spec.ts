import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount } from '@vue/test-utils'

// P5-T2 AppShell：CSS 主导布局改造后组件可挂载 + useBreakpoint 行为分支
// （<md 抽屉模式 / 跨断点回桌面自动收起抽屉）。jsdom 无 matchMedia，用可编程 stub。

let mockWidth = 1200
const listeners = new Set<() => void>()

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({
    connectionState: { value: 'connected' },
    lastError: { value: null },
    connect: vi.fn(),
    disconnect: vi.fn(),
    mdConnect: vi.fn(),
    mdDisconnect: vi.fn(),
    subscribeLog: vi.fn(),
    unsubscribeLog: vi.fn(),
  }),
}))

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
})

afterEach(() => {
  vi.unstubAllGlobals()
  vi.resetModules()
})

function setWidth(w: number): void {
  mockWidth = w
  for (const cb of listeners) cb()
}

const TopbarStub = {
  name: 'Topbar',
  emits: ['toggle-mobile'],
  template: '<button type="button" class="topbar-toggle" @click="$emit(\'toggle-mobile\')" />',
}

async function mountShell() {
  const { default: Shell } = await import('../AppShell.vue')
  return mount(Shell, {
    global: {
      stubs: { Sidebar: true, Topbar: TopbarStub, teleport: true },
    },
    slots: { default: '<div class="test-slot">content</div>' },
  })
}

function overlay(wrapper: Awaited<ReturnType<typeof mountShell>>) {
  return wrapper.find('.sidebar-overlay')
}

describe('AppShell (P5 CSS 主导布局)', () => {
  it('桌面宽度挂载成功，渲染 slot 与布局类', async () => {
    const wrapper = await mountShell()
    expect(wrapper.find('.app-shell').exists()).toBe(true)
    expect(wrapper.find('.test-slot').exists()).toBe(true)
    // 桌面（≥md）无 mobile overlay
    expect(overlay(wrapper).exists()).toBe(false)
  })

  it('mobile 抽屉由 toggle-mobile 打开（overlay display:block）', async () => {
    const wrapper = await mountShell()
    setWidth(500)  // 桌面 → mobile
    await wrapper.vm.$nextTick()

    expect(overlay(wrapper).exists()).toBe(true)
    expect((overlay(wrapper).element as HTMLElement).style.display).toBe('none')

    await wrapper.find('.topbar-toggle').trigger('click')
    expect((overlay(wrapper).element as HTMLElement).style.display).toBe('block')
  })

  it('跨断点回桌面自动收起抽屉，再回 mobile 仍保持关闭', async () => {
    const wrapper = await mountShell()
    setWidth(500)
    await wrapper.vm.$nextTick()
    await wrapper.find('.topbar-toggle').trigger('click')
    expect((overlay(wrapper).element as HTMLElement).style.display).toBe('block')

    setWidth(1200)  // mobile → 桌面
    await wrapper.vm.$nextTick()
    expect(overlay(wrapper).exists()).toBe(false)

    setWidth(500)  // 再回 mobile
    await wrapper.vm.$nextTick()
    expect(overlay(wrapper).exists()).toBe(true)
    expect((overlay(wrapper).element as HTMLElement).style.display).toBe('none')
  })
})
