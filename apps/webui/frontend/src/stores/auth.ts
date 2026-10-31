import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { authApi } from '@/api/auth'
import { usersApi } from '@/api/users'
import { ApiError } from '@/api/client'
import { loginErrorText } from '@/composables/loginErrors'
import type { User, LoginRequest } from '@/types/api'

export const useAuthStore = defineStore('auth', () => {
  const token = ref<string | null>(localStorage.getItem('jwt_token'))
  const user = ref<User | null>(null)
  const loading = ref(false)
  const error = ref<string | null>(null)
  // 默认口令告警文案（null = 无告警）。契约 rest §2.1：文案由后端下发，
  // 前端只负责展示——store 里不出现任何硬编码的口令或告警文本（#202）。
  const defaultPasswordWarning = ref<string | null>(null)
  // 契约 rest §1「强制改密」：管理员重置过口令的账户登录后必须先改密。
  // 与 token 一起持久化：刷新页面后若不恢复，App.vue 会照常连 WS，而 WS 对强制改密账户
  // 一律 auth failed → wsClient 误判会话失效 → 登出，用户被锁在「登录→弹窗→刷新→登出」里。
  const mustChangePassword = ref(localStorage.getItem('must_change_password') === '1')

  const isAuthenticated = computed(() => !!token.value)
  const isAdmin = computed(() => user.value?.role === 'admin')

  async function login(credentials: LoginRequest) {
    loading.value = true
    error.value = null
    try {
      const resp = await authApi.login(credentials)
      token.value = resp.token
      user.value = resp.user
      defaultPasswordWarning.value = resp.default_password_warning?.message ?? null
      mustChangePassword.value = !!resp.must_change_password
      localStorage.setItem('jwt_token', resp.token)
      localStorage.setItem('user_info', JSON.stringify(resp.user))
      setMustChangeStorage(mustChangePassword.value)
      return resp
    } catch (e: unknown) {
      if (e instanceof ApiError) {
        // 契约 rest §1/§2.1：错误体 code 是机器码、error 是人类可读英文消息。
        // 本分支按 code 映射中文（与登录页共用 loginErrorText 单一真相源，
        // 缺 code 的 400 等通用失败回落「登录失败」），不回显英文 error。
        error.value = loginErrorText(e.body?.code)
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
    defaultPasswordWarning.value = null
    mustChangePassword.value = false
    localStorage.removeItem('jwt_token')
    localStorage.removeItem('user_info')
    setMustChangeStorage(false)
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
    // restoreUser 不恢复 defaultPasswordWarning（纯提示，刷新后由登录/WS 重新给出）；
    // mustChangePassword 例外——它决定「要不要连 WS / 显示不可关闭的弹窗」，从
    // localStorage 恢复（见上方 ref 初始化）
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

  /** 清除默认口令告警（用户点「我知道了」并 ack 成功后调用） */
  function clearDefaultPasswordWarning(): void {
    defaultPasswordWarning.value = null
  }

  /** 强制改密标记的持久化（与 token 同生命周期） */
  function setMustChangeStorage(on: boolean): void {
    try {
      if (on) {
        localStorage.setItem('must_change_password', '1')
      } else {
        localStorage.removeItem('must_change_password')
      }
    } catch { /* localStorage 不可用时只丢持久化，不影响本次会话 */ }
  }

  /** 自助改密成功后清除强制改密标记（后端已清除，前端同步本地态以便跳转登录） */
  function clearMustChangePassword(): void {
    mustChangePassword.value = false
    setMustChangeStorage(false)
  }

  return {
    token,
    user,
    loading,
    error,
    defaultPasswordWarning,
    mustChangePassword,
    isAuthenticated,
    isAdmin,
    login,
    logout,
    restoreUser,
    refreshCurrentUser,
    clearDefaultPasswordWarning,
    clearMustChangePassword,
  }
})
