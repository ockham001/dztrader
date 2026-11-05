import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { tdApi } from '@/api/td'
import { usePending, PENDING_TIMEOUT } from '@/composables/usePending'
import {
  TD_ACCOUNT_ERROR_TEXT,
  TD_ACCOUNT_NETWORK_ERROR_TEXT,
  tdAccountErrorCodeOf,
  tdAccountErrorStatusOf,
  tdAccountErrorText,
  tdAccountServerErrorText,
} from '@/composables/tdAccountErrors'
import { useProcessStore } from '@/stores/process'
import { useProgressStore } from '@/stores/progress'
import type {
  TdAccountConfig,
  TdAccountsDegradedReason,
  TdConfigAccountList,
  TdConfigPayload,
  TdState,
  TdStatusPayload,
  TdUpdateAccountBody,
} from '@/types/api'
import type { ProgressView } from '@/stores/progress'

/**
 * 交易账户配置 store（契约 rest §2.9.7/§2.9.9、契约 td-config §1/§2/§5、契约 td-status、契约 webui-ws §2.1/§2.2/§2.3）。
 *
 * 数据来源分工（三种，职责不重叠）：
 * - **账户列表视角 = 注册表**（`GET /api/td/accounts`，契约 rest §2.9.9）：跨网关的账户全集，
 *   也是唯一性预检的依据。注册表是投影（`account_id → gateway` + 最近一次配置帧的 `enabled`），
 *   单网关的配置镜像拼不出全集，所以列表必须以它为准。
 * - **配置细节 = 配置镜像**（WS `td_rtn_config`，契约 td-config §4/§8）：按网关全量覆盖
 *   （`configs[gateway]`）。提供 `ext`（密码恒为 `"****"`）、`risk_control_enabled`、
 *   `currency_id` 与三个查询间隔——注册表不携带这些字段。
 * - **细状态 = 状态镜像**（WS `td_rtn_status`，契约 td-status §3）：按 `网关名:账户ID` 全量覆盖。
 *   登录态/交易日/登录时间只从它取，**不从 `td_rtn_config` 推导**（契约 rest §4 前端义务）。
 *
 * 为什么进度不取 `td_rtn_status` 的 `progress_*`：契约 td-status §2.1 明示前端进度展示以
 * 契约 progress 为**唯一入口**（`td_rtn_status` 携带进度是历史兼容）。这里读 `progress` 镜像，
 * key = `网关名:账户ID`（与 `td_rtn_status` 的 instance_id 同形）。
 *
 * pending 语义（契约 webui-ws §5 第 1/2 条 + 契约 td-config §4「回报先于/等于前端生效信号」）：
 * - key = `td:{gateway}:{account}:{op}`（op ∈ add/update/remove/toggle_enabled）；
 * - **HTTP 200 不清 pending**——响应只表示已写入事件通道，不代表业务已生效；
 * - 清除唯一来源 = 该网关的 `td_rtn_config` 到达（契约 td-config §4 场景 1/2/3）。清除粒度是
 *   该网关下**全部** pending（配置帧是整网关全量，`clearByPrefix` 前缀 = `td:{gateway}:`），
 *   包括删除操作的 pending：RemoveAccount 成功后帧里该账户消失，判定依据是「帧到达」而不是
 *   「账户消失」——否则帧里本来就没有该账户时 pending 会悬挂；
 * - 兜底超时 {@link TD_ACCOUNT_OP_TIMEOUT_MS}（5s，契约 webui-ws §5 第 1 条建议值）。
 *
 * 唯一性预检（契约 rest §2.9.7 预检 4 + §4 前端义务）：提交前查注册表，发现 `account_id`
 * 已被任何网关占用（含已禁用/离线）→ 前端直接提示冲突并**不发请求**。这只是体验优化：
 * 权威校验在 dzweb（409 `account_exists`），两个客户端同时添加时只有服务端能裁决。
 *
 * **降级时不做预检**（重要取舍）：注册表 `available:false` 表示"无账户"不可信
 *（与"确实没有账户"不可区分，契约 rest §2.9.9）。此时若按「查不到 = 无冲突」放行，
 * 等于把一个必然不完整的注册表当成唯一性依据；若反过来禁用提交，又会在 dzweb 可正常
 * 转发写帧的情况下把功能锁死。因此：**降级时允许提交、跳过预检，把裁决完全交给 dzweb 的
 * 409 `account_exists`**（服务端才是权威校验点）。页面只展示降级提示，不因降级禁用添加按钮。
 */

