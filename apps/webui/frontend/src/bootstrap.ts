/**
 * 应用挂载时机（issue #118）。
 *
 * 路由就绪前挂载时 `route.meta` 为空，`App.vue` 会按「非 public」先渲染 AppShell，
 * 顶栏 `onMounted` 随即发起需要登录态的请求 → 401 → 整页跳转 `/login` → 重载后再次
 * 先渲染 AppShell，形成整页刷新循环（登录页输入框被反复重置）。
 *
 * 初次导航失败（如懒加载 chunk 网络失败）仍继续挂载：显示界面/错误态优于静默白屏
 * 加一个未处理的 rejection。
 */

export interface MountableApp {
  mount(selector: string): unknown
}

export interface ReadyRouter {
  isReady(): Promise<unknown>
}

export async function mountWhenRouterReady(app: MountableApp, router: ReadyRouter): Promise<void> {
  try {
    await router.isReady()
  } catch (error) {
    // 初次导航失败不阻断挂载（降级：至少渲染界面，而不是白屏 + 未处理 rejection）
    console.warn('initial navigation failed, mounting anyway', error)
  }
  app.mount('#app')
}
