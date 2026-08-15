import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { nextTick } from 'vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProcessStore } from '@/stores/process'
import { useMdConfigStore } from '@/stores/mdConfig'
import { useProgressStore } from '@/stores/progress'
import { __resetForTests } from '@/composables/usePending'
import { useToastStore } from '@/stores/toast'
import { marketSourcesApi } from '@/api/marketSources'

// =====================================================================
// P4 Task 5: marketSources 聚合语义测试
// store 已瘦身为「DB 列表 + 聚合 view」——sources 由 baseSources(DB) ×
// process/mdConfig 镜像 × usePending(模块级 pending) × 本地 UI 状态组合。
// 测试策略: 写入领域 store / usePending 数据, 断言聚合结果 (组合正确性),
// 以及薄代理操作与现状一致的 pending 语义 (成功不清等 RTN / 失败清 / 超时清)。
// =====================================================================

vi.mock('@/api/marketSources', () => ({
  marketSourcesApi: {
    list: vi.fn(),
    listAvailable: vi.fn(),
    create: vi.fn(),
    start: vi.fn(),
    remove: vi.fn(),
    setAutoLogin: vi.fn(),
    login: vi.fn(),
    logout: vi.fn(),
    addBroker: vi.fn(),
    removeBroker: vi.fn(),
    updateBroker: vi.fn(),
    setCurrentBroker: vi.fn(),
    updateFrontends: vi.fn(),
  },
}))

const dbSource = {
  id: 1,
  source_type: 'CTP',
  source_name: 'dzmd_ctp',
  display_name: 'CTP',
  ui_card: 'CtpCard',
  is_added: true,
  auto_login: false,
  created_at: '2026-01-01T00:00:00Z',
  updated_at: '2026-01-01T00:00:00Z',
}

const dbSourceB = {
  id: 2,
  source_type: 'XTP',
  source_name: 'dzmd_xtp',
  display_name: 'XTP',
  ui_card: '',
  is_added: true,
  auto_login: false,
  created_at: '2026-01-01T00:00:00Z',
  updated_at: '2026-01-01T00:00:00Z',
}

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