/** 账户配置操作 pending 兜底超时（契约 webui-ws §5 第 1 条建议 5s） */
export const TD_ACCOUNT_OP_TIMEOUT_MS = 5_000

/**
 * 本领域独立 pending 实例（账户配置页无跨 store pending 交互，与 settings/sysmon 同型）。
 * 模块级单例导出供 store 与测试共享同一实例（可重置/可断言）。
 */
export const tdAccountsPending = usePending()

/** 账户配置写操作（pending key 的 op 段） */
export type TdAccountOp = 'add' | 'update' | 'remove' | 'toggle_enabled'

/** 各操作的可读超时文案（usePending opLabel，超时 toast 用「{opLabel}超时」） */
const OP_LABELS: Record<TdAccountOp, string> = {
  add: '添加交易账户',
  update: '修改交易账户',
  remove: '删除交易账户',
  toggle_enabled: '启停交易账户',
}

/**
 * 写操作结果：
 * - `ok:true` = HTTP 已受理（pending 保持挂起，等 `td_rtn_config` 清）；
 * - `kind:'conflict'` = 唯一性预检冲突，**未发请求**（页面据此做字段级提示）；
 * - `kind:'failed'` = HTTP 失败（`message` 已按 code 映射中文，不回显英文）；
 * - `kind:'busy'` = 同 key 已有在途请求（防重入）；
 * - `kind:'timeout'` = 兜底超时（usePending 已 toast，调用方不要重复提示）。
 */
export type TdAccountSubmitResult =
  | { ok: true }
  | { ok: false; kind: 'conflict'; message: string }
  | { ok: false; kind: 'failed'; message: string }
  | { ok: false; kind: 'busy' }
  | { ok: false; kind: 'timeout' }

/** 冲突提示文案（注册表条目可展示归属网关，帮用户定位是哪个网关占用了 ID） */
export function accountConflictText(conflict: TdConfigAccountList): string {
  return `账户 ID「${conflict.account_id}」已被交易网关 ${conflict.gateway} 占用（含已禁用/离线账户），请换一个账户 ID`
}

/**
 * 从交易网关进程名派生接口类型（契约 td-config §1）：
 * 去掉 `dztd_` 前缀得到 tail，tail 中第一个 `_` 之前的部分即 interface_type（不含 `_` 时 = tail 本身）。
 * 非法进程名（无 `dztd_` 前缀 / 前缀后为空）返回空串——调用方据此不渲染任何柜台表单。
 */
export function tdInterfaceType(gateway: string): string {
  if (!gateway.startsWith('dztd_')) return ''
  const tail = gateway.slice('dztd_'.length)
  if (tail === '') return ''
  const underscore = tail.indexOf('_')
  return underscore < 0 ? tail : tail.slice(0, underscore)
}

/** 细状态 11 态的展示文案（契约 td-status §2.2；未知值原样显示，不吞掉事实） */
const TD_STATE_TEXT: Record<TdState, string> = {
  Idle: '未启动',
  Connecting: '连接中',
  Connected: '已连接',
  Authenticating: '认证中',
  Authenticated: '已认证',
  LoggingIn: '登录中',
  LoggedIn: '已登录',
  Confirming: '确认结算单',
  LoadingInstruments: '加载合约',
  Ready: '就绪',
  Disconnected: '连接断开',
}

/** 细状态 → 中文文案；未到达（null）显示占位 `—`，未知枚举原样显示 */
export function tdStateText(state: TdState | string | null | undefined): string {
  if (state === null || state === undefined || state === '') return '—'
  return (TD_STATE_TEXT as Record<string, string>)[state] ?? state
}

/** 降级原因文案（契约 rest §2.9.9 只有 no_gateway_snapshot 一种原因） */
export const TD_ACCOUNTS_DEGRADED_TEXT =
  '账户注册表暂不可用（尚未收到任何交易网关配置），列表可能不完整，正在自动重试'

