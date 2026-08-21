import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { authApi } from '@/api/auth'
import { usersApi } from '@/api/users'
import { ApiError } from '@/api/client'
import type { User, LoginRequest } from '@/types/api'

export const useAuthStore = defineStore('auth', () => {
  const token = ref<string | null>(localStorage.getItem('jwt_token'))
  const user = ref<User | null>(null)
  const loading = ref(false)
  const error = ref<string | null>(null)
  const isDefaultPassword = ref(false)

  const isAuthenticated = computed(() => !!token.value)
  const isAdmin = computed(() => user.value?.role === 'admin')

  async function login(credentials: LoginRequest) {
    loading.value = true
    error.value = null
    try {
      const resp = await authApi.login(credentials)
      token.value = resp.token
      user.value = resp.user
      isDefaultPassword.value = !!resp.is_default_password
      localStorage.setItem('jwt_token', resp.token)
      localStorage.setItem('user_info', JSON.stringify(resp.user))
      return resp
    } catch (e: unknown) {
      if (e instanceof ApiError) {
        error.value = e.body?.error || e.message
      } else if (e instanceof Error) {
        error.value = e.message
      } else {
        error.value = 'unknown error'
      }
      throw e
    } finally {
      loading.value = false
    }
  }

  function logout() {
    token.value = null
    user.value = null
    isDefaultPassword.value = false
    localStorage.removeItem('jwt_token')
    localStorage.removeItem('user_info')
  }

  function restoreUser() {
    const stored = localStorage.getItem('user_info')
    if (stored) {
      try {
        user.value = JSON.parse(stored) as User
      } catch {
        user.value = null
      }
    }
    // restoreUser 不恢复 isDefaultPassword(需要重新登录就知道)
  }

  // 从后端刷新当前登录用户（WS 重连成功后调用）：同步角色降级等服务端变更，
  // 使 isAdmin 实时联动（与后端踢降级用户 WS 连接配套）。失败保持现状（尽力而为）。
  async function refreshCurrentUser(): Promise<void> {
    if (!token.value) return
    try {
      const fresh = await usersApi.me()
      user.value = fresh
      localStorage.setItem('user_info', JSON.stringify(fresh))
    } catch {
      // 刷新失败保持本地缓存；401 已由 client.ts 统一跳转 /login
    }
  }

  return { token, user, loading, error, isDefaultPassword, isAuthenticated, isAdmin, login, logout, restoreUser, refreshCurrentUser }
})
