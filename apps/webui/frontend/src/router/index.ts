import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { appRoutes } from './routes'

const router = createRouter({
  history: createWebHistory(),
  routes: appRoutes,
})

router.beforeEach((to) => {
  const auth = useAuthStore()
  auth.restoreUser()
  if (!to.meta.public && !auth.isAuthenticated) {
    return { name: 'login' }
  }
  if (to.meta.requiresAdmin && auth.user?.role !== 'admin') {
    return { name: 'dashboard' }
  }
  if (to.name === 'login' && auth.isAuthenticated) {
    return { name: 'dashboard' }
  }
})

export default router