/** 交易账户行（页面渲染单元 = 注册表条目 ∪ 配置镜像 ∪ 状态镜像 ∪ 进度镜像） */
export interface TdAccountRow {
  account_id: string
  gateway: string
  /** 从网关进程名派生的接口类型（契约 td-config §1，如 ctp）；表单字段按它选择 */
  interfaceType: string
  /** 是否启用（注册表权威；注册表不含该账户时回落配置镜像） */
  enabled: boolean
  riskControlEnabled: boolean
  currencyId: string
  /** 该账户的配置（含脱敏 ext）；配置镜像未到达为 null */
  config: TdAccountConfig | null
  /** 账户级细状态（WS td_rtn_status）；未到达为 null */
  status: TdStatusPayload | null
  /** 进度（契约 progress，instance_id = 网关:账户）；未到达为 null */
  progress: ProgressView | null
  /** 网关进程是否 Running（进程镜像）；镜像未到达为 false（页面据此提示先启动网关） */
  gatewayRunning: boolean
}

export const useTdAccountsStore = defineStore('tdAccounts', () => {
  // === 账户注册表（REST 基线，契约 rest §2.9.9）===
  /** 注册表条目（跨网关全集，按 account_id 升序） */
  const registry = ref<TdConfigAccountList[]>([])
  /** 注册表可用性；false = 降级（契约 rest §2.9.9，**不是**空列表） */
  const available = ref(true)
  const degradedReason = ref<TdAccountsDegradedReason | null>(null)
  const loading = ref(false)
  const initialized = ref(false)
  const error = ref<string | null>(null)

  // === WS 镜像 ===
  /** 交易网关配置镜像（key = 网关进程名；契约 td-config §4 全量覆盖） */
  const configs = ref<Record<string, TdConfigPayload>>({})
  /** 账户级细状态镜像（key = 网关名:账户ID；契约 td-status §3 全量覆盖） */
  const statuses = ref<Record<string, TdStatusPayload>>({})

  // === 内部状态（非响应式）===
  /**
   * 请求代号：只有最后一次发起的注册表请求允许写状态
   *（`data_changed` 重拉与页面挂载可能重叠，旧响应晚到不得覆盖新基线）。
   */
  let requestSeq = 0
  /** 注册表自愈重试定时器（降级时按固定间隔重试，与 td 列表退避同属「恢复要快」口径） */
  let retryHandle: ReturnType<typeof setTimeout> | null = null
  let disposed = false

  const processStore = useProcessStore()
  const progressStore = useProgressStore()

  // === 派生状态 ===

  /**
   * 已注册的交易网关名单（契约 rest §2.9.7 预检 2：进程配置中的 `dztd_*`）。
   * 来源 = process_config 镜像（契约 process）——与 dzweb 的预检口径同源。
   */
  const gatewayNames = computed<string[]>(() =>
    Object.keys(processStore.configs).filter(name => name.startsWith('dztd_')).sort())

  /** 可用网关（带上运行态；页面下拉用，运行态只做提示，不放行/拦截由 dzweb 裁决） */
  const gatewayOptions = computed<{ name: string; running: boolean }[]>(() =>
    gatewayNames.value.map(name => ({
      name,
      running: processStore.statuses[name]?.state === 'Running',
    })))

  /** 进程是否 Running（页面据此提示「先启动网关」；决策权仍在 dzweb 预检 3） */
  function isGatewayRunning(gateway: string): boolean {
    return processStore.statuses[gateway]?.state === 'Running'
  }

  /** 账户的注册表归属（找不到 = 注册表里没有该账户） */
  function registryEntry(accountId: string): TdConfigAccountList | undefined {
    return registry.value.find(entry => entry.account_id === accountId)
  }

  /** 账户所属网关：注册表优先；注册表缺失时回落配置镜像（唯一网关包含该账户时） */
  function gatewayOf(accountId: string): string | null {
    const entry = registryEntry(accountId)
    if (entry) return entry.gateway
    for (const [gateway, config] of Object.entries(configs.value)) {
      if (config.accounts?.some(a => a.account_id === accountId)) return gateway
    }
    return null
  }

  /**
   * 唯一性预检（契约 rest §2.9.7 预检 4 / §4 前端义务）：返回占用该 ID 的注册表条目，无冲突为 null。
   *
   * **降级时恒返回 null（= 不预检）**：注册表不可用时「查不到」不代表「不存在」，此时既不能
   * 据此拦下提交（会误伤合法添加），也不该把不完整注册表当唯一性依据——交给 dzweb 的 409 裁决。
   * 见文件头「降级时不做预检」的取舍说明。
   */
  function accountIdConflict(accountId: string): TdConfigAccountList | null {
    if (!available.value) return null
    return registryEntry(accountId) ?? null
  }

  /**
   * 账户行（页面渲染单元）：注册表为骨架，配置/状态/进度镜像逐字段补全。
   *
   * **降级时以配置镜像为骨架**（而不是空列表）：注册表不可用只代表「全集不可信」，
   * 各网关已推来的配置仍是真实的账户事实——页面展示它们并同时给出降级提示，而不是让用户
   * 以为账户被删了（契约 rest §2.9.9 约束）。
   */
  const rows = computed<TdAccountRow[]>(() => {
    const merged = new Map<string, TdAccountRow>()

    const makeRow = (accountId: string, gateway: string): TdAccountRow => ({
      account_id: accountId,
      gateway,
      interfaceType: tdInterfaceType(gateway),
      enabled: true,
      riskControlEnabled: false,
      currencyId: 'CNY',
      config: null,
      status: null,
      progress: null,
      gatewayRunning: isGatewayRunning(gateway),
    })

    // 1) 骨架：注册表（可用时）——降级时改由配置镜像提供骨架
    if (available.value) {
      for (const entry of registry.value) {
        const row = makeRow(entry.account_id, entry.gateway)
        row.enabled = entry.enabled
        merged.set(entry.account_id, row)
      }
    }

    // 2) 配置镜像：补全 ext/风控/币种/enabled，并为降级态提供骨架
    for (const [gateway, config] of Object.entries(configs.value)) {
      for (const account of config.accounts ?? []) {
        if (!account || typeof account.account_id !== 'string') continue
        const existing = merged.get(account.account_id)
        const row = existing ?? makeRow(account.account_id, gateway)
        // **enabled 优先取配置镜像（WS 帧，最新），注册表值只作无配置时的回落**：
        // 注册表的 enabled 是「最近一次配置帧的通用层」的投影（契约 rest §2.9.9），但启停是
        // UpdateAccount 而非账户集合变化——dzweb 不会为 enabled 单独广播 data_changed
        //（契约 rest §2.9.7 可观察效果：只有账户集合**增减**才广播），因此注册表中的 enabled
        // 会一直停在旧值直到下次重拉。配置镜像由 td_rtn_config 直接覆盖，是 enabled 的最新真相。
        row.enabled = account.enabled !== false
        row.config = account
        row.riskControlEnabled = account.risk_control_enabled === true
        row.currencyId = account.currency_id ?? 'CNY'
        merged.set(account.account_id, row)
      }
    }

    // 3) 状态镜像（key = 网关名:账户ID）+ 进度镜像（同 key，契约 progress 为进度唯一入口）
    for (const [instanceId, status] of Object.entries(statuses.value)) {
      const separator = instanceId.indexOf(':')
      if (separator <= 0) continue
      const row = merged.get(instanceId.slice(separator + 1))
      if (!row) continue  // 状态镜像里的账户不在注册表/配置镜像中：等配置帧到达，不自造行
      row.status = status
    }
    for (const [instanceId, progress] of Object.entries(progressStore.progress)) {
      const separator = instanceId.indexOf(':')
      if (separator <= 0) continue
      const row = merged.get(instanceId.slice(separator + 1))
      if (!row) continue
      row.progress = progress
    }

    // 注册表已按 account_id 升序返回；配置镜像骨架的降级态也按同一口径排序，避免顺序抖动
    return [...merged.values()].sort((a, b) =>
      a.account_id === b.account_id ? 0 : (a.account_id < b.account_id ? -1 : 1))
  })

  /** 是否「确实没有账户」空态：注册表可用且请求无错且行数为 0（降级/失败都不算空） */
  const isEmpty = computed<boolean>(
    () => available.value && error.value === null && rows.value.length === 0)

  /** 降级提示文案（可用时为空串） */
  const degradedText = computed<string>(() => (available.value ? '' : TD_ACCOUNTS_DEGRADED_TEXT))

  // === pending 查询（页面用） ===

  function opKey(gateway: string, accountId: string, op: TdAccountOp): string {
    return `td:${gateway}:${accountId}:${op}`
  }

  /** 该账户是否有任一在途操作（行级 spinner） */
  function accountPending(accountId: string): boolean {
    const gateway = gatewayOf(accountId)
    if (gateway === null) return false
    return (Object.keys(OP_LABELS) as TdAccountOp[]).some(op => tdAccountsPending.pending[opKey(gateway, accountId, op)] === true)
  }

  /** 账户在指定网关下是否有在途的某类操作 */
  function isOpPending(gateway: string, accountId: string, op: TdAccountOp): boolean {
    return tdAccountsPending.pending[opKey(gateway, accountId, op)] === true
  }

  /** 添加操作的在途态（新账户尚未进注册表，不能用 accountPending 判定） */
  const addPending = computed<boolean>(() =>
    Object.keys(tdAccountsPending.pending).some(key =>
      key.startsWith('td:') && key.endsWith(':add') && tdAccountsPending.pending[key] === true))

  // === 注册表读取 ===

  function clearRetry(): void {
    if (retryHandle !== null) {
      clearTimeout(retryHandle)
      retryHandle = null
    }
  }

  /**
   * 排一次注册表自愈重试（降级期间）。
   * 间隔固定 5s：注册表恢复的唯一触发是「首个 TD_RTN_CONFIG 到达」，没有可调的分级退避依据；
   * 5s 兼顾「恢复后尽快纠正」与「不把 7×24 的 dzweb 打成轮询源」。
   */
  function scheduleRetry(): void {
    if (disposed || retryHandle !== null) return
    retryHandle = setTimeout(() => {
      retryHandle = null
      void loadRegistry()
    }, TD_ACCOUNT_OP_TIMEOUT_MS)
  }

  /**
   * 提交一个写操作：pending 保持到 `td_rtn_config` 到达；HTTP 失败按 code 映射中文文案后返回。
   *
   * `usePending.run` 会吞异常（返回 undefined），因此这里必须在 `fn` 内把错误捕获进闭包变量，
   * 否则失败原因（code → 文案）会丢失。这也是「不得回显英文 error」的唯一落点。
   */
  async function submit(
    key: string,
    opLabel: string,
    request: () => Promise<unknown>,
  ): Promise<TdAccountSubmitResult> {
    if (tdAccountsPending.pending[key] === true) return { ok: false, kind: 'busy' }
    let failure: unknown = null
    let failed = false
    const result = await tdAccountsPending.run(key, async () => {
      try {
        return await request()
      } catch (e) {
        failure = e
        failed = true
        throw e
      }
    }, { timeout: TD_ACCOUNT_OP_TIMEOUT_MS, opLabel, distinguishTimeout: true })
    if (result === PENDING_TIMEOUT) return { ok: false, kind: 'timeout' }
    if (failed) {
      // 文案三分（契约 rest §2.9.7）：
      // - 预检失败：按 code 映射（account_exists / gateway_not_running / ...）；
      // - 有响应但不带 code（如 dzweb 写事件通道失败 503）：按状态码给基础设施文案，
      //   绝不套预检码文案（会误导用户去改账户 ID/权限）；
      // - 无响应（网络中断）：网络文案。
      const code = tdAccountErrorCodeOf(failure)
      const status = tdAccountErrorStatusOf(failure)
      let message: string
      if (code !== undefined) message = tdAccountErrorText(code)
      else if (status !== undefined) message = tdAccountServerErrorText(status)
      else message = TD_ACCOUNT_NETWORK_ERROR_TEXT
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return { ok: true }
  }

  // === 写操作（契约 rest §2.9.7）===

  /**
   * 添加账户（`POST /api/td/accounts`，契约 rest §2.9.7）。
   *
   * 预检顺序（前端部分）：account_id 非空 → 唯一性（注册表可用时）→ 发请求。
   * 其余预检（admin / 网关存在 / 网关 Running / 长度上限）由 dzweb 裁决，前端按 code 展示。
   */
  async function addAccount(gateway: string, account: TdAccountConfig): Promise<TdAccountSubmitResult> {
    const accountId = account.account_id?.trim() ?? ''
    if (accountId === '') return { ok: false, kind: 'failed', message: '账户 ID 不能为空' }
    const conflict = accountIdConflict(accountId)
    if (conflict) return { ok: false, kind: 'conflict', message: accountConflictText(conflict) }
    const key = opKey(gateway, accountId, 'add')
    if (tdAccountsPending.pending[key] === true) return { ok: false, kind: 'busy' }
    return submit(key, OP_LABELS.add, () =>
      tdApi.addAccount({ gateway, account: { ...account, account_id: accountId } }))
  }

  /**
   * 修改账户（`PUT /api/td/accounts/{account_id}`，契约 rest §2.9.7）：
   * 字段级 merge，缺失字段保留旧值（契约 td-config §2.1 UpdateAccount）。
   *
   * 网关不发（body 不得含 account_id；dzweb 按注册表从路径解析网关）——但 pending key 需要网关，
   * 因此从注册表/配置镜像解析；解析不到说明账户已不在平台视角内，直接按 account_not_found 提示，
   * 不发一个必然 404 的请求。
   */
  async function updateAccount(accountId: string, patch: TdUpdateAccountBody): Promise<TdAccountSubmitResult> {
    const gateway = gatewayOf(accountId)
    if (gateway === null) {
      const message = TD_ACCOUNT_ERROR_TEXT.account_not_found
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return submit(opKey(gateway, accountId, 'update'), OP_LABELS.update, () =>
      tdApi.updateAccount(accountId, patch))
  }

  /**
   * 删除账户（`DELETE /api/td/accounts/{account_id}`，契约 rest §2.9.7）。
   * 不删交易数据（ADR 0045）；pending 由该网关的 `td_rtn_config` 到达清除（不是「账户消失」判定）。
   */
  async function removeAccount(accountId: string): Promise<TdAccountSubmitResult> {
    const gateway = gatewayOf(accountId)
    if (gateway === null) {
      const message = TD_ACCOUNT_ERROR_TEXT.account_not_found
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return submit(opKey(gateway, accountId, 'remove'), OP_LABELS.remove, () =>
      tdApi.removeAccount(accountId))
  }

  /**
   * 启用/禁用账户。
   *
   * REST 契约把启停归入 `PUT /api/td/accounts/{id}` 的 `UpdateAccount`（契约 rest §2.9.7：
   * body 是 UpdateAccount 的 params 子集），因此走 UpdateAccount 而非 td-config §2.1 的独立
   * `SetAccountEnabled` op。**行为差异**：UpdateAccount 有状态保护（账户有活跃会话时 dzweb 转发
   * 的 op 会被网关拒绝并回滚），所以会话活跃时启停可能失败——这是契约现状，不是前端缺陷；
   * 失败经 NOTIFY_UI 弹窗 + 帧回滚旧值表达（前端镜像随帧回弹）。
   */
  async function setAccountEnabled(accountId: string, enabled: boolean): Promise<TdAccountSubmitResult> {
    const gateway = gatewayOf(accountId)
    if (gateway === null) {
      const message = TD_ACCOUNT_ERROR_TEXT.account_not_found
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return submit(opKey(gateway, accountId, 'toggle_enabled'), OP_LABELS.toggle_enabled, () =>
      tdApi.updateAccount(accountId, { enabled }))
  }

  // === WS 镜像 apply（快照分发与增量共用，幂等覆盖）===

  /**
   * `td_rtn_config`（契约 td-config §4 / 契约 webui-ws §2.2）：该网关配置**整体覆盖**，
   * 并清除该网关下全部 pending（key 前缀 `td:{gateway}:`）。
   *
   * `gateway` 取 WS 消息的 instance_id（契约 webui-ws §2.2：镜像 domain `td_config` 的 key =
   * 网关进程名）。缺失时无从定位镜像，整帧忽略（非法帧不得覆盖已有镜像，对齐 applyMdConfig）。
   */
  function applyTdConfig(gateway: string | undefined, payload: TdConfigPayload | undefined): void {
    if (!gateway || !payload || typeof payload !== 'object') return
    if (!Array.isArray(payload.accounts)) return
    configs.value[gateway] = payload
    // 清除粒度 = 该网关全部 pending：配置帧整网关全量，任何一次写帧都先于/等于生效信号
    //（契约 td-config §4 末条），因此一个网关的帧就是该网关所有在途写操作的收敛点。
    tdAccountsPending.clearByPrefix(`td:${gateway}:`)
  }

  /**
   * `td_rtn_status`（契约 td-status §3 / 契约 webui-ws §2.2）：按 `网关名:账户ID` 全量覆盖。
   * 缺 instance_id 或字段非法的帧忽略；**不做字段清空推断**（契约 td-status §2.4/§3：
   * 清空时机由发送方决定，前端以字段值变化为准）。
   */
  function applyTdStatus(instanceId: string | undefined, payload: TdStatusPayload | undefined): void {
    if (!instanceId || instanceId.indexOf(':') <= 0) return
    if (!payload || typeof payload !== 'object') return
    if (typeof payload.account_id !== 'string' || payload.account_id === '') return
    // state 是契约 td-status §2.2 的 11 态枚举名；运行时不可信，非字符串/空串的帧丢弃
    //（类型声明已收窄为 TdState，因此先转 unknown 再判，避免与类型系统打架）
    const rawState: unknown = payload.state
    if (typeof rawState !== 'string' || rawState === '') return
    statuses.value[instanceId] = payload
  }

  // === 注册表加载 ===

  /**
   * 拉账户注册表（`GET /api/td/accounts`，契约 rest §2.9.9）。
   *
   * 响应判别：
   * - `available:true` → 整体替换条目（注册表是投影，覆盖天然含删除）；
   * - `available:false` → 进降级态（**保留已有条目**，不清空：旧投影比假空态安全），排自愈重试；
   * - HTTP 失败 → 记 error + 排重试，同样保留已有条目。
   */
  async function loadRegistry(): Promise<void> {
    if (disposed) return
    const seq = ++requestSeq
    loading.value = true
    try {
      const response = await tdApi.listAccounts()
      if (disposed || seq !== requestSeq) return
      if (response.available === false) {
        available.value = false
        degradedReason.value = response.reason
        scheduleRetry()
        return
      }
      available.value = true
      degradedReason.value = null
      error.value = null
      clearRetry()
      registry.value = [...response.accounts].sort((a, b) =>
        a.account_id === b.account_id ? 0 : (a.account_id < b.account_id ? -1 : 1))
    } catch (e) {
      if (disposed || seq !== requestSeq) return
      // HTTP 失败不是注册表降级（降级有独立的 available:false 语义）：记录 error、保留
      // 上一次的 available 与条目，并按同一 5s 间隔重试（dzweb 重启/网络抖动后自愈）。
      error.value = e instanceof Error ? e.message : String(e)
      scheduleRetry()
    } finally {
      if (seq === requestSeq) {
        loading.value = false
        initialized.value = true
      }
    }
  }

  /**
   * 页面挂载：重拉注册表（store 是单例，离开期间 `data_changed` 可能已被漏掉，旧投影不可信）。
   * 同时解除 dispose 并复位重试定时器（与 td 列表 store 的 init/dispose 同型）。
   */
  async function init(): Promise<void> {
    disposed = false
    clearRetry()
    await loadRegistry()
  }

  /** `data_changed{scope:"td"}`（契约 webui-ws §2.3：账户注册表集合增减由它广播）→ 重拉注册表 */
  async function handleDataChanged(scope?: string): Promise<void> {
    if (scope !== 'td') return
    if (disposed) return
    await loadRegistry()
  }

  /** 页面卸载：停自愈重试与在途响应写入（store 生命周期不终结，重新挂载由 init 恢复） */
  function dispose(): void {
    disposed = true
    clearRetry()
  }

  return {
    registry,
    available,
    degradedReason,
    loading,
    initialized,
    error,
    configs,
    statuses,
    rows,
    isEmpty,
    degradedText,
    gatewayNames,
    gatewayOptions,
    addPending,
    accountIdConflict,
    registryEntry,
    gatewayOf,
    accountPending,
    isOpPending,
    addAccount,
    updateAccount,
    removeAccount,
    setAccountEnabled,
    applyTdConfig,
    applyTdStatus,
    loadRegistry,
    init,
    handleDataChanged,
    dispose,
    isGatewayRunning,
    clearError(): void { error.value = null },
  }
})
