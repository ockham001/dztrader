import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import { setActivePinia, createPinia } from 'pinia'
import { useMdConfigStore, MD_CONFIG_OPS } from '../mdConfig'
import { usePending, __resetForTests, PENDING_TIMEOUT } from '@/composables/usePending'
import { useProgressStore } from '@/stores/progress'
import { marketSourcesApi } from '@/api/marketSources'

// Mock API module（mdConfig store 的所有操作走 marketSourcesApi）
vi.mock('@/api/marketSources', () => ({
  marketSourcesApi: {
    login: vi.fn(),
    logout: vi.fn(),
    setAutoLogin: vi.fn(),
    addBroker: vi.fn(),
    removeBroker: vi.fn(),
    updateBroker: vi.fn(),
    setCurrentBroker: vi.fn(),
    updateFrontends: vi.fn(),
  },
}))

// usePending 是模块级状态（pending + timers Map），每个用例前重置；
// 超时路径会调用 useToastStore，需要 active pinia

const configPayload = {
  brokers: [
    {
      name: 'b1',
      broker_id: 'bid1',
      user_id: 'uid1',
      password: 'pwd1',
      product_info: 'pi1',
      frontends: [{ address: 'a1', label: 'L1', enabled: false }],
    },
  ],
  current_broker_name: 'b1',
}

const autoLoginPayload = {
  enabled: true,
  schedules: [{ login_time: '09:00', logout_time: '15:00' }],
}

