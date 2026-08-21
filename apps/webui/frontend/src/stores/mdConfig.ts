import { defineStore } from 'pinia'
import { ref, watch } from 'vue'
import { marketSourcesApi } from '@/api/marketSources'
import type { AddBrokerBody, AutoLoginBody, SubscribeParamsBody, ShmConfigPatch } from '@/api/marketSources'
import type { BrokerEntry, BrokerFrontend, MdConfigPayload, MdRtnConfigPayload, MdRtnStatusPayload } from '@/types/api'
import type { ShmConfigView } from '@/types/mirror'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import { marketSourcePending, clearByPrefix as clearMSPrefix } from '@/composables/marketSourcePending'
import { createOperationRunner } from '@/composables/useOperation'
import { useProgressStore } from '@/stores/progress'
import { progressLoginState } from '@/composables/marketSourceView'

// 设计 §5.2：MD 配置/状态领域（marketSources 的 md_rtn_config / md_rtn_status 拆出）
// - configs:  md_rtn_config 帧镜像（source → MdConfigPayload，后到覆盖先到）
// - statuses: md_rtn_status 帧镜像（source → MdRtnStatusPayload，后到覆盖先到）
// - autoLogins: auto_login 帧镜像（契约 auto-login：source → {enabled, schedules}，后到覆盖先到）
// - 操作: pending 迁移 usePending（key: source:{id}:{op}，配置类操作 timeout 10s）
//   pending 语义与 marketSources.ts 现有实现逐项一致（拆 store 不得改变清理触发点）：
//     HTTP 成功不清 pending（keepPendingOnSuccess 默认 true，等 WS RTN 帧清）
//     HTTP 失败清 pending + 返回 false；超时兜底（10s，usePending 超时清 + toast）
// - applyMdConfig: 到达时清该 source 全部配置类 pending——对照 setMdConfig 批量清
//   各配置字段语义以契约 md-config MdConfig schema 为准（逐字段核对），用 clearByPrefix 逐 op 前缀批量清。
//   注意不能用 source:{id}: 全量前缀——pending 为跨 store 共享的模块级命名空间，
//   会误伤 process store 的 source:{id}:start/stop/remove 与 login/logout。
// - applyMdStatus: 仅写镜像；login/logout pending 由 RTN_PROGRESS 状态转移驱动
//   （契约 progress 消费者约定: current>=max 清 login、current<=min 清 logout,
//    progressLoginState 为唯一实现, 见 composables/marketSourceView.ts; 禁止依赖 desc 文案）
// - applyAutoLogin: 到达时清排程类 pending（auto_login/schedule_add/schedule_remove）
// 本 store 不持有 sources 列表（Task 5 聚合层回填），通过 name→id 映射定位 pending key
// （与 process store 模式一致）。isStateIdle 状态保护（契约 §状态保护）依赖 sources
// 列表，由 Task 5 聚合层/UI 承担，本 store 不重复实现。
const CONFIG_OP_TIMEOUT_MS = 10_000

export type MdConfigOp =
  | 'login' | 'logout' | 'auto_login'
  | 'schedule_add' | 'schedule_remove'
  | 'broker_add' | 'broker_remove' | 'broker_update' | 'broker_select'
  | 'frontend_add' | 'frontend_remove' | 'frontend_edit' | 'frontend_toggle'
  | 'subscribe_params' | 'shm_config'

// 操作结果: boolean(成功/HTTP 失败) 或 PENDING_TIMEOUT(超时, usePending 已 toast)。
// 薄代理对 PENDING_TIMEOUT 不设 error, 避免超时双 toast(§7.2)。
export type MdConfigOpResult = boolean | typeof PENDING_TIMEOUT

// 各操作的可读超时文案(usePending opLabel, 超时 toast 用「{opLabel}超时」)
const OP_LABELS: Record<MdConfigOp, string> = {
  login: '行情源登录',
  logout: '行情源登出',
  auto_login: '自动登录',
  schedule_add: '添加时段',
  schedule_remove: '删除时段',
  broker_add: '添加经纪商',
  broker_remove: '删除经纪商',
  broker_update: '修改经纪商',
  broker_select: '切换经纪商',
  frontend_add: '添加前置',
  frontend_remove: '删除前置',
  frontend_edit: '修改前置',
  frontend_toggle: '切换前置',
  subscribe_params: '修改订阅参数',
  shm_config: '修改 SHM 配置',
}

