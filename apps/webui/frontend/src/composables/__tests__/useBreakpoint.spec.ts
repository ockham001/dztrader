import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { effectScope, type EffectScope } from 'vue'
import { useBreakpoint, BREAKPOINTS } from '../useBreakpoint'

// P5-T1 断点体系测试：Tailwind 档（sm 640 / md 768 / lg 1024 / xl 1280）。
// matchMedia 事件驱动（跨断点瞬间触发，非 resize 逐像素轮询）。
// jsdom 无 matchMedia 实现，用可编程 stub：按当前 mock 宽度应答 min-width 查询，
// 并把 change 监听器收集起来供测试手动派发（模拟断点跨越）。
// useBreakpoint 的 onScopeDispose 要求 effectScope：所有用例在作用域内调用，
// 用例结束 stop，保证监听器被移除（否则 jsdom 跨用例泄漏）。

interface MqlStub {
  matches: boolean
  media: string
  addEventListener: (type: 'change', cb: () => void) => void
  removeEventListener: (type: 'change', cb: () => void) => void
}

let mockWidth = 1200
const listeners = new Set<() => void>()
let scope: EffectScope | null = null

function makeMql(media: string): MqlStub {
  return {
    media,
    get matches(): boolean {
      const m = media.match(/\(min-width:\s*([\d.]+)px\)/)
      if (!m) return false
      return mockWidth >= Number.parseFloat(m[1])
    },
    addEventListener: (_type, cb) => listeners.add(cb),
    removeEventListener: (_type, cb) => listeners.delete(cb),
  }
}

function runInScope<T>(fn: () => T): T {
  scope = effectScope()
  return scope.run(fn)!
}

function setWidth(w: number): void {
  mockWidth = w
  for (const cb of listeners) cb()  // 模拟跨断点触发的 change 事件
}

beforeEach(() => {
  mockWidth = 1200
  listeners.clear()
  vi.stubGlobal('matchMedia', (q: string) => makeMql(q))
})

afterEach(() => {
  scope?.stop()
  scope = null
  vi.unstubAllGlobals()
})

describe('BREAKPOINTS', () => {
  it('档位为 Tailwind 标准值（与 layout.css @media 断点双源对齐）', () => {
    expect(BREAKPOINTS).toEqual({ sm: 640, md: 768, lg: 1024, xl: 1280 })
  })
})

describe('useBreakpoint', () => {
  it('按当前视口宽度返回最大命中的档位（<640 为 base）', () => {
    const { tier } = runInScope(() => useBreakpoint())
    setWidth(1400)
    expect(tier.value).toBe('xl')
    setWidth(1100)
    expect(tier.value).toBe('lg')
    setWidth(800)
    expect(tier.value).toBe('md')
    setWidth(650)
    expect(tier.value).toBe('sm')
    setWidth(375)
    expect(tier.value).toBe('base')
  })

  it('isMobile：<md(768) 为 true（侧栏抽屉阈值，与 CSS md 档一致）', () => {
    const { isMobile } = runInScope(() => useBreakpoint())
    setWidth(767)
    expect(isMobile.value).toBe(true)
    setWidth(768)
    expect(isMobile.value).toBe(false)
  })

  it('同档内宽度变化 tier 保持稳定（仅跨断点才切换）', () => {
    const { tier } = runInScope(() => useBreakpoint())
    setWidth(900)
    expect(tier.value).toBe('md')
    setWidth(950)   // 仍在 md 档内
    expect(tier.value).toBe('md')
    setWidth(1024)  // 跨入 lg
    expect(tier.value).toBe('lg')
  })

  it('作用域销毁后移除全部 change 监听（onScopeDispose）', () => {
    setWidth(1400)
    const localScope = effectScope()
    const { tier } = localScope.run(() => useBreakpoint())!
    expect(listeners.size).toBe(Object.keys(BREAKPOINTS).length)
    expect(tier.value).toBe('xl')

    localScope.stop()
    expect(listeners.size).toBe(0)

    // 销毁后宽度变化不应再改 tier（监听已移除）
    setWidth(375)
    expect(tier.value).toBe('xl')
  })
})
