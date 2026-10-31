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

  it('login 成功：写 token/user/localStorage/默认口令告警并复位 loading', async () => {
    const user = makeUser()
    mockedAuthApi.login.mockResolvedValue({
      token: 'jwt-1', expires_in: 3600, user,
      default_password_warning: { message: '后端下发的告警文案' },
      must_change_password: true,
    })
    const auth = useAuthStore()
    const resp = await auth.login({ username: 'regular', password: 'p' })
    expect(resp.token).toBe('jwt-1')
    expect(mockedAuthApi.login).toHaveBeenCalledWith({ username: 'regular', password: 'p' })
    expect(auth.token).toBe('jwt-1')
    expect(auth.user).toEqual(user)
    // 契约 rest §2.1：告警文案原样取自后端，store 不拼接、不硬编码
    expect(auth.defaultPasswordWarning).toBe('后端下发的告警文案')
    expect(auth.mustChangePassword).toBe(true)
    expect(auth.isAuthenticated).toBe(true)
    expect(auth.loading).toBe(false)
    expect(auth.error).toBeNull()
    expect(localStorage.getItem('jwt_token')).toBe('jwt-1')
    expect(JSON.parse(localStorage.getItem('user_info')!)).toEqual(user)
  })

  it('login 失败：保留 ApiError、复位 loading 并重新抛出', async () => {
    const apiError = new ApiError(401, {
      error: 'invalid credentials',
      code: 'invalid_credentials',
    })
    mockedAuthApi.login.mockRejectedValue(apiError)
    const auth = useAuthStore()
    auth.token = 'existing-token'  // 已有会话：失败不得写/清 token
    await expect(auth.login({ username: 'regular', password: 'bad' })).rejects.toBe(apiError)
    // ApiError 分支按 code 映射中文（与登录页共用单一真相源），
    // 不回显英文 error 消息，也不展示机器码
    expect(auth.error).toBe('用户名或密码错误')
    expect(auth.token).toBe('existing-token')  // 失败不写 token
    expect(auth.loading).toBe(false)
    expect(localStorage.getItem('jwt_token')).toBeNull()
  })

  it('login 无告警时：defaultPasswordWarning 为 null、mustChangePassword 为 false', async () => {
    mockedAuthApi.login.mockResolvedValue({
      token: 'jwt-2', expires_in: 3600, user: makeUser(),
      default_password_warning: null, must_change_password: false,
    })
    const auth = useAuthStore()
    await auth.login({ username: 'regular', password: 'p' })
    expect(auth.defaultPasswordWarning).toBeNull()
    expect(auth.mustChangePassword).toBe(false)
  })

  it('mustChangePassword 与 token 一起持久化（刷新后不丢，避免连 WS 被拒→登出循环）', async () => {
    mockedAuthApi.login.mockResolvedValue({
      token: 'jwt-3', expires_in: 3600, user: makeUser(), must_change_password: true,
    })
    const auth = useAuthStore()
    await auth.login({ username: 'regular', password: 'p' })
    expect(localStorage.getItem('must_change_password')).toBe('1')

    // 重新创建 store（模拟刷新页面）：从 localStorage 恢复
    setActivePinia(createPinia())
    const restored = useAuthStore()
    expect(restored.mustChangePassword).toBe(true)
  })

  it('clearMustChangePassword 与 logout 都清除持久化标记', () => {
    localStorage.setItem('must_change_password', '1')
    const auth = useAuthStore()
    auth.mustChangePassword = true
    auth.clearMustChangePassword()
    expect(auth.mustChangePassword).toBe(false)
    expect(localStorage.getItem('must_change_password')).toBeNull()

    localStorage.setItem('must_change_password', '1')
    auth.mustChangePassword = true
    auth.logout()
    expect(localStorage.getItem('must_change_password')).toBeNull()
  })

  it('logout：清 token/user/告警态/强制改密标记与 localStorage', () => {
    localStorage.setItem('jwt_token', 'jwt-1')
    localStorage.setItem('user_info', JSON.stringify(makeUser()))
    const auth = useAuthStore()
    auth.user = makeUser()
    auth.defaultPasswordWarning = 'some warning'
    auth.mustChangePassword = true
    auth.logout()
    expect(auth.token).toBeNull()
    expect(auth.user).toBeNull()
    expect(auth.defaultPasswordWarning).toBeNull()
    expect(auth.mustChangePassword).toBe(false)
    expect(auth.isAuthenticated).toBe(false)
    expect(localStorage.getItem('jwt_token')).toBeNull()
    expect(localStorage.getItem('user_info')).toBeNull()
  })
})