describe('useMdConfigStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetForTests()
    vi.clearAllMocks()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  describe('applyMdConfig', () => {
    it('写入完整条目（source → MdConfigPayload）', () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      expect(store.configs['dzmd_ctp']).toEqual(configPayload)
    })

    it('后到完整覆盖先到', () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      store.applyMdConfig({
        source: 'dzmd_ctp',
        config: { brokers: [], current_broker_name: '' },
      })
      expect(store.configs['dzmd_ctp']).toEqual({ brokers: [], current_broker_name: '' })
    })

    it('不同 source 互不影响', () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      store.applyMdConfig({ source: 'dzmd_other', config: { brokers: [], current_broker_name: '' } })
      expect(Object.keys(store.configs)).toEqual(['dzmd_ctp', 'dzmd_other'])
      expect(store.configs['dzmd_ctp'].current_broker_name).toBe('b1')
    })

    it('非法 payload（缺 source/config/null）忽略不写', () => {
      const store = useMdConfigStore()
      store.applyMdConfig({} as never)
      store.applyMdConfig({ source: 'dzmd_ctp' } as never)
      store.applyMdConfig({ config: configPayload } as never)
      store.applyMdConfig(null as never)
      expect(Object.keys(store.configs)).toHaveLength(0)
    })

    it('到达时批量清全部配置类 pending（对照 setMdConfig 清 11 字段语义）', async () => {
      // 先通过一次操作记录 name→id 映射（login 只记录映射, 挂起 login pending）
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')

      // 手动挂起全部配置类 op + login + process 领域 key + 其他 source 的 key
      const { run } = usePending()
      for (const op of MD_CONFIG_OPS) {
        await run(`source:1:${op}`, () => Promise.resolve('x'))
      }
      await run('source:1:login', () => Promise.resolve('x'))   // 应保留（applyMdStatus 负责）
      await run('source:1:start', () => Promise.resolve('x'))   // process 领域, 应保留
      await run('source:2:auto_login', () => Promise.resolve('x'))  // 其他 source, 应保留

      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })

      for (const op of MD_CONFIG_OPS) {
        // clearByPrefix 语义: delete 条目 → key 不存在 (undefined)
        expect(usePending().pending[`source:1:${op}`]).toBeUndefined()
      }
      expect(usePending().pending['source:1:login']).toBe(true)       // 配置类批量清不含 login
      expect(usePending().pending['source:1:start']).toBe(true)       // 不误伤 process store 领域
      expect(usePending().pending['source:2:auto_login']).toBe(true)  // 不影响其他 source
    })

    it('未经过本 store 操作的 source（无 name→id 映射）：镜像照写, 不清 pending', async () => {
      const { run } = usePending()
      await run('source:9:auto_login', () => Promise.resolve('x'))
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'unknown_src', config: configPayload })
      expect(store.configs['unknown_src']).toEqual(configPayload)
      expect(usePending().pending['source:9:auto_login']).toBe(true)
    })
  })

  describe('applyMdStatus', () => {
    it('写入完整条目（source → MdRtnStatusPayload）', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({ source: 'dzmd_ctp', status: { login_state: 'online' } })
      expect(store.statuses['dzmd_ctp']).toEqual({ source: 'dzmd_ctp', status: { login_state: 'online' } })
    })

    it('后到完整覆盖先到', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({ source: 'dzmd_ctp', status: { login_state: 'online' } })
      store.applyMdStatus({ source: 'dzmd_ctp', status: { login_state: 'offline' } })
      expect(store.statuses['dzmd_ctp'].status.login_state).toBe('offline')
    })

    it('写镜像但不清 pending（契约 09 无状态字段；pending 由 RTN_PROGRESS 驱动）', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:login']).toBe(true)

      store.applyMdStatus({ source: 'dzmd_ctp', status: { api_version: 'v6.7.2' } })
      await vi.advanceTimersByTimeAsync(0)
      // 仅写镜像, 不清 login pending
      expect(store.statuses['dzmd_ctp'].status.api_version).toBe('v6.7.2')
      expect(usePending().pending['source:1:login']).toBe(true)
    })

    it('非法 payload（缺 source/null）忽略不写', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({} as never)
      store.applyMdStatus(null as never)
      expect(Object.keys(store.statuses)).toHaveLength(0)
    })
  })

  describe('applyAutoLogin（契约 04）', () => {
    it('写入完整条目（source → {enabled, schedules}）', () => {
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      expect(store.autoLogins['dzmd_ctp']).toEqual(autoLoginPayload)
    })

    it('后到完整覆盖先到', () => {
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })
      expect(store.autoLogins['dzmd_ctp']).toEqual({ enabled: false, schedules: [] })
    })

    it('非法 payload（非对象/缺字段）忽略不写', () => {
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', null)
      store.applyAutoLogin('dzmd_ctp', { enabled: true })
      store.applyAutoLogin('dzmd_ctp', { schedules: [] })
      expect(Object.keys(store.autoLogins)).toHaveLength(0)
    })

    it('到达时批量清排程类 pending（auto_login/schedule_add/schedule_remove）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      await store.addSchedule(1, 'dzmd_ctp', '09:00', '15:00')
      expect(usePending().pending['source:1:auto_login']).toBe(true)
      expect(usePending().pending['source:1:schedule_add']).toBe(true)

      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:auto_login']).toBeUndefined()
      expect(usePending().pending['source:1:schedule_add']).toBeUndefined()
      // 不误伤 broker 类 pending
      const { run } = usePending()
      await run('source:1:broker_add', () => Promise.resolve('x'))
      expect(usePending().pending['source:1:broker_add']).toBe(true)
    })
  })

  describe('login / logout', () => {
    it('login: HTTP 成功不清 pending（等 RTN_PROGRESS "已登录" 清）', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const result = await store.login(1, 'dzmd_ctp')
      expect(result).toBe(true)
      expect(usePending().pending['source:1:login']).toBe(true)

      // 契约 05: dzmd_ctp 状态转移推送 RTN_PROGRESS, 数值映射 current==max=4 (LoggedIn)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      await nextTick()
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:login']).toBeUndefined()
    })

    it('login: HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.login).mockRejectedValue(new Error('login failed'))
      const store = useMdConfigStore()
      const result = await store.login(1, 'dzmd_ctp')
      expect(result).toBe(false)
      expect(usePending().pending['source:1:login']).toBe(false)
    })

    it('logout: HTTP 成功不清 pending（等 RTN_PROGRESS "未登录" 清）', async () => {
      vi.mocked(marketSourcesApi.logout).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const result = await store.logout(1, 'dzmd_ctp')
      expect(result).toBe(true)
      expect(usePending().pending['source:1:logout']).toBe(true)

      // 契约 05: dzmd_ctp 状态转移推送 RTN_PROGRESS, 数值映射 current==min=0 (Idle)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 0 })
      await nextTick()
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:logout']).toBeUndefined()
    })

    it('中间态 progress（数值映射 1..3）不清 pending', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:login']).toBe(true)

      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2 })
      await nextTick()
      expect(usePending().pending['source:1:login']).toBe(true)
    })

    it('配置类操作超时兜底（10s）清 pending 并返回 PENDING_TIMEOUT', async () => {
      vi.mocked(marketSourcesApi.login).mockImplementation(() => new Promise(() => {}))
      const store = useMdConfigStore()
      const p = store.login(1, 'dzmd_ctp')
      expect(usePending().pending['source:1:login']).toBe(true)
      await vi.advanceTimersByTimeAsync(10_001)
      expect(usePending().pending['source:1:login']).toBe(false)
      const result = await p
      expect(result).toBe(PENDING_TIMEOUT)  // 超时供薄代理识别去重双 toast
    })

    it('同 key 已 pending 时防重入：不重复发 HTTP 并返回 false', async () => {
      vi.mocked(marketSourcesApi.login).mockImplementation(() => new Promise(() => {}))
      const store = useMdConfigStore()
      const p1 = store.login(1, 'dzmd_ctp')
      const p2 = store.login(1, 'dzmd_ctp')
      await vi.advanceTimersByTimeAsync(0)  // 冲刷微任务，让 p1 的 HTTP 调用发出
      expect(marketSourcesApi.login).toHaveBeenCalledTimes(1)
      expect(await p2).toBe(false)
      // 收尾：推进 10s 让 p1 走超时兜底 settle，不残留挂起 promise/timer
      await vi.advanceTimersByTimeAsync(10_001)
      expect(await p1).toBe(PENDING_TIMEOUT)
      expect(usePending().pending['source:1:login']).toBe(false)
    })
  })

  describe('toggleAutoLogin / schedule（契约 04: 全量提交 SET_AUTO_LOGIN）', () => {
    it('toggleAutoLogin: HTTP 成功不清 pending（等 applyAutoLogin 清）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const result = await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      expect(result).toBe(true)
      expect(usePending().pending['source:1:auto_login']).toBe(true)
      // 全量提交: 镜像未建立时 enabled=true, schedules=[]
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledWith(1, { enabled: true, schedules: [] })

      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:auto_login']).toBeUndefined()
    })

    it('toggleAutoLogin: HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockRejectedValue(new Error('toggle failed'))
      const store = useMdConfigStore()
      expect(await store.toggleAutoLogin(1, 'dzmd_ctp', true)).toBe(false)
      expect(usePending().pending['source:1:auto_login']).toBe(false)
    })

    it('toggleAutoLogin: HTTP 失败回滚乐观更新（镜像不残留虚假值）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockRejectedValue(new Error('503'))
      const store = useMdConfigStore()
      // 先建立镜像 enabled=false
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })
      // 乐观更新为 true 后 HTTP 失败 → 回滚
      expect(await store.toggleAutoLogin(1, 'dzmd_ctp', true)).toBe(false)
      expect(store.autoLogins['dzmd_ctp'].enabled).toBe(false)
      // 镜像未建立时失败: 不残留条目
      expect(await store.toggleAutoLogin(1, 'dzmd_xtp', true)).toBe(false)
      expect(store.autoLogins['dzmd_xtp']).toBeUndefined()
    })

    it('addSchedule: 重复时段幂等成功（不下发）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)  // 已含 09:00-15:00
      expect(await store.addSchedule(1, 'dzmd_ctp', '09:00', '15:00')).toBe(true)
      expect(marketSourcesApi.setAutoLogin).not.toHaveBeenCalled()
      expect(usePending().pending['source:1:schedule_add']).toBeUndefined()
    })

    it('addSchedule / removeSchedule 独立 pending（互不干扰）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      // 先建立镜像 (全量提交的 schedules 基准)
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await store.addSchedule(1, 'dzmd_ctp', '20:45', '02:30')
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledWith(1, {
        enabled: true,
        schedules: [
          { login_time: '09:00', logout_time: '15:00' },
          { login_time: '20:45', logout_time: '02:30' },
        ],
      })
      expect(usePending().pending['source:1:schedule_add']).toBe(true)

      // schedule_add 挂起时 schedule_remove 仍可发起（现有 spec: 时段增删独立等待状态）
      const r = await store.removeSchedule(1, 'dzmd_ctp', '09:00', '15:00')
      expect(r).toBe(true)
      expect(marketSourcesApi.setAutoLogin).toHaveBeenLastCalledWith(1, {
        enabled: true,
        schedules: [{ login_time: '20:45', logout_time: '02:30' }],
      })
      expect(usePending().pending['source:1:schedule_remove']).toBe(true)

      // 收尾：applyAutoLogin 批量清（clearByPrefix 语义: delete 条目）
      store.applyAutoLogin('dzmd_ctp', { enabled: true, schedules: [{ login_time: '20:45', logout_time: '02:30' }] })
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:schedule_add']).toBeUndefined()
      expect(usePending().pending['source:1:schedule_remove']).toBeUndefined()
    })
  })

  describe('broker 系列', () => {
    it('addBroker / removeBroker / updateBroker / selectBroker 独立 pending 且由 applyMdConfig 批量清', async () => {
      vi.mocked(marketSourcesApi.addBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.removeBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.updateBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.setCurrentBroker).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.addBroker(1, 'dzmd_ctp', { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' })
      await store.removeBroker(1, 'dzmd_ctp', 'b1')
      await store.updateBroker(1, 'dzmd_ctp', 'b1', { name: 'b1', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi', frontends: [] })
      await store.selectBroker(1, 'dzmd_ctp', 'b1')

      expect(marketSourcesApi.addBroker).toHaveBeenCalledTimes(1)
      expect(marketSourcesApi.removeBroker).toHaveBeenCalledWith(1, 'b1')
      expect(marketSourcesApi.updateBroker).toHaveBeenCalledTimes(1)
      expect(marketSourcesApi.setCurrentBroker).toHaveBeenCalledWith(1, 'b1')
      expect(usePending().pending['source:1:broker_add']).toBe(true)
      expect(usePending().pending['source:1:broker_remove']).toBe(true)
      expect(usePending().pending['source:1:broker_update']).toBe(true)
      expect(usePending().pending['source:1:broker_select']).toBe(true)

      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      await vi.advanceTimersByTimeAsync(0)
      // clearByPrefix 语义: delete 条目 → key 不存在 (undefined)
      expect(usePending().pending['source:1:broker_add']).toBeUndefined()
      expect(usePending().pending['source:1:broker_remove']).toBeUndefined()
      expect(usePending().pending['source:1:broker_update']).toBeUndefined()
      expect(usePending().pending['source:1:broker_select']).toBeUndefined()
    })

    it('broker 操作 HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.addBroker).mockRejectedValue(new Error('add broker failed'))
      const store = useMdConfigStore()
      expect(await store.addBroker(1, 'dzmd_ctp', { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' })).toBe(false)
      expect(usePending().pending['source:1:broker_add']).toBe(false)
    })
  })

  describe('frontend 系列', () => {
    it('addFrontend: 无镜像/无 broker 时不发请求并返回 false', async () => {
      const store = useMdConfigStore()
      const result = await store.addFrontend(1, 'dzmd_ctp', 'nope', 'a9', 'L9')
      expect(result).toBe(false)
      expect(marketSourcesApi.updateFrontends).not.toHaveBeenCalled()
      expect(Object.keys(usePending().pending)).toHaveLength(0)
    })

    it('addFrontend: 从镜像构造新列表（当前列表 + 新项, enabled 默认 false）', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      const result = await store.addFrontend(1, 'dzmd_ctp', 'b1', 'a2', 'L2')
      expect(result).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledWith(1, 'b1', [
        { address: 'a1', label: 'L1', enabled: false },
        { address: 'a2', label: 'L2', enabled: false },
      ])
      expect(usePending().pending['source:1:frontend_add']).toBe(true)
    })

    it('frontend 四个操作独立 pending（add 挂起时 remove 仍可发起）', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      // add 挂起（永不 resolve）
      vi.mocked(marketSourcesApi.updateFrontends).mockImplementationOnce(() => new Promise(() => {}))
      const p = store.addFrontend(1, 'dzmd_ctp', 'b1', 'a2', 'L2')
      await vi.advanceTimersByTimeAsync(0)
      expect(usePending().pending['source:1:frontend_add']).toBe(true)

      // remove 独立发起（spec: 各控件独立等待状态, 可同时发起）
      const r = await store.removeFrontend(1, 'dzmd_ctp', 'b1', 'a1')
      expect(r).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledTimes(2)
      expect(usePending().pending['source:1:frontend_remove']).toBe(true)

      // 收尾：add 走 10s 超时兜底
      await vi.advanceTimersByTimeAsync(10_001)
      expect(await p).toBe(PENDING_TIMEOUT)
      expect(usePending().pending['source:1:frontend_add']).toBe(false)
    })

    it('setFrontendEnabled: 只改目标前置 enabled', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      const result = await store.setFrontendEnabled(1, 'dzmd_ctp', 'b1', 'a1', true)
      expect(result).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledWith(1, 'b1', [
        { address: 'a1', label: 'L1', enabled: true },
      ])
    })

    it('editFrontend: 值未改变不下发并返回 false', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      const result = await store.editFrontend(1, 'dzmd_ctp', 'b1', 'a1', 'a1')
      expect(result).toBe(false)
      expect(marketSourcesApi.updateFrontends).not.toHaveBeenCalled()
    })

    it('editFrontend: 改地址后整体下发', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      const result = await store.editFrontend(1, 'dzmd_ctp', 'b1', 'a1', 'a9')
      expect(result).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledWith(1, 'b1', [
        { address: 'a9', label: 'L1', enabled: false },
      ])
    })
  })
})
