// 单一路由数据源（设计 §5.4：Sidebar NAV_ITEMS 与 router 路由表双份维护治理）
// appRoutes → router 路由表；navItems → Sidebar 导航项（由 appRoutes 派生，顺序一致）
// 约定：非 public 且带 meta.title 的路由自动进入 Sidebar；requiresAdmin 同步到 adminOnly；
// `meta.wip` 同步到 NavItem.wip（Sidebar 据此渲染「开发中」标签，#207 C4）
import type { RouteRecordRaw } from 'vue-router'

export interface AppRouteMeta {
  title?: string
  icon?: string
  public?: boolean
  requiresAdmin?: boolean
  /**
   * 功能尚未完成（路由指向占位页）。Sidebar 在导航项上渲染「开发中」标签，仍可点入；
   * PlaceholderView 自身也会显示「功能开发中」——本标记让用户**点进去之前**就能看出。
   */
  wip?: boolean
}

export interface NavItem {
  label: string
  icon: string
  route: string
  adminOnly?: boolean
  /** 未完成功能（来自 meta.wip），Sidebar 渲染「开发中」标签 */
  wip?: boolean
}

// 顺序即 Sidebar 展示顺序（对齐原 NAV_ITEMS 顺序；vue-router 按 path 匹配，字面路径顺序无关）
// 404 兜底必须保持最后
// 占位页路由（trading / strategy / data / settlement / risk）统一带 `wip: true`：
// 它们是尚未实现的五块功能，导航项必须与 PlaceholderView 的「功能开发中」一致。
export const appRoutes: RouteRecordRaw[] = [
  { path: '/', name: 'dashboard', component: () => import('@/views/DashboardView.vue'), meta: { title: '仪表盘', icon: 'dashboard' } },
  { path: '/market-sources', name: 'market-sources', component: () => import('@/views/MarketSourcesView.vue'), meta: { title: '行情源', icon: 'ArrowWallRight' } },
  { path: '/trading', name: 'trading', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '交易账户', icon: 'profile', wip: true } },
  { path: '/strategy', name: 'strategy', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '策略管理', icon: 'automation', wip: true } },
  { path: '/logs', name: 'logs', component: () => import('@/views/LogsView.vue'), meta: { title: '日志', icon: 'figma' } },
  { path: '/data', name: 'data', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '数据管理', icon: 'storage', wip: true } },
  { path: '/monitor', name: 'monitor', component: () => import('@/views/MonitorView.vue'), meta: { title: '资源监控', icon: 'monitor-filled' } },
  { path: '/settlement', name: 'settlement', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '结算', icon: 'time', wip: true } },
  { path: '/risk', name: 'risk', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '风控', icon: 'context', wip: true } },
  { path: '/user-management', name: 'user-management', component: () => import('@/views/UserManagementView.vue'), meta: { title: '访问控制', icon: 'shield', requiresAdmin: true } },
  { path: '/settings', name: 'settings', component: () => import('@/views/SettingsView.vue'), meta: { title: '系统设置', icon: 'settings', requiresAdmin: true } },
  { path: '/login', name: 'login', component: () => import('@/views/LoginView.vue'), meta: { public: true } },
  { path: '/:pathMatch(.*)*', redirect: '/' },
]

export const navItems: NavItem[] = appRoutes
  .filter((r): r is typeof r & { meta: AppRouteMeta & { title: string } } => !r.meta?.public && !!r.meta?.title)
  .map(r => ({
    label: r.meta.title,
    icon: r.meta.icon ?? '',
    route: r.path,
    adminOnly: r.meta.requiresAdmin,
    wip: r.meta.wip,
  }))