describe('useMarketSourcesStore (P4 Task 5 聚合)', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetForTests()
    vi.clearAllMocks()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  async function loadDb(): Promise<void> {
    vi.mocked(marketSourcesApi.list).mockResolvedValue([dbSource])
    const store = useMarketSourcesStore()
    await store.loadSources()
  }

  describe('聚合 computed `sources`', () => {
    it('DB 列表 + 默认 view 字段（镜像未建立时）', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      expect(store.sources).toHaveLength(1)
      const s = store.sources[0]
      // DB 权威字段透传
      expect(s.id).toBe(1)
      expect(s.source_type).toBe('CTP')
      expect(s.source_name).toBe('dzmd_ctp')
      expect(s.display_name).toBe('CTP')
      expect(s.ui_card).toBe('CtpCard')
      expect(s.auto_login).toBe(false)
      // 默认 view 字段
      expect(s.process_state).toBeNull()
      expect(s.loginState).toBe('offline')
      expect(s.brokers).toEqual([])
      expect(s.schedules).toEqual([])
      expect(s.selectedBrokerId).toBeNull()
      expect(s.expanded).toBe(false)
      expect(s.tradingDay).toBe('--')
      expect(s.subscribeCount).toBe(0)
      expect(s.subscribeTotal).toBe(0)
      // 17 个 pending 字段默认 false
      expect(s.loginPending).toBe(false)
      expect(s.logoutPending).toBe(false)
      expect(s.autoLoginPending).toBe(false)
      expect(s.scheduleAddPending).toBe(false)
      expect(s.scheduleRemovePending).toBe(false)
      expect(s.brokerAddPending).toBe(false)
      expect(s.brokerRemovePending).toBe(false)
      expect(s.brokerSelectPending).toBe(false)
      expect(s.brokerFieldEditPending).toBe(false)
      expect(s.frontendAddPending).toBe(false)
      expect(s.frontendRemovePending).toBe(false)
      expect(s.frontendEditPending).toBe(false)
      expect(s.frontendTogglePending).toBe(false)
      expect(s.startPending).toBe(false)
      expect(s.stopPending).toBe(false)
      expect(s.removePending).toBe(false)
    })

    describe('md_status 聚合桥接（契约 09）', () => {
      it('镜像到达后填充 6 字段', async () => {
        await loadDb()  // mock list + loadSources（sources 非空的唯一途径）
        useMdConfigStore().applyMdStatus({ source: 'dzmd_ctp', status: {
          api_version: 'v6.7.2',
          sys_version: 'v6.7.2_20240105',
          trading_day: '20260808',
          login_time: '2026-08-08 08:45:32',
          expected_subscribe_count: 5,
          subscribed_count: 3,
        }})
        const s = useMarketSourcesStore().sources.find(x => x.source_name === 'dzmd_ctp')!
        expect(s.tradingDay).toBe('20260808')
        expect(s.subscribeCount).toBe(3)
        expect(s.subscribeTotal).toBe(5)
        expect(s.apiVersion).toBe('v6.7.2')
        expect(s.sysVersion).toBe('v6.7.2_20240105')
        expect(s.loginTime).toBe('2026-08-08 08:45:32')
      })

      it('清空语义：登录失败仅清 login_time，其余保留（契约 09）', async () => {
        await loadDb()
        useMdConfigStore().applyMdStatus({ source: 'dzmd_ctp', status: {
          api_version: 'v6.7.2',
          sys_version: 'v6.7.2_20240105',
          trading_day: '20260808',
          login_time: '',
          expected_subscribe_count: 5,
          subscribed_count: 0,
        }})
        const s = useMarketSourcesStore().sources.find(x => x.source_name === 'dzmd_ctp')!
        expect(s.tradingDay).toBe('20260808')      // 保留，不因 login_time 为空整体丢弃
        expect(s.loginTime).toBe('')
        expect(s.subscribeCount).toBe(0)
      })

      it('字段类型非法时逐字段回落默认值', async () => {
        await loadDb()
        useMdConfigStore().applyMdStatus({ source: 'dzmd_ctp', status: { trading_day: 123, subscribed_count: 'x' } })
        const s = useMarketSourcesStore().sources.find(x => x.source_name === 'dzmd_ctp')!
        expect(s.tradingDay).toBe('--')
        expect(s.subscribeCount).toBe(0)
        expect(s.subscribeTotal).toBe(0)
      })
    })

    it('领域 store 写入后聚合回填（process_state / loginState / brokers / schedules / auto_login / 选中）', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      const process = useProcessStore()
      const md = useMdConfigStore()

      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 123 })
      expect(store.sources[0].process_state).toBe('Running')

      md.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      const s = store.sources[0]
      expect(s.brokers).toHaveLength(1)
      expect(s.brokers[0].name).toBe('b1')
      expect(s.selectedBrokerId).toBe('b1')
      // 排程/自动登录来自 auto_login 镜像（契约 04）
      md.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      const s2 = store.sources[0]
      expect(s2.auto_login).toBe(true)
      expect(s2.schedules).toHaveLength(1)
      expect(s2.schedules[0].login_time).toBe('09:00')
      expect(s2.schedules[0].source_id).toBe(1)

      // loginState 由 RTN_PROGRESS 驱动（契约 05）
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      await nextTick()  // loginState 缓存由 watch 同步
      expect(store.sources[0].loginState).toBe('online')
      // 非法 progress（不确定进度 max<=min）不清默认值 (保留上一次合法值)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 0, current: 0 })
      await nextTick()
      expect(store.sources[0].loginState).toBe('online')
    })

    it('display_name: 进程镜像优先于 DB; loadSources 清本地覆盖', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      const process = useProcessStore()
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1, display_name: 'CTP主行情' })
      expect(store.sources[0].display_name).toBe('CTP主行情')
      // 覆盖路径: addAndStartSource(existing) 设 overrides → 立即反馈
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      await store.addAndStartSource('dzmd_ctp', '启动名')
      expect(store.sources[0].display_name).toBe('启动名')
      // loadSources 清 overrides (DB 权威刷新), 回落到进程镜像名
      await loadDb()
      expect(store.sources[0].display_name).toBe('CTP主行情')
      // 清理 start pending (防 fake timer 挂起)
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1, event: 'StartSucceeded' })
      await nextTick()
    })

    it('expanded 为本地 UI 状态, toggleExpand 切换', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      expect(store.sources[0].expanded).toBe(false)
      store.toggleExpand(1)
      expect(store.sources[0].expanded).toBe(true)
      store.toggleExpand(1)
      expect(store.sources[0].expanded).toBe(false)
    })
  })

  describe('pending 语义（聚合自 usePending, 对照现状）', () => {
    it('login: HTTP 成功不清 pending, RTN_PROGRESS "已登录" 清', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      const store = useMarketSourcesStore()
      const p = store.login(1)
      expect(store.sources[0].loginPending).toBe(true)
      await p
      // 成功不清: 等 WS RTN
      expect(store.sources[0].loginPending).toBe(true)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      await nextTick()
      expect(store.sources[0].loginPending).toBe(false)
      expect(store.error).toBeNull()
    })

    it('login: HTTP 失败清 pending + 设置 error', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.login).mockRejectedValue(new Error('boom'))
      const store = useMarketSourcesStore()
      await store.login(1)
      expect(store.sources[0].loginPending).toBe(false)
      expect(store.error).toBe('login failed')
    })

    it('login: 超时兜底清 pending（usePending 30s/10s 语义）', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.login).mockImplementation(() => new Promise(() => {}))
      const store = useMarketSourcesStore()
      const p = store.login(1)
      expect(store.sources[0].loginPending).toBe(true)
      await vi.advanceTimersByTimeAsync(10_000)
      expect(store.sources[0].loginPending).toBe(false)
      await p  // 超时后 run resolve
    })

    it('logout: RTN_PROGRESS "未登录" 清 logout pending', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.logout).mockResolvedValue({ ok: true })
      const store = useMarketSourcesStore()
      const p = store.logout(1)
      expect(store.sources[0].logoutPending).toBe(true)
      await p
      expect(store.sources[0].logoutPending).toBe(true)
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 0 })
      await nextTick()
      expect(store.sources[0].logoutPending).toBe(false)
    })

    it('md_rtn_config 到达批量清全部配置类 pending（对照现状 setMdConfig 清 11 字段）', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      const md = useMdConfigStore()
      // 先通过一次操作建立 name→id 映射 (mdConfig.applyMdConfig 依此定位 pending key)
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      await md.login(1, 'dzmd_ctp')  // login pending 挂起 (成功不清)
      // 手动挂起 broker/frontend 配置类 pending + 排程类 pending + 进程类 pending
      const { usePending } = await import('@/composables/usePending')
      const pending = usePending()
      for (const op of ['auto_login', 'schedule_add', 'schedule_remove', 'broker_add', 'broker_remove', 'broker_update', 'broker_select', 'frontend_add', 'frontend_remove', 'frontend_edit', 'frontend_toggle']) {
        await pending.run(`source:1:${op}`, () => Promise.resolve('x'))
      }
      await pending.run('source:1:start', () => Promise.resolve('x'))
      md.applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      await nextTick()
      const s = store.sources[0]
      // md_rtn_config 只清 broker/frontend 类（契约 08）
      expect(s.brokerAddPending).toBe(false)
      expect(s.brokerRemovePending).toBe(false)
      expect(s.brokerSelectPending).toBe(false)
      expect(s.brokerFieldEditPending).toBe(false)
      expect(s.frontendAddPending).toBe(false)
      expect(s.frontendRemovePending).toBe(false)
      expect(s.frontendEditPending).toBe(false)
      expect(s.frontendTogglePending).toBe(false)
      // 排程类 pending 由 auto_login RTN 清（契约 04），此处保持挂起
      expect(s.autoLoginPending).toBe(true)
      expect(s.scheduleAddPending).toBe(true)
      expect(s.scheduleRemovePending).toBe(true)
      // 进程类 / login pending 不受配置类批量清影响
      expect(s.startPending).toBe(true)
      expect(s.loginPending).toBe(true)
      // auto_login RTN 到达 → 排程类 pending 全清
      md.applyAutoLogin('dzmd_ctp', autoLoginPayload)
      await nextTick()
      const s2 = store.sources[0]
      expect(s2.autoLoginPending).toBe(false)
      expect(s2.scheduleAddPending).toBe(false)
      expect(s2.scheduleRemovePending).toBe(false)
    })

    it('schedules 以 auto_login 镜像为准（契约 04, 含清空语义）', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      const md = useMdConfigStore()
      // 镜像未建立: 空列表
      expect(store.sources[0].schedules).toEqual([])
      // 镜像建立: 以镜像为准
      md.applyAutoLogin('dzmd_ctp', {
        enabled: true,
        schedules: [{ login_time: '09:00', logout_time: '15:00' }],
      })
      expect(store.sources[0].schedules).toEqual([
        { id: 0, source_id: 1, login_time: '09:00', logout_time: '15:00' },
      ])
      expect(store.sources[0].auto_login).toBe(true)
    })

    it('镜像已建立但 schedules 清空时显示空（清空语义不被回退掩盖）', async () => {
      await loadDb()
      const store = useMarketSourcesStore()
      const md = useMdConfigStore()
      md.applyAutoLogin('dzmd_ctp', {
        enabled: true,
        schedules: [{ login_time: '09:00', logout_time: '15:00' }],
      })
      expect(store.sources[0].schedules).toHaveLength(1)
      md.applyAutoLogin('dzmd_ctp', { enabled: false, schedules: [] })
      expect(store.sources[0].schedules).toEqual([])
      expect(store.sources[0].auto_login).toBe(false)
    })
  })

  describe('B1: removeSource 流程（Remove 语义含 Stop）', () => {
    it('remove 挂起时 stopPending 也显示; Remove RTN 成功清两者并移除卡片', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 1 })
      const store = useMarketSourcesStore()
      const process = useProcessStore()
      const p = store.removeSource(1)
      expect(store.sources[0].removePending).toBe(true)
      expect(store.sources[0].stopPending).toBe(true)  // B1
      await p
      // HTTP 成功不清: 等 RTN
      expect(store.sources[0].removePending).toBe(true)
      expect(store.sources[0].stopPending).toBe(true)
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopped', pid: 0, event: 'RemoveSucceeded' })
      await nextTick()
      expect(store.sources).toHaveLength(0)  // 卡片移除
    })

    it('Remove RTN 失败: 清 pending 保留卡片, 不弹 toast（反馈由 NOTIFY_UI 弹窗承载）', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 1 })
      const store = useMarketSourcesStore()
      const process = useProcessStore()
      await store.removeSource(1)
      const toast = useToastStore()
      const errorSpy = vi.spyOn(toast, 'error')
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopped', pid: 0, message: 'remove failed', event: 'RemoveFailed' })
      await nextTick()
      expect(store.sources).toHaveLength(1)
      expect(store.sources[0].removePending).toBe(false)
      expect(store.sources[0].stopPending).toBe(false)
      // 失败反馈单一出口: NOTIFY_UI 弹窗（契约 03）, 不重复 toast
      expect(errorSpy).not.toHaveBeenCalled()
      expect(store.error).toBeNull()
    })

    it('remove HTTP 失败清 pending + error', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.remove).mockRejectedValue(new Error('boom'))
      const store = useMarketSourcesStore()
      await store.removeSource(1)
      expect(store.sources[0].removePending).toBe(false)
      expect(store.sources[0].stopPending).toBe(false)
      expect(store.error).toBe('remove market source failed')
      expect(store.sources).toHaveLength(1)  // 卡片保留
    })

    it('Remove ack 消费后不再误删 loadSources 恢复的卡片（回归: 永久 ack 误删）', async () => {
      // 初始列表: A(dzmd_ctp) + B(dzmd_xtp)
      vi.mocked(marketSourcesApi.list).mockResolvedValue([dbSource, dbSourceB])
      const store = useMarketSourcesStore()
      const process = useProcessStore()
      await store.loadSources()
      expect(store.sources.map(s => s.source_name)).toEqual(['dzmd_ctp', 'dzmd_xtp'])

      // 删除 A → Remove RTN 成功 → 卡片移除 (ack 写入)
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 1 })
      await store.removeSource(1)
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Stopped', pid: 0, event: 'RemoveSucceeded' })
      await nextTick()
      expect(store.sources.map(s => s.source_name)).toEqual(['dzmd_xtp'])

      // 恢复 A: remove 保留 DB 记录, loadSources 重新拉回
      await store.loadSources()
      expect(store.sources.map(s => s.source_name)).toEqual(['dzmd_ctp', 'dzmd_xtp'])

      // 删除 B → Remove RTN 成功 → watch 触发 filter
      vi.mocked(marketSourcesApi.remove).mockResolvedValue({ ok: true, id: 2 })
      await store.removeSource(2)
      process.applyProcessStatus({ name: 'dzmd_xtp', state: 'Stopped', pid: 0, event: 'RemoveSucceeded' })
      await nextTick()
      // 只有 B 被移除, A 卡片保留 (B 的 ack 已消费, 不会误删 A)
      expect(store.sources.map(s => s.source_name)).toEqual(['dzmd_ctp'])
    })
  })

  describe('C7: pendingCreations 晋升（WS 驱动卡片添加）', () => {
    it('create + start 后不立即出现, process_status 到达时晋升为卡片(expanded)', async () => {
      vi.mocked(marketSourcesApi.list).mockResolvedValue([])
      vi.mocked(marketSourcesApi.create).mockResolvedValue({ ...dbSource })
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useMarketSourcesStore()
      await store.loadSources()
      const p = store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      expect(store.sources).toHaveLength(0)
      await p
      // HTTP 成功不清: 等 WS 推送晋升
      expect(store.sources).toHaveLength(0)
      expect(store.error).toBeNull()
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Starting', pid: 0 })
      await nextTick()
      expect(store.sources).toHaveLength(1)
      expect(store.sources[0].source_name).toBe('dzmd_ctp')
      expect(store.sources[0].expanded).toBe(true)
      expect(store.sources[0].process_state).toBe('Starting')
    })

    it('RTN_PROGRESS 到达同样可晋升', async () => {
      vi.mocked(marketSourcesApi.list).mockResolvedValue([])
      vi.mocked(marketSourcesApi.create).mockResolvedValue({ ...dbSource })
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useMarketSourcesStore()
      await store.loadSources()
      await store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2 })
      await nextTick()
      expect(store.sources).toHaveLength(1)
      expect(store.sources[0].loginState).toBe('pending')
    })

    it('loadSources 已含 DB 记录时 WS 到达不重复 push（防重复晋升）', async () => {
      vi.mocked(marketSourcesApi.list).mockResolvedValue([])
      vi.mocked(marketSourcesApi.create).mockResolvedValue({ ...dbSource })
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useMarketSourcesStore()
      await store.loadSources()
      await store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      // 重连窗口: loadSources 从 DB 拉回该记录
      vi.mocked(marketSourcesApi.list).mockResolvedValue([dbSource])
      await store.loadSources()
      expect(store.sources).toHaveLength(1)
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Starting', pid: 0 })
      await nextTick()
      expect(store.sources).toHaveLength(1)  // 不重复
    })

    it('addAndStartSource 失败清 pendingCreations + error', async () => {
      vi.mocked(marketSourcesApi.list).mockResolvedValue([])
      vi.mocked(marketSourcesApi.create).mockResolvedValue({ ...dbSource })
      vi.mocked(marketSourcesApi.start).mockRejectedValue(new Error('boom'))
      const store = useMarketSourcesStore()
      await store.loadSources()
      await store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      expect(store.error).toContain('add and start market source failed')
      // 无残留 pendingCreations: 后续 WS 到达不晋升
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Starting', pid: 0 })
      await nextTick()
      expect(store.sources).toHaveLength(0)
    })

    it('P2: 首次启动失败 (StartFailed/Crashed) 不晋升卡片, 且重试不受残留阻断', async () => {
      vi.mocked(marketSourcesApi.list).mockResolvedValue([])
      vi.mocked(marketSourcesApi.create).mockResolvedValue({ ...dbSource })
      vi.mocked(marketSourcesApi.start).mockResolvedValue({ ok: true, source: 'dzmd_ctp' })
      const store = useMarketSourcesStore()
      await store.loadSources()
      await store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      // master 失败路径: StartFailed 事件 + Crashed 状态 (契约 03)
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Crashed', pid: 0, event: 'StartFailed' })
      await nextTick()
      // 不晋升: 失败反馈由 NOTIFY_UI 弹窗承载, 卡片不残留
      expect(store.sources).toHaveLength(0)
      // 失败后重试: 残留 pendingCreations 已被清理, 二次 addAndStartSource 不被静默吞掉
      await store.addAndStartSource('dzmd_ctp', 'CTP主行情')
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1, event: 'StartSucceeded' })
      await nextTick()
      expect(store.sources).toHaveLength(1)
      expect(store.sources[0].process_state).toBe('Running')
    })
  })

  describe('薄代理操作（状态保护 / F-C10 / 防重入）', () => {
    it('状态保护: 非 Idle 时 removeBroker 拒收不发请求', async () => {
      await loadDb()
      const process = useProcessStore()
      // Running + online = 非 Idle
      process.applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1 })
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      const store = useMarketSourcesStore()
      await store.removeBroker(1, 'b1')
      expect(marketSourcesApi.removeBroker).not.toHaveBeenCalled()
      expect(store.error).toBeNull()
    })

    it('Idle 时 removeBroker 下发, 失败 re-throw (F-C10) + error', async () => {
      await loadDb()
      useProcessStore().applyProcessStatus({ name: 'dzmd_ctp', state: 'Running', pid: 1 })
      vi.mocked(marketSourcesApi.removeBroker).mockRejectedValue(new Error('boom'))
      const store = useMarketSourcesStore()
      await expect(store.removeBroker(1, 'b1')).rejects.toThrow('remove broker failed')
      expect(store.error).toBe('remove broker failed')
      expect(store.sources[0].brokerRemovePending).toBe(false)
    })

    it('addBroker 失败 re-throw (F-C10) + error', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.addBroker).mockRejectedValue(new Error('boom'))
      const store = useMarketSourcesStore()
      await expect(store.addBroker(1, { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' })).rejects.toThrow('add broker failed')
      expect(store.error).toBe('add broker failed')
      expect(store.sources[0].brokerAddPending).toBe(false)
    })

    it('addBroker 成功不清 pending, md_rtn_config 到达清 + 镜像回填', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.addBroker).mockResolvedValue({ ok: true })
      const store = useMarketSourcesStore()
      const p = store.addBroker(1, { name: 'b2', broker_id: 'x', user_id: 'u', password: 'p', product_info: 'pi' })
      expect(store.sources[0].brokerAddPending).toBe(true)
      await p
      expect(store.sources[0].brokerAddPending).toBe(true)
      useMdConfigStore().applyMdConfig({ source: 'dzmd_ctp', config: configPayload })
      await nextTick()
      expect(store.sources[0].brokerAddPending).toBe(false)
      expect(store.sources[0].brokers).toHaveLength(1)
    })

    it('防重入: pending 挂起时重复调用不发请求', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.login).mockImplementation(() => new Promise(() => {}))
      const store = useMarketSourcesStore()
      void store.login(1)
      expect(store.sources[0].loginPending).toBe(true)
      await store.login(1)
      expect(marketSourcesApi.login).toHaveBeenCalledTimes(1)
      await vi.advanceTimersByTimeAsync(10_000)
    })

    it('removeSchedule: 以 login+logout 时间对下发全量（契约 04 镜像无 id）', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.setAutoLogin).mockResolvedValue({ ok: true })
      useMdConfigStore().applyAutoLogin('dzmd_ctp', {
        enabled: true,
        schedules: [{ login_time: '09:00', logout_time: '15:00' }],
      })
      const store = useMarketSourcesStore()
      await store.removeSchedule(1, '09:00', '15:00')
      expect(marketSourcesApi.setAutoLogin).toHaveBeenCalledWith(1, {
        enabled: true,
        schedules: [],
      })
      // 成功不清 pending (等 auto_login RTN 清)
      expect(store.sources[0].scheduleRemovePending).toBe(true)
      // 手动 resolve (模拟 RTN_AUTO_LOGIN 批量清)
      const { usePending } = await import('@/composables/usePending')
      usePending().resolve('source:1:schedule_remove')
      await nextTick()
      expect(store.sources[0].scheduleRemovePending).toBe(false)
    })

    it('batchLogin 只登录非 online 且未 pending 的 source', async () => {
      await loadDb()
      vi.mocked(marketSourcesApi.login).mockResolvedValue({ ok: true })
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 4 })
      await nextTick()  // loginState 缓存由 watch 同步
      const store = useMarketSourcesStore()
      await store.batchLogin()
      expect(marketSourcesApi.login).not.toHaveBeenCalled()
      useProgressStore().applyProgress('dzmd_ctp', { min: 0, max: 4, current: 0 })
      await nextTick()
      await store.batchLogin()
      expect(marketSourcesApi.login).toHaveBeenCalledTimes(1)
      // 等超时兜底清 (防 fake timer 挂起)
      await vi.advanceTimersByTimeAsync(10_000)
    })
  })

  describe('error / clearError', () => {
    it('clearError 清空 error', async () => {
      const store = useMarketSourcesStore()
      expect(store.error).toBeNull()
      store.error = 'x'
      store.clearError()
      expect(store.error).toBeNull()
    })
  })
})