// applyMdConfig 到达时批量清理的配置类 op（对照 setMdConfig 清的 8 个 broker/frontend
// pending 字段：brokerAddPending / brokerRemovePending / brokerSelectPending /
// brokerFieldEditPending / frontendAddPending / frontendRemovePending / frontendEditPending /
// frontendTogglePending）
// 不含 login/logout（由 RTN_PROGRESS 状态转移清）、排程类（由 applyAutoLogin 清，
// 契约 auto-login：排程操作回 RTN_AUTO_LOGIN 而非 RTN_MD_CONFIG）与进程类 op（process store 领域）。
// frontend 四个操作拆独立 op：现有 spec "各控件独立等待状态, 可同时发起"
// （marketSources.ts:32-33），四个 frontend pending 字段互不干扰。
export const MD_CONFIG_OPS: readonly MdConfigOp[] = [
  'broker_add', 'broker_remove', 'broker_update', 'broker_select',
  'frontend_add', 'frontend_remove', 'frontend_edit', 'frontend_toggle',
  'subscribe_params',
]

// 排程类 op：由 applyAutoLogin（RTN_AUTO_LOGIN，契约 auto-login）批量清理
export const AUTO_LOGIN_OPS: readonly MdConfigOp[] = ['auto_login', 'schedule_add', 'schedule_remove']

