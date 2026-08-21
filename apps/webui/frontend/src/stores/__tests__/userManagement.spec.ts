import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useUserManagementStore } from '../userManagement'
import { usersApi } from '@/api/users'

// P3 任务6：userManagement 原型残留清理。
// 目标：createUser 失败不再本地插假用户（Date.now() id）/removeUser 不再乐观先删后调 API
// ——二者返回 boolean，由视图按真实结果反馈；store 不得产生与服务端不一致的幽灵用户/幽灵删除。

vi.mock('@/api/users', () => ({
  usersApi: {
    list: vi.fn(),
    create: vi.fn(),
    update: vi.fn(),
    remove: vi.fn(),
    updateStatus: vi.fn(),
    resetPassword: vi.fn(),
    getPermissions: vi.fn(),
    updatePermissions: vi.fn(),
    me: vi.fn(),
    get: vi.fn(),
  },
}))

vi.mock('@/api/security', () => ({
  securityApi: {
    getConfig: vi.fn(),
    setConfig: vi.fn(),
    listBlacklist: vi.fn(),
    listWhitelist: vi.fn(),
    addBlacklist: vi.fn(),
    addWhitelist: vi.fn(),
    removeBlacklist: vi.fn(),
    removeWhitelist: vi.fn(),
    loginHistory: vi.fn(),
  },
}))

function mockUser(over = {}): Record<string, unknown> {
  return {
    id: 7,
    username: 'alice',
    display_name: 'Alice',
    email: 'a@x.io',
    role: 'user',
    status: 'offline',
    created_at: '2026-01-01',
    ...over,
  }
}

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
})

describe('useUserManagementStore (P3 任务6 原型残留清理)', () => {
  describe('createUser', () => {
    it('成功：用服务端返回的真实 id 追加用户，返回 true', async () => {
      vi.mocked(usersApi.create).mockResolvedValue(mockUser() as never)
      const store = useUserManagementStore()
      const ok = await store.createUser({
        username: 'alice', display_name: 'Alice', email: 'a@x.io',
        password: 'p', role: 'user', accountPermissions: ['CTP-主力账户'], strategyPermissions: [],
      })
      expect(ok).toBe(true)
      expect(store.users).toHaveLength(1)
      expect(store.users[0].id).toBe(7)  // 服务端 id, 非 Date.now()
    })

    it('失败：不插入假用户（无 Date.now() 幽灵 id），返回 false 并置 error', async () => {
      vi.mocked(usersApi.create).mockRejectedValue(new Error('boom'))
      const store = useUserManagementStore()
      const ok = await store.createUser({
        username: 'ghost', display_name: 'Ghost', email: '',
        password: 'p', role: 'user', accountPermissions: [], strategyPermissions: [],
      })
      expect(ok).toBe(false)
      expect(store.users).toHaveLength(0)  // 不产生本地假用户
      expect(store.error).toBeTruthy()
    })
  })

  describe('removeUser', () => {
    it('成功：先调 API 成功后再从本地移除，返回 true', async () => {
      vi.mocked(usersApi.remove).mockResolvedValue(undefined as never)
      const store = useUserManagementStore()
      store.users = [{ ...mockUser() } as never]
      const ok = await store.removeUser(7)
      expect(ok).toBe(true)
      expect(store.users).toHaveLength(0)
      expect(usersApi.remove).toHaveBeenCalledWith(7)
    })

    it('失败：API 失败则保留用户（不回滚出幽灵删除），返回 false 并置 error', async () => {
      vi.mocked(usersApi.remove).mockRejectedValue(new Error('boom'))
      const store = useUserManagementStore()
      store.users = [{ ...mockUser() } as never]
      const ok = await store.removeUser(7)
      expect(ok).toBe(false)
      expect(store.users).toHaveLength(1)  // 保留, 与服务端一致
      expect(store.error).toBeTruthy()
    })
  })
})