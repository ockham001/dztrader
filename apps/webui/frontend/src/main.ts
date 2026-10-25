import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import router from './router'
import { mountWhenRouterReady } from './bootstrap'
import './composables/wsHandlers'
import './assets/css/design-tokens.css'
import './assets/css/theme.css'
import './assets/css/components.css'
import './assets/css/layout.css'
import './assets/css/dashboard.css'

const app = createApp(App)
app.use(createPinia())
app.use(router)
// 等首屏路由解析完再挂载：否则首帧 route.meta 为空，App.vue 会先渲染 AppShell，
// 顶栏挂载即发起需要登录态的请求 → 401 → 整页跳转 /login → 重载后又先渲染 AppShell，
// 形成整页刷新循环。挂载时机与降级见 bootstrap.ts（issue #118）。
void mountWhenRouterReady(app, router)
