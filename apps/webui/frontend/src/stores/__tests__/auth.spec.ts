import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useAuthStore } from '../auth'
import { usersApi } from '@/api/users'
import type { User } from '@/types/api'

// Mock API module（auth store 的 refreshCurrentUser 走 usersApi.me）
vi.mock('@/api/users', () => ({
  usersApi: { me: vi.fn() },
}))

const mockedUsersApi = vi.mocked(usersApi)

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