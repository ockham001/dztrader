import { defineStore } from 'pinia'
import { ref, computed, watch } from 'vue'
import { marketSourcesApi } from '@/api/marketSources'
import type { AvailableMarketSource, AddBrokerBody, SubscribeParamsBody } from '@/api/marketSources'
import type { BrokerEntry, MarketSource } from '@/types/api'
import { usePending, PENDING_TIMEOUT } from '@/composables/usePending'
import { useProcessStore } from '@/stores/process'
import { useMdConfigStore } from '@/stores/mdConfig'
import { useProgressStore } from '@/stores/progress'
import { makeSource, progressLoginState, parseMdStatus, type MarketSourceView, type LoginState, type ScheduleView } from '@/composables/marketSourceView'

export type { MarketSourceView, LoginState, ScheduleView }
// P4 Task5: DB列表+聚合view; 领域状态由 process/mdConfig 镜像+usePending(单写化);
// 操作=薄代理(失败设 error + rethrow); 类型已抽离到 marketSourceView.ts。
export const useMarketSourcesStore = defineStore('marketSources', () => {
  // ===== DB 列表职责 =====
  const loading = ref(false)
  const error = ref<string | null>(null)
  const availableSources = ref<AvailableMarketSource[]>([])
  const availableLoading = ref(false)
  // C7: WS 驱动卡片添加 - create 后暂存, 等 WS 推送晋升正式卡片; 失败移除。
  const pendingCreations = ref<Record<string, {
    id: number
    source_type: string
    source_name: string
    display_name: string
    ui_card: string
  }>>({})

  const baseSources = ref<MarketSource[]>([])  // DB 权威字段, loadSources 整体替换
  // ===== 本地 UI 状态(不属于领域) =====
  const expandedIds = ref<Record<number, boolean>>({})
  const displayOverrides = ref<Record<string, string>>({})
  const loginStateCache = ref<Record<string, LoginState>>({})
  // ===== 领域 store(镜像/pending 单一事实来源) =====
  const process = useProcessStore()
  const md = useMdConfigStore()
  const progress = useProgressStore()
  const { pending } = usePending()

  const keyOf = (id: number, op: string) => `source:${id}:${op}`

  /// 聚合 schedules: 以 auto_login 镜像为准 (契约 04, 含清空语义)。
  /// 镜像条目无 id, 前端以 login+logout 时间对匹配 (ScheduleView.id 为占位)。
  function mergeSchedules(id: number, mirror?: { login_time: string; logout_time: string }[]): ScheduleView[] {
    return (mirror ?? []).map((raw) => {
      const loginTime = typeof raw.login_time === 'string' ? raw.login_time : ''
      const logoutTime = typeof raw.logout_time === 'string' ? raw.logout_time : ''
      return { id: 0, source_id: id, login_time: loginTime, logout_time: logoutTime }
    })
  }

  /// 聚合 computed: 形状=MarketSourceView, 组件引用不变。字段来源见字段注解。
  const sources = computed<MarketSourceView[]>(() =>
    baseSources.value.map((b) => {
      const name = b.source_name
      const processStatus = process.statuses[name]
      const mdConfig = md.configs[name]
      const mdStatus = parseMdStatus(md.statuses[name]?.status)
      const autoLogin = md.autoLogins[name]
      const prog = progress.progress[name]
      const loginState = progressLoginState(prog) ?? loginStateCache.value[name] ?? 'offline'
      // P3: StatusIndicator 直连数据——有镜像用真实值, 无镜像回落三值映射
      const progressView = prog
        ? { current: prog.current, min: prog.min, max: prog.max, desc: prog.desc }
        : {
            current: loginState === 'online' ? 1 : loginState === 'pending' ? 0.5 : 0,
            min: 0,
            max: 1,
          }
      const pend = (op: string): boolean => pending[keyOf(b.id, op)] ?? false
      return makeSource({
        id: b.id,
        source_type: b.source_type,
        source_name: b.source_name,
        display_name: displayOverrides.value[name] ?? processStatus?.display_name ?? b.display_name,
        ui_card: b.ui_card,
        is_added: b.is_added,
        // 契约 04: 自动登录/排程单一真相源为 auto_login 镜像 (WS auto_login 帧)
        auto_login: autoLogin?.enabled ?? false,
        created_at: b.created_at,
        updated_at: b.updated_at,
        loginState,
        progressView,
        loginPending: pend('login'),
        logoutPending: pend('logout'),
        autoLoginPending: pend('auto_login'),
        scheduleAddPending: pend('schedule_add'),
        scheduleRemovePending: pend('schedule_remove'),
        expanded: expandedIds.value[b.id] ?? false,
        apiVersion: mdStatus?.apiVersion ?? '',
        sysVersion: mdStatus?.sysVersion ?? '',
        loginTime: mdStatus?.loginTime ?? '',
        // tradingDay 用 || 非 ??: parseMdStatus 对类型非法回落空串（非 nullish），
        // ?? 不会触发回落；|| 使空串也显示 '--'（Idle 清空语义下 UI 正确显示占位符）
        tradingDay: mdStatus?.tradingDay || '--',
        subscribeCount: mdStatus?.subscribeCount ?? 0,
        subscribeTotal: mdStatus?.subscribeTotal ?? 0,
        brokers: mdConfig?.brokers ?? [],
        subscribeBatchSize: mdConfig?.subscribe_batch_size ?? null,
        subscribeBatchDelayMs: mdConfig?.subscribe_batch_delay_ms ?? null,
        subCheckIntervalMs: mdConfig?.sub_check_interval_ms ?? null,
        subMaxRetry: mdConfig?.sub_max_retry ?? null,
        subscribeParamsPending: pend('subscribe_params'),
        selectedBrokerId: mdConfig?.current_broker_name || null,
        schedules: mergeSchedules(b.id, autoLogin?.schedules),
        brokerAddPending: pend('broker_add'),
        brokerRemovePending: pend('broker_remove'),
        brokerSelectPending: pend('broker_select'),
        brokerFieldEditPending: pend('broker_update'),
        frontendAddPending: pend('frontend_add'),
        frontendRemovePending: pend('frontend_remove'),
        frontendEditPending: pend('frontend_edit'),
        frontendTogglePending: pend('frontend_toggle'),
        process_state: processStatus?.state ?? null,
        startPending: pend('start'),
        // B1: Remove 语义含 Stop — remove 流程期间 stopPending 同样显示
        stopPending: pend('stop') || pend('remove'),
        removePending: pend('remove'),
      })
    })
  )

  const sourceCount = computed(() => sources.value.length)

  function findView(id: number): MarketSourceView | undefined {
    return sources.value.find(s => s.id === id)
  }

  /// 状态保护(契约 §状态保护): Running 且未登录(offline) 为 Idle, 可改连接参数。
  function isStateIdle(s: MarketSourceView): boolean {
    return s.process_state === 'Running' && s.loginState === 'offline'
  }

  /// C7: WS 推送到达时, pendingCreations 中条目晋升为正式卡片(防重复: 已拉回则只清暂存)。
  /// 监听源: process_status / md_status / progress（任一 RTN 到达即晋升; md 启动后三路都会推送）。
  /// P2 修复: 首次启动失败 (StartFailed / Crashed) 不晋升, 与"失败移除"语义一致——
  /// 失败反馈由 NOTIFY_UI 弹窗承载, 卡片不残留 (否则常驻一张 Crashed 卡片直到刷新)。
  function promotePendingFromStatuses(): void {
    const names = new Set([...Object.keys(process.statuses), ...Object.keys(md.statuses), ...Object.keys(progress.progress)])
    for (const name of names) {
      const info = pendingCreations.value[name]
      if (!info) continue
      const status = process.statuses[name]
      if (status && (status.event === 'StartFailed' || status.state === 'Crashed')) {
        delete pendingCreations.value[name]
        continue
      }
      delete pendingCreations.value[name]
      if (baseSources.value.some(s => s.source_name === name)) continue
      baseSources.value = [...baseSources.value, {
        id: info.id,
        source_type: info.source_type,
        source_name: info.source_name,
        display_name: info.display_name,
        ui_card: info.ui_card,
        is_added: true,
        auto_login: false,
        created_at: new Date().toISOString(),
        updated_at: new Date().toISOString(),
      }]
      expandedIds.value[info.id] = true
    }
  }
  watch(() => [process.statuses, md.statuses, progress.progress] as const, promotePendingFromStatuses, { deep: true })

  /// loginState 缓存同步: 提取合法 progress 数值映射写入缓存(非法帧不更新, 保留上一合法值)。
  function syncLoginStateCache(): void {
    for (const [name, p] of Object.entries(progress.progress)) {
      if (!name.startsWith('dzmd_')) continue  // td 实例 (dztd_ctp:{账户}) 不参与行情源聚合
      const parsed = progressLoginState(p)
      if (parsed) loginStateCache.value[name] = parsed
    }
  }
  watch(() => progress.progress, syncLoginStateCache, { deep: true })

  /// Remove RTN 成功 ack 后移除卡片; 消费后立即删除 ack(一次性事件, 避免误删已恢复卡片)。
  function removeAckedSources(): void {
    for (const name of Object.keys(process.removedNames)) {
      baseSources.value = baseSources.value.filter(s => s.source_name !== name)
      delete process.removedNames[name]
    }
  }
  watch(() => process.removedNames, removeAckedSources, { deep: true })

  async function loadSources(): Promise<void> {
    loading.value = true
    error.value = null
    try {
      const list = await marketSourcesApi.list()
      baseSources.value = list.map(item => ({ ...item }))
      displayOverrides.value = {}  // DB 权威刷新清显示名覆盖; expandedIds 保留
    } catch (err) {
      error.value = err instanceof Error ? err.message : 'load market source list failed'
    } finally {
      loading.value = false
    }
  }

  function toggleExpand(id: number): void {
    const src = findView(id)
    if (!src) return
    const newExpanded = !src.expanded
    expandedIds.value[id] = newExpanded
  }

  /// 操作薄代理统一 helper: 折叠 15 个薄代理。守卫: findView+防重入+可选
  /// requireIdle; 失败设 error; PENDING_TIMEOUT 已 toast 不设; rethrow 抛 Error。
  async function runOp(
    id: number,
    op: string,
    fn: (src: MarketSourceView) => Promise<boolean | typeof PENDING_TIMEOUT>,
    failMsg: string,
    opts: { requireIdle?: boolean; rethrow?: boolean } = {},
  ): Promise<void> {
    const src = findView(id)
    if (!src || pending[keyOf(id, op)]) return
    if (opts.requireIdle && !isStateIdle(src)) return
    const ok = await fn(src)
    if (ok === PENDING_TIMEOUT) return  // 超时已 toast, 不设 error (双 toast 去重)
    if (!ok) {
      error.value = failMsg
      if (opts.rethrow) throw new Error(failMsg)
    }
  }

  async function login(id: number): Promise<void> {
    await runOp(id, 'login', s => md.login(id, s.source_name), 'login failed')
  }
  async function logout(id: number): Promise<void> {
    await runOp(id, 'logout', s => md.logout(id, s.source_name), 'logout failed')
  }
  async function toggleAutoLogin(id: number, enabled: boolean): Promise<void> {
    await runOp(id, 'auto_login', s => md.toggleAutoLogin(id, s.source_name, enabled), 'toggle auto-login failed')
  }
  async function addSchedule(id: number, loginTime: string, logoutTime: string): Promise<void> {
    await runOp(id, 'schedule_add', s => md.addSchedule(id, s.source_name, loginTime, logoutTime),
      'add schedule failed', { rethrow: true })
  }
  async function removeSchedule(id: number, loginTime: string, logoutTime: string): Promise<void> {
    // 契约 04 镜像条目无 id, 以 login+logout 时间对匹配（ScheduleManager 直接传时间对）
    await runOp(id, 'schedule_remove', (s) =>
      md.removeSchedule(id, s.source_name, loginTime, logoutTime),
      'remove schedule failed')
  }
  async function addBroker(id: number, data: AddBrokerBody): Promise<void> {
    await runOp(id, 'broker_add', s => md.addBroker(id, s.source_name, data),
      'add broker failed', { rethrow: true })
  }
  async function removeBroker(id: number, brokerName: string): Promise<void> {
    await runOp(id, 'broker_remove', s => md.removeBroker(id, s.source_name, brokerName),
      'remove broker failed', { requireIdle: true, rethrow: true })
  }
  async function updateBroker(id: number, brokerName: string, broker: BrokerEntry): Promise<void> {
    await runOp(id, 'broker_update', s => md.updateBroker(id, s.source_name, brokerName, broker),
      'update broker failed', { requireIdle: true })
  }
  async function selectBroker(id: number, brokerName: string): Promise<void> {
    await runOp(id, 'broker_select', s => md.selectBroker(id, s.source_name, brokerName),
      'select broker failed', { requireIdle: true })
  }
  async function addFrontend(id: number, brokerName: string, address: string, label: string): Promise<void> {
    await runOp(id, 'frontend_add', (s) => {
      return s.brokers.some(b => b.name === brokerName)
        ? md.addFrontend(id, s.source_name, brokerName, address, label)
        : Promise.resolve(true)
    }, 'add frontend failed', { requireIdle: true, rethrow: true })
  }
  async function removeFrontend(id: number, brokerName: string, address: string): Promise<void> {
    await runOp(id, 'frontend_remove', (s) => {
      return s.brokers.some(b => b.name === brokerName)
        ? md.removeFrontend(id, s.source_name, brokerName, address)
        : Promise.resolve(true)
    }, 'remove frontend failed', { requireIdle: true, rethrow: true })
  }
  async function editFrontend(id: number, brokerName: string, oldAddress: string, newAddress: string): Promise<void> {
    await runOp(id, 'frontend_edit', (s) => {
      if (oldAddress === newAddress) return Promise.resolve(true)  // 值未改变不下发
      return s.brokers.some(b => b.name === brokerName)
        ? md.editFrontend(id, s.source_name, brokerName, oldAddress, newAddress)
        : Promise.resolve(true)
    }, 'edit frontend failed', { requireIdle: true })
  }
  async function setFrontendEnabled(id: number, brokerName: string, address: string, enabled: boolean): Promise<void> {
    await runOp(id, 'frontend_toggle', (s) => {
      return s.brokers.some(b => b.name === brokerName)
        ? md.setFrontendEnabled(id, s.source_name, brokerName, address, enabled)
        : Promise.resolve(true)
    }, 'toggle frontend failed', { requireIdle: true })
  }
  async function setSubscribeParams(id: number, patch: SubscribeParamsBody): Promise<void> {
    await runOp(id, 'subscribe_params', s => md.setSubscribeParams(id, s.source_name, patch),
      'set subscribe params failed')
  }

  /// 添加并启动行情源(设计 §3.1)。已有记录复用 id; 无则 create 后暂存, 等 WS 晋升。
  /// page 大小不再由 UI 传参: 由 master 读配置文件 configs/<source>.json 决定 (契约 03 §参数可改性)。
  async function addAndStartSource(processName: string, displayName: string): Promise<void> {
    const prefix = 'dzmd_'
    if (!processName.startsWith(prefix)) {
      error.value = `invalid market source process name | name=${processName}`
      return
    }
    if (pendingCreations.value[processName]) return
    const existing = sources.value.find(s => s.source_name === processName)
    if (existing?.startPending) return
    const sourceType = processName.slice(prefix.length).toUpperCase()
    try {
      let sourceId: number
      if (existing) {
        sourceId = existing.id
      } else {
        const created = await marketSourcesApi.create({
          source_type: sourceType,
          source_name: processName,
          display_name: displayName,
        })
        sourceId = created.id
        pendingCreations.value[processName] = {
          id: created.id,
          source_type: created.source_type,
          source_name: created.source_name,
          display_name: created.display_name,
          ui_card: created.ui_card,
        }
      }
      if (existing) {
        displayOverrides.value[processName] = displayName
        expandedIds.value[sourceId] = true
      }
      const ok = await process.start(sourceId, processName, {
        display_name: displayName,
      })
      if (!ok) throw new Error('start failed')
      // 排程/自动登录由 WS auto_login 帧驱动 (契约 04), 无需 REST 补拉
      void refreshAvailable()
    } catch {
      if (pendingCreations.value[processName]) delete pendingCreations.value[processName]
      error.value = `add and start market source failed | name=${processName}`
    }
  }

  /// 刷新后端真实扫描到的可用行情源列表。
  async function refreshAvailable(): Promise<void> {
    if (availableLoading.value) return
    availableLoading.value = true
    try {
      availableSources.value = await marketSourcesApi.listAvailable()
    } catch {
      availableSources.value = []
    } finally {
      availableLoading.value = false
    }
  }

  /// "删除此行情源": best-effort 停进程 + 保留 DB 记录; RTN 成功 ack 后移除卡片。
  async function removeSource(id: number): Promise<void> {
    await runOp(id, 'remove', s => process.removeSource(id, s.source_name), 'remove market source failed')
  }

  // ===== Process control =====
  async function start(id: number): Promise<void> {
    await runOp(id, 'start', s => process.start(id, s.source_name), 'start failed')
  }
  async function stop(id: number): Promise<void> {
    await runOp(id, 'stop', s => process.stop(id, s.source_name), 'stop failed')
  }

  // ===== Batch operations =====
  async function batchLogin(): Promise<void> {
    const targets = sources.value.filter(s => s.loginState !== 'online' && !s.loginPending)
    await Promise.all(targets.map(s => login(s.id)))
  }
  async function batchLogout(): Promise<void> {
    const targets = sources.value.filter(s => s.loginState === 'online' && !s.logoutPending)
    await Promise.all(targets.map(s => logout(s.id)))
  }
  async function batchToggleAutoLogin(enabled: boolean): Promise<void> {
    const targets = sources.value.filter(s => s.auto_login !== enabled && !s.autoLoginPending)
    await Promise.all(targets.map(s => toggleAutoLogin(s.id, enabled)))
  }

  function clearError(): void {
    error.value = null
  }

  return {
    sources,
    loading,
    error,
    sourceCount,
    availableSources,
    availableLoading,
    refreshAvailable,
    loadSources,
    toggleExpand,
    login,
    logout,
    toggleAutoLogin,
    addSchedule,
    removeSchedule,
    addBroker,
    removeBroker,
    updateBroker,
    selectBroker,
    addFrontend,
    removeFrontend,
    editFrontend,
    setFrontendEnabled,
    setSubscribeParams,
    addAndStartSource,
    removeSource,
    start,
    stop,
    batchLogin,
    batchLogout,
    batchToggleAutoLogin,
    clearError,
  }
})