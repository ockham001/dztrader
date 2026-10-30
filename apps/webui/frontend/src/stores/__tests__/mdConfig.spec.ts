import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import { setActivePinia, createPinia } from 'pinia'
import { useMdConfigStore, MD_CONFIG_OPS, CLEAR_BY_RTN, MD_CONFIG_ALL_OPS } from '../mdConfig'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import { marketSourcePending, __resetForTests } from '@/composables/marketSourcePending'
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
    setSubscribeParams: vi.fn(),
    setShmConfig: vi.fn(),  // 新增（Task 12）
  },
}))

// 行情源领域共享 pending（marketSourcePending）+ timers，每个用例前重置；
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

// P3 任务4：清理触点声明式门禁——每个 op 都必须登记清理归属，漏登记（→pending 悬挂到
// 超时，操作进度卡死）在此暴露而非线上才发现。
describe('CLEAR_BY_RTN 声明完整性', () => {
  it('每个 MdConfigOp 都声明了由哪个 RTN 清理（不漏登记）', () => {
    const claimed = new Set<string>(CLEAR_BY_RTN.md_config)
    for (const kind of Object.keys(CLEAR_BY_RTN) as (keyof typeof CLEAR_BY_RTN)[]) {
      for (const op of CLEAR_BY_RTN[kind]) claimed.add(op)
    }
    for (const op of MD_CONFIG_ALL_OPS) {
      expect(claimed.has(op), `op「${op}」未登记清理归属`).toBe(true)
    }
  })

  it('各清理类互不重叠（同 op 不得归属多个 RTN）——login 例外：progress 登录完成 + md_status 失败双触点', () => {
    // 契约 md-status 前端义务①：登录失败态 RTN_PROGRESS 数值映射与 Connected 相同（不产生
    // online/offline 转移），因此 login pending 除 progress_login 外还由 last_error.timestamp
    // 变化清理（CLEAR_BY_RTN.md_status_failure）——这是唯一有意登记的跨类双触点。
    for (const kind of Object.keys(CLEAR_BY_RTN) as (keyof typeof CLEAR_BY_RTN)[]) {
      if (kind === 'progress_login' || kind === 'md_status_failure') continue
      for (const op of CLEAR_BY_RTN[kind]) {
        const others = (Object.keys(CLEAR_BY_RTN) as (keyof typeof CLEAR_BY_RTN)[])
          .filter(k => k !== kind)
        for (const other of others) {
          expect(CLEAR_BY_RTN[other].includes(op), `op「${op}」归属了多个清理类`).toBe(false)
        }
      }
    }
    // 双触点必须且仅覆盖 login
    expect(CLEAR_BY_RTN.progress_login).toEqual(['login'])
    expect(CLEAR_BY_RTN.md_status_failure).toEqual(['login'])
  })
})

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

    it('后到完整覆盖先到（可选字段不残留，契约：始终全量）', () => {
      const store = useMdConfigStore()
      // 首帧含全部可选字段；次帧省略 → 若误用 {...old, ...new} 合并, 旧字段会残留
      store.applyMdConfig({ source: 'dzmd_ctp', config: {
        ...configPayload,
        subscribe_batch_size: 500,
        subscribe_batch_delay_ms: 1200,
        sub_check_interval_ms: 3000,
        sub_max_retry: 4,
      }})
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

    it('非法 payload（缺 source/config/null）忽略不写且不清已有镜像', () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      store.applyMdConfig({} as never)
      store.applyMdConfig({ source: 'dzmd_ctp' } as never)
      store.applyMdConfig({ config: configPayload } as never)
      store.applyMdConfig(null as never)
      expect(store.configs['dzmd_ctp']).toEqual(configPayload)
    })

    it('到达时批量清全部配置类 pending（对照 setMdConfig 清 11 字段语义）', async () => {
      // 先通过一次操作记录 name→id 映射（login 只记录映射, 挂起 login pending）
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')

      // 手动挂起全部配置类 op + login + process 领域 key + 其他 source 的 key
      const { run } = marketSourcePending
      for (const op of MD_CONFIG_OPS) {
        await run(`source:1:${op}`, () => Promise.resolve('x'))
      }
      await run('source:1:login', () => Promise.resolve('x'))   // 应保留（applyMdStatus 负责）
      await run('source:1:start', () => Promise.resolve('x'))   // process 领域, 应保留
      await run('source:2:auto_login', () => Promise.resolve('x'))  // 其他 source, 应保留

      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })

      for (const op of MD_CONFIG_OPS) {
        // clearByPrefix 语义: delete 条目 → key 不存在 (undefined)
        expect(marketSourcePending.pending[`source:1:${op}`]).toBeUndefined()
      }
      expect(marketSourcePending.pending['source:1:login']).toBe(true)       // 配置类批量清不含 login
      expect(marketSourcePending.pending['source:1:start']).toBe(true)       // 不误伤 process store 领域
      expect(marketSourcePending.pending['source:2:auto_login']).toBe(true)  // 不影响其他 source
    })

    it('未经过本 store 操作的 source（无 name→id 映射）：镜像照写, 不清 pending', async () => {
      const { run } = marketSourcePending
      await run('source:9:auto_login', () => Promise.resolve('x'))
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'unknown_src', config: configPayload })
      expect(store.configs['unknown_src']).toEqual(configPayload)
      expect(marketSourcePending.pending['source:9:auto_login']).toBe(true)
    })
  })

  describe('applyMdStatus', () => {
    it('写入完整条目（source → MdRtnStatusPayload）', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({ source: 'dzmd_ctp', status: { login_state: 'online' } })
      expect(store.statuses['dzmd_ctp']).toEqual({ source: 'dzmd_ctp', status: { login_state: 'online' } })
    })

    it('后到完整覆盖先到（缺失字段不残留）', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({
        source: 'dzmd_ctp',
        status: { login_state: 'online', api_version: 'v6.7.2' },
      })
      store.applyMdStatus({ source: 'dzmd_ctp', status: { login_state: 'offline' } })
      expect(store.statuses['dzmd_ctp']).toEqual({
        source: 'dzmd_ctp',
        status: { login_state: 'offline' },
      })
    })

    it('写镜像但不清 pending（契约 md-status 无状态字段；pending 由 RTN_PROGRESS 驱动）', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)

      store.applyMdStatus({ source: 'dzmd_ctp', status: { api_version: 'v6.7.2' } })
      await vi.advanceTimersByTimeAsync(0)
      // 仅写镜像, 不清 login pending
      expect(store.statuses['dzmd_ctp'].status.api_version).toBe('v6.7.2')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
    })

    it('非法 payload（缺 source/status/null）忽略不写且不清已有镜像', () => {
      const store = useMdConfigStore()
      store.applyMdStatus({ source: 'dzmd_ctp', status: { api_version: 'v6.7.2' } })
      store.applyMdStatus({} as never)
      store.applyMdStatus({ source: 'dzmd_ctp' } as never)  // 缺 status
      store.applyMdStatus(null as never)
      expect(store.statuses['dzmd_ctp']).toEqual({
        source: 'dzmd_ctp',
        status: { api_version: 'v6.7.2' },
      })
    })
  })

  describe('applyAutoLogin（契约 auto-login）', () => {
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

    it('非法 payload（非对象/缺字段）忽略不写且不清已有镜像', () => {
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      store.applyAutoLogin('dzmd_ctp', null)
      store.applyAutoLogin('dzmd_ctp', { enabled: true })
      store.applyAutoLogin('dzmd_ctp', { schedules: [] })
      expect(store.autoLogins['dzmd_ctp']).toEqual(autoLoginPayload)
    })

    it('到达时批量清排程类 pending（auto_login/schedule_add/schedule_remove）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })  // 建立镜像（守卫要求）
      await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      expect(marketSourcePending.pending['source:1:auto_login']).toBe(true)
      // 域互斥: auto_login 在途时 addSchedule 会被拒 → 先 RTN 清掉 auto_login pending 再发起 addSchedule
      store.applyAutoLogin('dzmd_ctp', { enabled: true, schedules: [] })
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:auto_login']).toBeUndefined()
      await store.addSchedule(1, 'dzmd_ctp', '09:00', '15:00')
      expect(marketSourcePending.pending['source:1:schedule_add']).toBe(true)

      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:auto_login']).toBeUndefined()
      expect(marketSourcePending.pending['source:1:schedule_add']).toBeUndefined()
      // 不误伤 broker 类 pending
      const { run } = marketSourcePending
      await run('source:1:broker_add', () => Promise.resolve('x'))
      expect(marketSourcePending.pending['source:1:broker_add']).toBe(true)
    })
  })

  describe('镜像未就绪守卫（防破坏性全量提交）', () => {
    it('toggleAutoLogin 镜像未就绪时返回 false 且不下发', async () => {
      const store = useMdConfigStore()
      // 不 applyAutoLogin——镜像无 dzmd_ctp 条目
      const r = await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      expect(r).toBe(false)
      expect(vi.mocked(marketSourcesApi.setAutoLogin)).not.toHaveBeenCalled()
    })

    it('addSchedule 镜像未就绪时返回 false 且不下发', async () => {
      const store = useMdConfigStore()
      const r = await store.addSchedule(1, 'dzmd_ctp', '08:45', '15:30')
      expect(r).toBe(false)
      expect(vi.mocked(marketSourcesApi.setAutoLogin)).not.toHaveBeenCalled()
    })

    it('镜像就绪后正常下发（回归）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })
      const r = await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      expect(r).toBe(true)
      expect(vi.mocked(marketSourcesApi.setAutoLogin)).toHaveBeenCalledWith(1, {
        enabled: true,
        schedules: [],
      })
    })
  })

  describe('md_status last_error 驱动登录 pending 清理（契约 md-status 前端义务①）', () => {
    const lastErrorStatus = (timestamp: number) => ({
      source: 'dzmd_ctp',
      status: {
        api_version: 'v6.7.2',
        login_time: '',
        last_error: { code: 'login_rejected', message: 'CTP:用户密码错误', error_id: 3, timestamp },
      },
    })

    it('login() 发起后 pending；last_error 新 timestamp 到达 → 清除该 source 的 login pending', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)

      store.applyMdStatus(lastErrorStatus(1_700_000_000))
      await nextTick()
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:login']).toBeUndefined()
    })

    it('同 timestamp 重复推送不重复清（不清除新一次登录的 pending）', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      store.applyMdStatus(lastErrorStatus(1_700_000_000))
      await nextTick()
      expect(marketSourcePending.pending['source:1:login']).toBeUndefined()

      // 用户重试登录（新 pending），md_status 因订阅统计等变化重复推送同一次失败
      await store.login(1, 'dzmd_ctp')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
      store.applyMdStatus(lastErrorStatus(1_700_000_000))
      await nextTick()
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
    })

    it('last_error 变 null（下一次尝试开始/登录成功）不清 pending', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      store.applyMdStatus({ source: 'dzmd_ctp', status: { last_error: null } })
      await nextTick()
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
    })

    it('无 name→id 映射（未经过本 store 操作）时不清 pending', async () => {
      const { run } = marketSourcePending
      await run('source:9:login', () => Promise.resolve('x'))
      const store = useMdConfigStore()
      store.applyMdStatus({ source: 'unknown_src', status: {
        last_error: { code: 'login_timeout', message: '', error_id: 0, timestamp: 1_700_000_000 },
      } })
      await nextTick()
      expect(marketSourcePending.pending['source:9:login']).toBe(true)
    })
  })

  describe('login / logout', () => {
    it('login: HTTP 成功不清 pending（等 RTN_PROGRESS "已登录" 清）', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const result = await store.login(1, 'dzmd_ctp')
      expect(result).toBe(true)
      expect(marketSourcePending.pending['source:1:login']).toBe(true)

      // 契约 progress: dzmd_ctp 状态转移推送 RTN_PROGRESS, 数值映射 current==max=4 (LoggedIn)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      await nextTick()
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:login']).toBeUndefined()
    })

    it('login: HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.login).mockRejectedValue(new Error('login failed'))
      const store = useMdConfigStore()
      const result = await store.login(1, 'dzmd_ctp')
      expect(result).toBe(false)
      expect(marketSourcePending.pending['source:1:login']).toBe(false)
    })

    it('logout: HTTP 成功不清 pending（等 RTN_PROGRESS "未登录" 清）', async () => {
      vi.mocked(marketSourcesApi.logout).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const result = await store.logout(1, 'dzmd_ctp')
      expect(result).toBe(true)
      expect(marketSourcePending.pending['source:1:logout']).toBe(true)

      // 契约 progress: dzmd_ctp 状态转移推送 RTN_PROGRESS, 数值映射 current==min=0 (Idle)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 0 })
      await nextTick()
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:logout']).toBeUndefined()
    })

    it('中间态 progress（数值映射 1..3）不清 pending', async () => {
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.login(1, 'dzmd_ctp')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)

      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2 })
      await nextTick()
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
    })

    it('登录操作超时兜底（45s：连接 30s + 登录 10s + 余量）清 pending 并返回 PENDING_TIMEOUT', async () => {
      vi.mocked(marketSourcesApi.login).mockImplementation(() => new Promise(() => {}))
      const store = useMdConfigStore()
      const p = store.login(1, 'dzmd_ctp')
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
      // 其他配置类 op 仍是 10s：10s 时 login 不得被误清
      await vi.advanceTimersByTimeAsync(10_001)
      expect(marketSourcePending.pending['source:1:login']).toBe(true)
      await vi.advanceTimersByTimeAsync(34_999)
      expect(marketSourcePending.pending['source:1:login']).toBe(false)
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
      // 收尾：推进 45s 让 p1 走登录超时兜底 settle，不残留挂起 promise/timer
      await vi.advanceTimersByTimeAsync(45_001)
      expect(await p1).toBe(PENDING_TIMEOUT)
      expect(marketSourcePending.pending['source:1:login']).toBe(false)
    })
  })

  describe('toggleAutoLogin / schedule（契约 auto-login: 全量提交 SET_AUTO_LOGIN）', () => {
    it('toggleAutoLogin: HTTP 成功不清 pending（等 applyAutoLogin 清）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })  // 建立镜像（守卫要求）
      const result = await store.toggleAutoLogin(1, 'dzmd_ctp', true)
      expect(result).toBe(true)
      expect(marketSourcePending.pending['source:1:auto_login']).toBe(true)
      // 全量提交: 以镜像为基准翻转 enabled（镜像 schedules=[]）
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledWith(1, { enabled: true, schedules: [] })

      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:auto_login']).toBeUndefined()
    })

    it('toggleAutoLogin: HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockRejectedValue(new Error('toggle failed'))
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })  // 建立镜像（守卫要求）
      expect(await store.toggleAutoLogin(1, 'dzmd_ctp', true)).toBe(false)
      expect(marketSourcePending.pending['source:1:auto_login']).toBe(false)
    })

    it('toggleAutoLogin: HTTP 失败镜像保持原值（非乐观, 不预写镜像）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockRejectedValue(new Error('503'))
      const store = useMdConfigStore()
      // 先建立镜像 enabled=false
      store.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })
      // 非乐观: 提交不预写镜像, 失败时镜像从未被修改
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
      expect(marketSourcePending.pending['source:1:schedule_add']).toBeUndefined()
    })

    it('addSchedule / removeSchedule 域互斥（在途全量提交未回推前拒绝新提交, 防旧基准覆盖）', async () => {
      // setAutoLogin 挂起: addSchedule 提交后 RTN 未回推（镜像仍为旧值）
      const resolvers: ((v: { ok: boolean }) => void)[] = []
      vi.mocked(marketSourcesApi.setAutoLogin).mockImplementation(
        () => new Promise(res => { resolvers.push(res) }))
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)  // 已含 09:00-15:00
      const p = store.addSchedule(1, 'dzmd_ctp', '20:45', '02:30')
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledTimes(1)
      expect(marketSourcePending.pending['source:1:schedule_add']).toBe(true)

      // schedule_add 在途时 removeSchedule 被域互斥拒绝: 不发第二次全量（旧基准会覆盖添加意图）
      const rp = store.removeSchedule(1, 'dzmd_ctp', '09:00', '15:00')
      await vi.advanceTimersByTimeAsync(0)
      // 旧实现（乐观 + 独立 pending）会让 removeSchedule 发出第二次全量并挂起——
      // 若确实发出则 resolve 之, 使下方断言在结果上干净失败而非 5s 超时
      if (resolvers.length >= 2) resolvers[1]({ ok: true })
      expect(await rp).toBe(false)
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledTimes(1)  // 仍只有第一次

      // 收尾: resolve 在途提交; RTN 回推权威覆盖——镜像含新时段, 域 pending 批量清
      for (const res of resolvers) res({ ok: true })
      await p
      store.applyAutoLogin('dzmd_ctp', {
        enabled: true,
        schedules: [
          { login_time: '09:00', logout_time: '15:00' },
          { login_time: '20:45', logout_time: '02:30' },
        ],
      })
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:schedule_add']).toBeUndefined()
      expect(marketSourcePending.pending['source:1:schedule_remove']).toBeUndefined()
    })

    it('removeSchedule 非乐观: HTTP 挂起期间镜像不变（时段仍在, 无假删除）', async () => {
      let resolveRemove!: (v: { ok: boolean }) => void
      vi.mocked(marketSourcesApi.setAutoLogin).mockImplementation(
        () => new Promise(res => { resolveRemove = res }))
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)  // 含 09:00-15:00
      const p = store.removeSchedule(1, 'dzmd_ctp', '09:00', '15:00')
      await vi.advanceTimersByTimeAsync(0)
      // 非乐观: RTN 未回推前镜像保持旧值——09:00 仍在（用户看到的应是"转圈待确认", 不是"立即消失"）
      expect(store.autoLogins['dzmd_ctp'].schedules).toHaveLength(1)
      expect(store.autoLogins['dzmd_ctp'].schedules[0].login_time).toBe('09:00')
      resolveRemove({ ok: true })
      await p
    })

    it('removeSchedule HTTP 失败: 镜像保持原值, 无需回滚（非乐观天然成立）', async () => {
      vi.mocked(marketSourcesApi.setAutoLogin).mockRejectedValue(new Error('net down'))
      const store = useMdConfigStore()
      store.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      expect(await store.removeSchedule(1, 'dzmd_ctp', '09:00', '15:00')).toBe(false)
      // 后台断网失败: 镜像从未被修改, 09:00 保留（与服务端一致）
      expect(store.autoLogins['dzmd_ctp'].schedules).toHaveLength(1)
    })
  })

  describe('broker 系列', () => {
    it('addBroker / removeBroker / updateBroker / selectBroker 独立 pending 且由 applyMdConfig 批量清', async () => {
      vi.mocked(marketSourcesApi.addBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.removeBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.updateBroker).mockResolvedValue({ ok: true })
      vi.mocked(marketSourcesApi.setCurrentBroker).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const addBody = { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' }
      const updateBody = { name: 'b1', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi', frontends: [] }
      await store.addBroker(1, 'dzmd_ctp', addBody)
      await store.removeBroker(1, 'dzmd_ctp', 'b1')
      await store.updateBroker(1, 'dzmd_ctp', 'b1', updateBody)
      await store.selectBroker(1, 'dzmd_ctp', 'b1')

      // 契约 md-config BrokerEntry：完整请求体锚定（字段缺失/错位必须变红）
      expect(marketSourcesApi.addBroker).toHaveBeenCalledWith(1, addBody)
      expect(marketSourcesApi.removeBroker).toHaveBeenCalledWith(1, 'b1')
      expect(marketSourcesApi.updateBroker).toHaveBeenCalledWith(1, 'b1', updateBody)
      expect(marketSourcesApi.setCurrentBroker).toHaveBeenCalledWith(1, 'b1')
      expect(marketSourcePending.pending['source:1:broker_add']).toBe(true)
      expect(marketSourcePending.pending['source:1:broker_remove']).toBe(true)
      expect(marketSourcePending.pending['source:1:broker_update']).toBe(true)
      expect(marketSourcePending.pending['source:1:broker_select']).toBe(true)

      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      await vi.advanceTimersByTimeAsync(0)
      // clearByPrefix 语义: delete 条目 → key 不存在 (undefined)
      expect(marketSourcePending.pending['source:1:broker_add']).toBeUndefined()
      expect(marketSourcePending.pending['source:1:broker_remove']).toBeUndefined()
      expect(marketSourcePending.pending['source:1:broker_update']).toBeUndefined()
      expect(marketSourcePending.pending['source:1:broker_select']).toBeUndefined()
    })

    it('broker 操作 HTTP 失败清 pending 并返回 false', async () => {
      vi.mocked(marketSourcesApi.addBroker).mockRejectedValue(new Error('add broker failed'))
      const store = useMdConfigStore()
      expect(await store.addBroker(1, 'dzmd_ctp', { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' })).toBe(false)
      expect(marketSourcePending.pending['source:1:broker_add']).toBe(false)
    })
  })

  describe('frontend 系列', () => {
    // 双前置 fixture：区分「只改目标」与「整体替换/全部翻转」
    const configTwoFrontends = {
      brokers: [{
        ...configPayload.brokers[0],
        frontends: [
          { address: 'a1', label: 'L1', enabled: false },
          { address: 'a2', label: 'L2', enabled: false },
        ],
      }],
      current_broker_name: 'b1',
    }

    it('addFrontend: 无镜像/无 broker 时不发请求并返回 false', async () => {
      const store = useMdConfigStore()
      const result = await store.addFrontend(1, 'dzmd_ctp', 'nope', 'a9', 'L9')
      expect(result).toBe(false)
      expect(marketSourcesApi.updateFrontends).not.toHaveBeenCalled()
      expect(Object.keys(marketSourcePending.pending)).toHaveLength(0)
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
      expect(marketSourcePending.pending['source:1:frontend_add']).toBe(true)
    })

    it('frontend 四个操作独立 pending（add 挂起时 remove 仍可发起）', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      // add 挂起（永不 resolve）
      vi.mocked(marketSourcesApi.updateFrontends).mockImplementationOnce(() => new Promise(() => {}))
      const p = store.addFrontend(1, 'dzmd_ctp', 'b1', 'a2', 'L2')
      await vi.advanceTimersByTimeAsync(0)
      expect(marketSourcePending.pending['source:1:frontend_add']).toBe(true)

      // remove 独立发起（spec: 各控件独立等待状态, 可同时发起）
      const r = await store.removeFrontend(1, 'dzmd_ctp', 'b1', 'a1')
      expect(r).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledTimes(2)
      // 第二次装置体：仅删除目标 a1（空列表），而非重复/全量旧列表
      expect(marketSourcesApi.updateFrontends).toHaveBeenNthCalledWith(2, 1, 'b1', [])
      expect(marketSourcePending.pending['source:1:frontend_remove']).toBe(true)

      // 收尾：add 走 10s 超时兜底
      await vi.advanceTimersByTimeAsync(10_001)
      expect(await p).toBe(PENDING_TIMEOUT)
      expect(marketSourcePending.pending['source:1:frontend_add']).toBe(false)
    })

    it('setFrontendEnabled: 只改目标前置 enabled，其余前置原样保留', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configTwoFrontends })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      const result = await store.setFrontendEnabled(1, 'dzmd_ctp', 'b1', 'a1', true)
      expect(result).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledWith(1, 'b1', [
        { address: 'a1', label: 'L1', enabled: true },
        { address: 'a2', label: 'L2', enabled: false },
      ])
    })

    it('editFrontend: 值未改变不下发并返回 false', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      const result = await store.editFrontend(1, 'dzmd_ctp', 'b1', 'a1', 'a1')
      expect(result).toBe(false)
      expect(marketSourcesApi.updateFrontends).not.toHaveBeenCalled()
    })

    it('editFrontend: 改地址后整体下发，其余前置原样保留', async () => {
      const store = useMdConfigStore()
      store.applyMdConfig({ source: 'dzmd_ctp', config: configTwoFrontends })
      vi.mocked(marketSourcesApi.updateFrontends).mockResolvedValue({ ok: true })
      const result = await store.editFrontend(1, 'dzmd_ctp', 'b1', 'a1', 'a9')
      expect(result).toBe(true)
      expect(marketSourcesApi.updateFrontends).toHaveBeenCalledWith(1, 'b1', [
        { address: 'a9', label: 'L1', enabled: false },
        { address: 'a2', label: 'L2', enabled: false },
      ])
    })
  })

  describe('setSubscribeParams（契约 md-config SetSubscribeParams op）', () => {
    it('下发单字段 patch，HTTP 成功保持 pending 等 RTN，md_rtn_config 到达清除', async () => {
      vi.mocked(marketSourcesApi.setSubscribeParams).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const r = await store.setSubscribeParams(1, 'dzmd_ctp', { subscribe_batch_size: 500 })
      expect(r).toBe(true)
      expect(marketSourcesApi.setSubscribeParams).toHaveBeenCalledWith(1, { subscribe_batch_size: 500 })
      // keepPendingOnSuccess 默认 true: 成功后 pending 仍挂起, 等 RTN 清
      expect(marketSourcePending.pending['source:1:subscribe_params']).toBe(true)
      store.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      expect(marketSourcePending.pending['source:1:subscribe_params']).toBeUndefined()
    })

    it('空提交幂等成功，不下发', async () => {
      const store = useMdConfigStore()
      const r = await store.setSubscribeParams(1, 'dzmd_ctp', {})
      expect(r).toBe(true)
      expect(marketSourcesApi.setSubscribeParams).not.toHaveBeenCalled()
    })

    it('HTTP 失败返回 false 并清 pending', async () => {
      vi.mocked(marketSourcesApi.setSubscribeParams).mockRejectedValue(new Error('503'))
      const store = useMdConfigStore()
      const r = await store.setSubscribeParams(1, 'dzmd_ctp', { sub_max_retry: 5 })
      expect(r).toBe(false)
      // HTTP 失败清 pending: usePending settle→clearPending 置 false（与 login 失败用例一致）
      expect(marketSourcePending.pending['source:1:subscribe_params']).toBe(false)
    })
  })

  describe('SHM 配置（契约 shm）', () => {
    const shmPayload = {
      page_size_mb: 1024,
      preload_points: { '08:45': { pages: 1, bytes: 0 } },
      check_interval_min: 5,
      check_pages: 1,
      check_bytes: 0,
    }

    it('applyMdShmConfig 合法 payload 写入镜像', () => {
      const store = useMdConfigStore()
      store.applyMdShmConfig('dzmd_ctp', shmPayload)
      expect(store.shmConfigs['dzmd_ctp'].page_size_mb).toBe(1024)
      expect(store.shmConfigs['dzmd_ctp'].check_interval_min).toBe(5)
    })

    it('applyMdShmConfig 后到完整覆盖先到（缺失 preload key 不残留）', () => {
      const store = useMdConfigStore()
      store.applyMdShmConfig('dzmd_ctp', shmPayload)
      store.applyMdShmConfig('dzmd_ctp', {
        page_size_mb: 1024,
        preload_points: {},
        check_interval_min: 0,
        check_pages: 0,
        check_bytes: 0,
      })
      expect(store.shmConfigs['dzmd_ctp']).toEqual({
        page_size_mb: 1024,
        preload_points: {},
        check_interval_min: 0,
        check_pages: 0,
        check_bytes: 0,
      })
    })

    it('applyMdShmConfig 非法 payload 忽略不写且不清已有镜像', () => {
      const store = useMdConfigStore()
      store.applyMdShmConfig('dzmd_ctp', shmPayload)
      store.applyMdShmConfig('dzmd_ctp', { page_size_mb: 'x' })
      store.applyMdShmConfig('dzmd_ctp', null)
      expect(store.shmConfigs['dzmd_ctp']).toEqual(shmPayload)
    })

    it('setShmConfig 下发 patch 并注册 pending，md_shm_config 到达清除', async () => {
      vi.mocked(marketSourcesApi.setShmConfig).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      const r = await store.setShmConfig(1, 'dzmd_ctp', { check_interval_min: 10 })
      expect(r).toBe(true)
      expect(marketSourcesApi.setShmConfig).toHaveBeenCalledWith(1, { check_interval_min: 10 })
      expect(marketSourcePending.pending['source:1:shm_config']).toBe(true)
      store.applyMdShmConfig('dzmd_ctp', shmPayload)
      expect(marketSourcePending.pending['source:1:shm_config']).toBeUndefined()
    })

    it('setShmConfig 下发 preload_points null 删除语义', async () => {
      vi.mocked(marketSourcesApi.setShmConfig).mockResolvedValue({ ok: true })
      const store = useMdConfigStore()
      await store.setShmConfig(1, 'dzmd_ctp', { preload_points: { '08:45': null } })
      expect(marketSourcesApi.setShmConfig).toHaveBeenCalledWith(1,
        { preload_points: { '08:45': null } })
    })

    it('setShmConfig 空 patch 幂等成功：不下发、不挂 pending', async () => {
      const store = useMdConfigStore()
      const r = await store.setShmConfig(1, 'dzmd_ctp', {})
      expect(r).toBe(true)
      expect(marketSourcesApi.setShmConfig).not.toHaveBeenCalled()
      expect(marketSourcePending.pending['source:1:shm_config']).toBeUndefined()
    })

    it('setShmConfig HTTP 失败返回 false 并清 pending', async () => {
      vi.mocked(marketSourcesApi.setShmConfig).mockRejectedValue(new Error('503'))
      const store = useMdConfigStore()
      const r = await store.setShmConfig(1, 'dzmd_ctp', { check_pages: 2 })
      expect(r).toBe(false)
      expect(marketSourcePending.pending['source:1:shm_config']).toBe(false)
    })
  })
})
