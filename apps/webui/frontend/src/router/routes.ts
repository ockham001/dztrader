// 单一路由数据源（设计 §5.4：Sidebar NAV_ITEMS 与 router 路由表双份维护治理）
// appRoutes → router 路由表；navItems → Sidebar 导航项（由 appRoutes 派生，顺序一致）
// 约定：非 public 且带 meta.title 的路由自动进入 Sidebar；requiresAdmin 同步到 adminOnly
import type { RouteRecordRaw } from 'vue-router'

export interface AppRouteMeta {
  title?: string
  icon?: string
  public?: boolean
  requiresAdmin?: boolean
}

export interface NavItem {
  label: string
  icon: string
  route: string
  adminOnly?: boolean
}

// 顺序即 Sidebar 展示顺序（对齐原 NAV_ITEMS 顺序；vue-router 按 path 匹配，字面路径顺序无关）
// 404 兜底必须保持最后
export const appRoutes: RouteRecordRaw[] = [
  { path: '/', name: 'dashboard', component: () => import('@/views/DashboardView.vue'), meta: { title: '仪表盘', icon: 'dashboard' } },
  { path: '/market-sources', name: 'market-sources', component: () => import('@/views/MarketSourcesView.vue'), meta: { title: '行情源', icon: 'ArrowWallRight' } },
  { path: '/trading', name: 'trading', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '交易账户', icon: 'profile' } },
  { path: '/strategy', name: 'strategy', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '策略管理', icon: 'automation' } },
  { path: '/logs', name: 'logs', component: () => import('@/views/LogsView.vue'), meta: { title: '日志', icon: 'figma' } },
  { path: '/data', name: 'data', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '数据管理', icon: 'storage' } },
  { path: '/monitor', name: 'monitor', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '资源监控', icon: 'monitor-filled' } },
  { path: '/settlement', name: 'settlement', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '结算', icon: 'time' } },
  { path: '/risk', name: 'risk', component: () => import('@/views/PlaceholderView.vue'), meta: { title: '风控', icon: 'context' } },
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
  }))