import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useAuthStore } from '../auth'
import { usersApi } from '@/api/users'
import { authApi } from '@/api/auth'
import { ApiError } from '@/api/client'
import type { User } from '@/types/api'

// Mock API module（auth store 的 refreshCurrentUser 走 usersApi.me；login 走 authApi.login）
vi.mock('@/api/users', () => ({
  usersApi: { me: vi.fn() },
}))
vi.mock('@/api/auth', () => ({
  authApi: { login: vi.fn() },
}))

const mockedUsersApi = vi.mocked(usersApi)
const mockedAuthApi = vi.mocked(authApi)

function makeUser(overrides: Partial<User> = {}): User {
  return {
    id: 1,
    username: 'regular',
    display_name: 'Regular',
    email: 'reg@test.com',
    role: 'user',
    status: 'online',
    last_login_at: '',
    last_login_ip: '',
    created_at: '',
    ...overrides,
  }
}

describe('useAuthStore.refreshCurrentUser（角色降级后前端缓存同步）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    localStorage.clear()
    mockedUsersApi.me.mockReset()
  })
  afterEach(() => {
    vi.clearAllMocks()
  })

  it('未登录（无 token）时不发起请求', async () => {
    const auth = useAuthStore()
    await auth.refreshCurrentUser()
    expect(mockedUsersApi.me).not.toHaveBeenCalled()
  })

  it('成功后更新 user 与 localStorage，isAdmin 联动', async () => {
    localStorage.setItem('jwt_token', 't')
    localStorage.setItem('user_info', JSON.stringify(makeUser({ role: 'admin' })))
    const auth = useAuthStore()
    auth.restoreUser()  // 从 localStorage 快照恢复 user（模拟登录后清单/降级前状态）
    expect(auth.isAdmin).toBe(true)

    // 后端角色已降为 user → me 返回最新 role
    mockedUsersApi.me.mockResolvedValue(makeUser({ role: 'user' }))
    await auth.refreshCurrentUser()

    expect(auth.user?.role).toBe('user')
    expect(auth.isAdmin).toBe(false)  // 角色缓存随服务端刷新
    const cached = JSON.parse(localStorage.getItem('user_info')!) as User
    expect(cached.role).toBe('user')
  })

  it('刷新失败保持本地缓存（尽力而为）', async () => {
    localStorage.setItem('jwt_token', 't')
    localStorage.setItem('user_info', JSON.stringify(makeUser({ role: 'admin' })))
    const auth = useAuthStore()
    auth.restoreUser()
    expect(auth.isAdmin).toBe(true)

    mockedUsersApi.me.mockRejectedValue(new Error('network'))
    await auth.refreshCurrentUser()

    expect(auth.user?.role).toBe('admin')  // 失败不清空
    expect(auth.isAdmin).toBe(true)
  })
})

describe('useAuthStore.login / logout（主登录链路）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    localStorage.clear()
    mockedAuthApi.login.mockReset()
  })
  afterEach(() => {
    vi.clearAllMocks()
  })

  it('login 成功：写 token/user/localStorage/isDefaultPassword 并复位 loading', async () => {
    const user = makeUser()
    mockedAuthApi.login.mockResolvedValue({
      token: 'jwt-1', expires_in: 3600, user, is_default_password: true,
    })
    const auth = useAuthStore()
    const resp = await auth.login({ username: 'regular', password: 'p' })
    expect(resp.token).toBe('jwt-1')
    expect(mockedAuthApi.login).toHaveBeenCalledWith({ username: 'regular', password: 'p' })
    expect(auth.token).toBe('jwt-1')
    expect(auth.user).toEqual(user)
    expect(auth.isDefaultPassword).toBe(true)
    expect(auth.isAuthenticated).toBe(true)
    expect(auth.loading).toBe(false)
    expect(auth.error).toBeNull()
    expect(localStorage.getItem('jwt_token')).toBe('jwt-1')
    expect(JSON.parse(localStorage.getItem('user_info')!)).toEqual(user)
  })

  it('login 失败：ApiError.body.error 映射 error、复位 loading 并重新抛出', async () => {
    const apiError = new ApiError(401, { error: 'invalid_credentials' })
    mockedAuthApi.login.mockRejectedValue(apiError)
    const auth = useAuthStore()
    await expect(auth.login({ username: 'regular', password: 'bad' })).rejects.toBe(apiError)
    expect(auth.error).toBe('invalid_credentials')
    expect(auth.token).toBeNull()  // 失败不写 token
    expect(auth.loading).toBe(false)
    expect(localStorage.getItem('jwt_token')).toBeNull()
  })

  it('logout：清 token/user/isDefaultPassword 与 localStorage', () => {
    localStorage.setItem('jwt_token', 'jwt-1')
    localStorage.setItem('user_info', JSON.stringify(makeUser()))
    const auth = useAuthStore()
    auth.user = makeUser()
    auth.isDefaultPassword = true
    auth.logout()
    expect(auth.token).toBeNull()
    expect(auth.user).toBeNull()
    expect(auth.isDefaultPassword).toBe(false)
    expect(auth.isAuthenticated).toBe(false)
    expect(localStorage.getItem('jwt_token')).toBeNull()
    expect(localStorage.getItem('user_info')).toBeNull()
  })
})