export const useMdConfigStore = defineStore('mdConfig', () => {
  const configs = ref<Record<string, MdConfigPayload>>({})
  const statuses = ref<Record<string, MdRtnStatusPayload>>({})
  // md_shm_config 帧镜像（契约 shm，dzmd_* 通用层 schema 非 CTP 专属）：
  // 全量覆盖（后到覆盖先到），非法整体忽略。到达时清 shm_config pending。
  const shmConfigs = ref<Record<string, ShmConfigView>>({})
  const autoLogins = ref<Record<string, { enabled: boolean; schedules: { login_time: string; logout_time: string }[] }>>({})

  // source 名（如 'dzmd_ctp'）→ source id 映射：操作时记录，
  // RTN 回调（仅带进程名 source）据此定位 pending key（source:{id}:{op}）
  const nameToId = ref<Record<string, number>>({})

  // 行情源领域共享 pending（P3）：process/mdConfig/marketSources 围绕同一批
  // source:{id}:{op} 操作，必须共用同一实例以便 RTN 批量清理与前端映射。
  // P3 任务 2: 经领域 runner 使用该实例（防重入/name→id/run/结果映射收敛见 useOperation.ts）。
  const runner = createOperationRunner({
    pending: marketSourcePending,
    timeout: CONFIG_OP_TIMEOUT_MS,
    opKey,
    nameToId,
  })

  function opKey(id: number, op: MdConfigOp): string {
    return `source:${id}:${op}`
  }

  // 清一个 id 的全部配置类 pending（现有 setMdConfig 批量清语义），
  // 逐 op 前缀 clearByPrefix——精确限定配置类 op，不误伤同 key 空间的
  // process store（start/stop/remove）与 login/logout（applyProgress 负责）
  function clearConfigPending(id: number): void {
    for (const op of MD_CONFIG_OPS) {
      clearMSPrefix(opKey(id, op))
    }
  }

  // md_rtn_config 帧：单条完整覆盖（后到覆盖先到），非法 payload 忽略不写
  // 到达时清该 source 全部配置类 pending（现有 setMdConfig 批量清语义）
  function applyMdConfig(payload: MdRtnConfigPayload): void {
    if (!payload || !payload.source || !payload.config) return
    configs.value[payload.source] = payload.config
    const id = nameToId.value[payload.source]
    if (id !== undefined) clearConfigPending(id)
  }

  // md_rtn_status 帧：单条完整覆盖（后到覆盖先到），非法 payload 忽略不写。
  // 不再承担 pending 清理（契约 md-status 无状态字段）：login/logout pending 由
  // RTN_PROGRESS 状态转移驱动（契约 progress，见下方 watch）。
  function applyMdStatus(payload: MdRtnStatusPayload): void {
    if (!payload || !payload.source) return
    statuses.value[payload.source] = payload
  }

  // md_shm_config 帧（契约 shm）: 全量覆盖, 非法整体忽略; 到达时清 shm_config pending
  function applyMdShmConfig(source: string, payload: unknown): void {
    const p = payload as Partial<Record<keyof ShmConfigView, unknown>> | null | undefined
    if (!source || !p || typeof p !== 'object') return
    const isNum = (v: unknown): boolean => typeof v === 'number' && Number.isFinite(v)
    if (!isNum(p.page_size_mb) || !isNum(p.check_interval_min)
      || !isNum(p.check_pages) || !isNum(p.check_bytes)) return
    if (typeof p.preload_points !== 'object' || p.preload_points === null) return
    for (const v of Object.values(p.preload_points)) {
      const pt = v as { pages?: unknown; bytes?: unknown }
      if (typeof pt !== 'object' || pt === null) return
      if (!isNum(pt.pages) || !isNum(pt.bytes)) return
    }
    shmConfigs.value[source] = p as ShmConfigView
    const id = nameToId.value[source]
    if (id !== undefined) clearMSPrefix(opKey(id, 'shm_config'))
  }

  // auto_login 帧（契约 auto-login）：单条完整覆盖（后到覆盖先到），非法 payload 忽略不写。
  // 到达时清该 source 排程类 pending（auto_login/schedule_add/schedule_remove）。
  function applyAutoLogin(source: string, payload: unknown): void {
    const p = payload as { enabled?: unknown; schedules?: unknown } | null | undefined
    if (!p || typeof p !== 'object') return
    if (typeof p.enabled !== 'boolean' || !Array.isArray(p.schedules)) return
    autoLogins.value[source] = {
      enabled: p.enabled,
      schedules: p.schedules as { login_time: string; logout_time: string }[],
    }
    const id = nameToId.value[source]
    if (id === undefined) return
    for (const op of AUTO_LOGIN_OPS) {
      clearMSPrefix(opKey(id, op))
    }
  }

  // ===== 登录/登出 pending 驱动 (契约 progress: RTN_PROGRESS 状态转移) =====
  // dzmd_ctp 的 MdStateMachine 在状态转移时推送 RTN_PROGRESS（md_state.cpp set_state）,
  // 数值映射（progressLoginState, 见 composables/marketSourceView.ts）为跨进程稳定契约:
  //   current>=max = 登录完成 (LoggedIn); current<=min = 已登出 (Idle); 其余 = 中间态
  // 禁止依赖 desc 文案判定（desc 为展示文案, 契约 progress）。
  const progressStore = useProgressStore()
  watch(
    () => progressStore.progress,
    (progress) => {
      for (const [instanceId, p] of Object.entries(progress)) {
        // 仅处理行情进程实例 (dzmd_*); td 实例格式 "{进程}:{账户}" 不参与
        if (!instanceId.startsWith('dzmd_')) continue
        const sourceId = nameToId.value[instanceId]
        if (sourceId === undefined) continue
        const state = progressLoginState(p)
        if (state === 'online') {
          clearMSPrefix(opKey(sourceId, 'login'))
        } else if (state === 'offline') {
          clearMSPrefix(opKey(sourceId, 'logout'))
        }
      }
    },
    { deep: true },
  )

  // ===== 操作（pending 迁移 usePending；签名带 sourceName 用于 name→id 映射）=====
  // 语义与 marketSources.ts 现有实现一致：HTTP 成功不清 pending（等 WS RTN 清），
  // HTTP 失败清 pending + 返回 false，超时（10s）清 + toast。
  // 现有 addBroker/removeBroker/addFrontend/removeFrontend 失败 re-throw 的
  // F-C10 语义（Modal 保持打开）依赖调用方错误信息，Task 5 聚合层接入时处理。
  async function login(id: number, sourceName: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'login')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.login(id), OP_LABELS.login)
  }

  async function logout(id: number, sourceName: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'logout')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.logout(id), OP_LABELS.logout)
  }

  // ===== 自动登录/登出排程 (契约 auto-login) =====
  // 单一真相源为 auto_login 镜像（WS auto_login 帧）；提交均全量下发
  // {enabled, schedules} 直发 SET_AUTO_LOGIN，RTN_AUTO_LOGIN 到达后清 pending。

  /// 当前排程镜像（供操作拼接全量提交用）
  function currentAutoLogin(sourceName: string): AutoLoginBody {
    const cur = autoLogins.value[sourceName]
    return {
      enabled: cur?.enabled ?? false,
      schedules: cur?.schedules ?? [],
    }
  }

  /// 全量提交 SET_AUTO_LOGIN + 乐观更新，HTTP 失败/超时时回滚乐观值。
  /// 回滚必要性：网关侧校验失败契约 auto-login 必回 RTN（旧值），由 applyAutoLogin 权威覆盖；
  /// 但 HTTP 失败（如 503 进程未运行）或超时无任何 RTN——若不回滚，虚假镜像会残留
  /// 并被下一次全量提交再次下发（enabled=true 意外开启自动登录是安全敏感操作）。
  async function submitAutoLogin(
    id: number,
    sourceName: string,
    key: string,
    opLabel: string,
    build: (cur: AutoLoginBody) => AutoLoginBody,
  ): Promise<MdConfigOpResult> {
    const prev = autoLogins.value[sourceName]
    const body = build(currentAutoLogin(sourceName))
    // runner.execute 内含乐观更新 + 结果映射；失败/超时（无 RTN）需回滚乐观值
    const result = await runner.execute(key, () => {
      autoLogins.value[sourceName] = body  // 乐观更新; RTN_AUTO_LOGIN 权威覆盖
      return marketSourcesApi.setAutoLogin(id, body)
    }, opLabel)
    if (result === false || result === PENDING_TIMEOUT) {
      // HTTP 失败 / 超时: 无 RTN 到达, 回滚乐观值
      if (prev === undefined) {
        delete autoLogins.value[sourceName]
      } else {
        autoLogins.value[sourceName] = prev
      }
    }
    return result
  }

  async function toggleAutoLogin(id: number, sourceName: string, enabled: boolean): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'auto_login')
    if (runner.busy(key)) return false
    // 守卫：镜像未就绪禁止提交。RFC7386 全量提交中 schedules 出现即整体覆盖，
    // 兜底值 {enabled:false, schedules:[]} 会清空网关侧已有排程（破坏性）
    if (!autoLogins.value[sourceName]) return false
    runner.assign(id, sourceName)
    return submitAutoLogin(id, sourceName, key, OP_LABELS.auto_login,
      cur => ({ ...cur, enabled }))
  }

  async function addSchedule(id: number, sourceName: string, loginTime: string, logoutTime: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'schedule_add')
    if (runner.busy(key)) return false
    // 守卫：同 toggleAutoLogin——镜像未就绪时禁止全量提交
    if (!autoLogins.value[sourceName]) return false
    runner.assign(id, sourceName)
    const cur = currentAutoLogin(sourceName)
    if (cur.schedules.some(s => s.login_time === loginTime && s.logout_time === logoutTime)) {
      return true  // 重复时段: 幂等成功（目标状态已达成, 不下发）
    }
    const schedules = [...cur.schedules, { login_time: loginTime, logout_time: logoutTime }]
    return submitAutoLogin(id, sourceName, key, OP_LABELS.schedule_add,
      c => ({ ...c, schedules }))
  }

  async function removeSchedule(id: number, sourceName: string, loginTime: string, logoutTime: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'schedule_remove')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    const cur = currentAutoLogin(sourceName)
    const schedules = cur.schedules.filter(s => !(s.login_time === loginTime && s.logout_time === logoutTime))
    if (schedules.length === cur.schedules.length) {
      return true  // 时段不存在（镜像未就绪/已删除）: 幂等成功, 无操作
    }
    return submitAutoLogin(id, sourceName, key, OP_LABELS.schedule_remove,
      c => ({ ...c, schedules }))
  }

  async function addBroker(id: number, sourceName: string, data: AddBrokerBody): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'broker_add')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.addBroker(id, data), OP_LABELS.broker_add)
  }

  async function removeBroker(id: number, sourceName: string, brokerName: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'broker_remove')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.removeBroker(id, brokerName), OP_LABELS.broker_remove)
  }

  async function updateBroker(id: number, sourceName: string, brokerName: string, broker: BrokerEntry): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'broker_update')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.updateBroker(id, brokerName, broker), OP_LABELS.broker_update)
  }

  async function selectBroker(id: number, sourceName: string, brokerName: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'broker_select')
    if (runner.busy(key)) return false
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.setCurrentBroker(id, brokerName), OP_LABELS.broker_select)
  }

  // ===== Frontend 系列（从 configs 镜像读 broker.frontends 构造新列表，与现有行为一致：
  // 镜像未建立/无此 broker 时不发请求）=====
  async function addFrontend(id: number, sourceName: string, brokerName: string, address: string, label: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'frontend_add')
    if (runner.busy(key)) return false
    const broker = configs.value[sourceName]?.brokers.find(b => b.name === brokerName)
    if (!broker) return false
    runner.assign(id, sourceName)
    const newFrontends: BrokerFrontend[] = [...broker.frontends, { address, label, enabled: false }]
    return runner.execute(key, () => marketSourcesApi.updateFrontends(id, brokerName, newFrontends), OP_LABELS.frontend_add)
  }

  async function removeFrontend(id: number, sourceName: string, brokerName: string, address: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'frontend_remove')
    if (runner.busy(key)) return false
    const broker = configs.value[sourceName]?.brokers.find(b => b.name === brokerName)
    if (!broker) return false
    runner.assign(id, sourceName)
    const newFrontends = broker.frontends.filter(f => f.address !== address)
    return runner.execute(key, () => marketSourcesApi.updateFrontends(id, brokerName, newFrontends), OP_LABELS.frontend_remove)
  }

  async function editFrontend(id: number, sourceName: string, brokerName: string, oldAddress: string, newAddress: string): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'frontend_edit')
    if (runner.busy(key)) return false
    const broker = configs.value[sourceName]?.brokers.find(b => b.name === brokerName)
    if (!broker) return false
    if (oldAddress === newAddress) return false  // 值未改变不下发（现有语义）
    runner.assign(id, sourceName)
    const newFrontends = broker.frontends.map(f =>
      f.address === oldAddress ? { ...f, address: newAddress } : f
    )
    return runner.execute(key, () => marketSourcesApi.updateFrontends(id, brokerName, newFrontends), OP_LABELS.frontend_edit)
  }

  async function setFrontendEnabled(id: number, sourceName: string, brokerName: string, address: string, enabled: boolean): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'frontend_toggle')
    if (runner.busy(key)) return false
    const broker = configs.value[sourceName]?.brokers.find(b => b.name === brokerName)
    if (!broker) return false
    runner.assign(id, sourceName)
    const newFrontends = broker.frontends.map(f =>
      f.address === address ? { ...f, enabled } : f
    )
    return runner.execute(key, () => marketSourcesApi.updateFrontends(id, brokerName, newFrontends), OP_LABELS.frontend_toggle)
  }

  // ===== 订阅参数 (契约 md-config SetSubscribeParams: 单字段 patch, 缺失=保留旧值, 无状态保护) =====
  async function setSubscribeParams(
    id: number,
    sourceName: string,
    patch: SubscribeParamsBody,
  ): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'subscribe_params')
    if (runner.busy(key)) return false
    if (Object.keys(patch).length === 0) return true  // 空提交幂等成功, 不下发
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.setSubscribeParams(id, patch), OP_LABELS.subscribe_params)
  }

  // ===== SHM 行情通道配置 (契约 shm: merge patch, page_size_mb 不可变不在此列) =====
  async function setShmConfig(
    id: number,
    sourceName: string,
    patch: ShmConfigPatch,
  ): Promise<MdConfigOpResult> {
    const key = runner.opKey(id, 'shm_config')
    if (runner.busy(key)) return false
    if (Object.keys(patch).length === 0) return true  // 空提交幂等成功, 不下发
    runner.assign(id, sourceName)
    return runner.execute(key, () => marketSourcesApi.setShmConfig(id, patch), OP_LABELS.shm_config)
  }

  return {
    configs,
    statuses,
    autoLogins,
    applyMdConfig,
    applyMdStatus,
    applyAutoLogin,
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
    shmConfigs,
    applyMdShmConfig,
    setShmConfig,
  }
})
