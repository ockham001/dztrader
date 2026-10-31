import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useUserManagementStore } from '../userManagement'
import { usersApi } from '@/api/users'
import { securityApi } from '@/api/security'
import type { SecurityConfig } from '@/types/api'

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
        password: 'p', role: 'user',
      })
      expect(ok).toBe(true)
      expect(store.users).toHaveLength(1)
      expect(store.users[0].id).toBe(7)  // 服务端 id, 非 Date.now()
      // #205 权限功能下线：创建用户不再携带任何权限字段
      expect(usersApi.create).toHaveBeenCalledWith({
        username: 'alice', display_name: 'Alice', email: 'a@x.io',
        password: 'p', role: 'user',
      })
    })

    it('失败：不插入假用户（无 Date.now() 幽灵 id），返回 false 并置 error', async () => {
      vi.mocked(usersApi.create).mockRejectedValue(new Error('boom'))
      const store = useUserManagementStore()
      const ok = await store.createUser({
        username: 'ghost', display_name: 'Ghost', email: '',
        password: 'p', role: 'user',
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

    it('删除进行中二次调用不重复调 API（removePending 防重入）', async () => {
      let resolve!: () => void
      vi.mocked(usersApi.remove).mockImplementation(() => new Promise(res => { resolve = res }))
      const store = useUserManagementStore()
      store.users = [{ ...mockUser(), removePending: false } as never]
      const p1 = store.removeUser(7)
      const ok2 = await store.removeUser(7)
      expect(ok2).toBe(true)      // 幂等无操作, 不重复调 API
      expect(usersApi.remove).toHaveBeenCalledTimes(1)
      resolve()
      await p1
      expect(store.users).toHaveLength(0)  // 首次删除完成后移除
    })

    it('失败后恢复 removePending=false（可重试, 与服务端一致保留用户）', async () => {
      vi.mocked(usersApi.remove).mockRejectedValue(new Error('boom'))
      const store = useUserManagementStore()
      store.users = [{ ...mockUser(), removePending: false } as never]
      const p = store.removeUser(7)
      expect(store.users[0].removePending).toBe(true)  // 立即置 pending
      const ok = await p
      expect(ok).toBe(false)
      expect(store.users[0].removePending).toBe(false) // 失败恢复
      expect(store.users).toHaveLength(1)
    })
  })

  describe('toggleLoginLockout（安全开关）', () => {
    // 仅本组使用 fake timers（minPendingMs 300）；不改变既有 createUser/removeUser 运行环境
    beforeEach(() => {
      vi.useFakeTimers()
    })
    afterEach(() => {
      vi.useRealTimers()
    })

    it('成功：全量提交 setConfig，minDelay 后应用服务端返回并清错误', async () => {
      const updated: SecurityConfig = {
        login_lockout_enabled: true,
        access_mode: 'auto',
        max_failed_attempts: 5,
        lockout_duration_sec: 1800,
      }
      vi.mocked(securityApi.setConfig).mockResolvedValue(updated)
      const store = useUserManagementStore()
      const p = store.toggleLoginLockout(true)
      expect(store.securityConfig.lockoutPending).toBe(true)
      // 契约 rest：PUT /api/security/config 全量下发（当前值 + 目标开关）
      expect(securityApi.setConfig).toHaveBeenCalledWith({
        login_lockout_enabled: true,
        access_mode: 'auto',
        max_failed_attempts: 5,
        lockout_duration_sec: 1800,
      })
      await vi.advanceTimersByTimeAsync(300)
      await p
      expect(store.securityConfig.login_lockout_enabled).toBe(true)
      expect(store.securityConfig.lockoutPending).toBe(false)
      expect(store.securityConfig.lockoutError).toBeNull()
    })

    it('失败：清 pending、置 lockoutError，开关保持原值', async () => {
      vi.mocked(securityApi.setConfig).mockRejectedValue(new Error('503'))
      const store = useUserManagementStore()
      const p = store.toggleLoginLockout(true)
      await vi.advanceTimersByTimeAsync(300)
      await p
      expect(store.securityConfig.login_lockout_enabled).toBe(false)  // 非乐观, 不回滚出假状态
      expect(store.securityConfig.lockoutPending).toBe(false)
      expect(store.securityConfig.lockoutError).toBe('同步失败')
    })

    it('pending 中再次调用不重复发请求（防重入）', async () => {
      let resolveConfig!: (v: SecurityConfig) => void
      vi.mocked(securityApi.setConfig).mockImplementation(
        () => new Promise<SecurityConfig>(res => { resolveConfig = res }))
      const store = useUserManagementStore()
      const p = store.toggleLoginLockout(true)
      await store.toggleLoginLockout(false)  // 挂起期间直接返回
      expect(securityApi.setConfig).toHaveBeenCalledTimes(1)
      resolveConfig({
        login_lockout_enabled: true,
        access_mode: 'auto',
        max_failed_attempts: 5,
        lockout_duration_sec: 1800,
      })
      await vi.advanceTimersByTimeAsync(300)
      await p
      expect(store.securityConfig.lockoutPending).toBe(false)
    })
  })
})
