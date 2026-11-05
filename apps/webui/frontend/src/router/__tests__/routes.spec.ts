import { describe, it, expect } from 'vitest'
import { appRoutes, navItems } from '../routes'

// #207 C4：占位页路由在导航上必须标出「开发中」，且实现完成的页面不得被误标。
// 单测直接锁路由表事实（Sidebar 渲染断言在 SidebarNavWip.spec.ts）。
//
// #251：/trading 已由 PlaceholderView 换成真实账户配置页（TradingAccountsView），
// 因此从占位页清单移出并加入「已实现页面」白名单——事实更新而不是放宽断言：
// 下面的「wip 与 component 指向 PlaceholderView 一致」用**精确集合比对**守住
// 「新增占位页必须登记 wip、替换占位页必须摘掉 wip」。
const PLACEHOLDER_PATHS = ['/strategy', '/data', '/settlement', '/risk']

describe('路由 navItems 的 wip 标记（#207 C4）', () => {
  it('四个占位页路由全部带 meta.wip，并在 navItems 上透传', () => {
    for (const path of PLACEHOLDER_PATHS) {
      const route = appRoutes.find(r => r.path === path)
      expect(route, `缺少路由 ${path}`).toBeTruthy()
      expect(route!.meta?.wip, `${path} 缺少 meta.wip`).toBe(true)

      const nav = navItems.find(n => n.route === path)
      expect(nav, `navItems 缺少 ${path}`).toBeTruthy()
      expect(nav!.wip).toBe(true)
    }
  })

  it('已实现页面的导航项不得带 wip（否则「开发中」标注会变成噪音）', () => {
    // /trading（#251）：账户配置页已交付，页面不是占位页 → 不得带 wip
    // /trading-data（#246）：委托 tab 已交付，页面不是占位页 → 不得带 wip
    const implemented = ['/', '/market-sources', '/trading', '/trading-data', '/logs', '/monitor', '/user-management', '/settings']
    for (const path of implemented) {
      const nav = navItems.find(n => n.route === path)
      expect(nav, `navItems 缺少 ${path}`).toBeTruthy()
      expect(nav!.wip, `${path} 不应标 wip`).toBeUndefined()
    }
  })

  it('/trading 指向真实账户配置页（不再复用 PlaceholderView）', () => {
    const route = appRoutes.find(r => r.path === '/trading')
    expect(route).toBeTruthy()
    expect(typeof route!.component).toBe('function')
    expect(String(route!.component)).toContain('TradingAccountsView')
    expect(String(route!.component)).not.toContain('PlaceholderView')
  })

  it('wip 与「component 指向 PlaceholderView」一致（防止占位页新增/替换后漏标）', () => {
    const placeholderRoutes = appRoutes.filter(r =>
      typeof r.component === 'function'
      && String(r.component).includes('PlaceholderView'))
    const placeholderPaths = placeholderRoutes.map(r => r.path).sort()
    expect(placeholderPaths).toEqual([...PLACEHOLDER_PATHS].sort())

    const wipPaths = navItems.filter(n => n.wip === true).map(n => n.route).sort()
    expect(wipPaths).toEqual(placeholderPaths)
  })
})
