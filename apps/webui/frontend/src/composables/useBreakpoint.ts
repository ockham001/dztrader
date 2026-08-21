import { ref, computed, onScopeDispose, type Ref, type ComputedRef } from 'vue'

// P5-T1 断点体系：Tailwind 档位，matchMedia 事件驱动。
//
// 档位（与 layout.css 的 @media 断点【双源对齐】，白名单测试 machine-enforce）：
//   base <640 | sm ≥640 | md ≥768 | lg ≥1024 | xl ≥1280
// 用途：布局重排一律走 CSS @media（CSS 主导）；本 composable 只供【行为分支】
// （如侧栏 抽屉 vs rail、跨断点时收起 mobile 抽屉）读取当前档位。
// 机制：matchMedia('(min-width: Npx)').addEventListener('change') —— 浏览器只在
// 跨越断点瞬间触发回调（非 resize 逐像素），事件驱动无轮询。

/** 断点档位（px）。CSS 侧 @media 数值与此一一对应，改这里必须同步 layout.css */
export const BREAKPOINTS = {
  sm: 640,
  md: 768,
  lg: 1024,
  xl: 1280,
} as const

export type Breakpoint = keyof typeof BREAKPOINTS

/** 视口档位：base = <sm（最小档，Tailwind 无 xs 惯例） */
export type ViewportTier = 'base' | Breakpoint

export interface UseBreakpoint {
  /** 当前命中的最大档位（响应式） */
  tier: Ref<ViewportTier>
  /** <md(768)：侧栏转抽屉、触摸优先形态（与 CSS md 档语义一致） */
  isMobile: ComputedRef<boolean>
}

export function useBreakpoint(): UseBreakpoint {
  const tier = ref<ViewportTier>('base')

  const mqls: MediaQueryList[] = []
  const cleanups: (() => void)[] = []

  function recompute(): void {
    // 取满足 min-width 的最大档；全不满足 = base
    let current: ViewportTier = 'base'
    for (const key of Object.keys(BREAKPOINTS) as Breakpoint[]) {
      if (window.matchMedia(`(min-width: ${BREAKPOINTS[key]}px)`).matches) {
        current = key
      }
    }
    tier.value = current
  }

  // 每个断点一个 MQL；任一 change（跨断点瞬间）即重算
  for (const key of Object.keys(BREAKPOINTS) as Breakpoint[]) {
    const mql = window.matchMedia(`(min-width: ${BREAKPOINTS[key]}px)`)
    mqls.push(mql)
    const onChange = (): void => recompute()
    mql.addEventListener('change', onChange)
    cleanups.push(() => mql.removeEventListener('change', onChange))
  }
  recompute()  // 初始化：同步当前宽度

  onScopeDispose(() => {
    for (const fn of cleanups) fn()
  })

  const isMobile = computed(() => {
    const order: ViewportTier[] = ['base', 'sm', 'md', 'lg', 'xl']
    return order.indexOf(tier.value) < order.indexOf('md')
  })

  return { tier, isMobile }
}