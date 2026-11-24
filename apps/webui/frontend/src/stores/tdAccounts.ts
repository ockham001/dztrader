import { defineStore } from 'pinia'
import { computed, ref, watch } from 'vue'
import { tdApi } from '@/api/td'
import {
  TD_GATEWAY_NETWORK_ERROR_TEXT,
  tdGatewayErrorText,
  tdGatewaysApi,
  tdGatewayServerErrorText,
  type TdGatewayAvailable,
} from '@/api/tdGateways'
import { usePending, PENDING_TIMEOUT } from '@/composables/usePending'
import { sessionPendingClear, sessionStateOfProgress } from '@/composables/sessionProgress'
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
  ProcessState,
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
 * - **细状态 = 状态镜像**（WS `td_rtn_status`，契约 td-status §3）：按 `instance_id` = **账户 ID**
 *   全量覆盖。登录态/交易日/登录时间只从它取，**不从 `td_rtn_config` 推导**（契约 rest §4 前端义务）。
 * - **展示名 = 解析层**（ADR 0056 决策 9、契约 td-config §8）：`account_id → account_name`
 *   由注册表 + 配置镜像合并而来（{@link displayName}），所有展示位（账户表、委托/成交/持仓/
 *   资金、筛选器、toast/aria）在渲染时按 id 现查，**不缓存名字副本**——改名经
 *   `td_rtn_config` 到达即全站生效（含历史行）；解析不到回落 `account_id`。
 *   名字是纯展示字段，**不用于任何路由或过滤**（过滤参数始终是 `account_id`）。
 *
 * 为什么进度不取 `td_rtn_status` 的 `progress_*`：契约 td-status §2.1 明示前端进度展示以
 * 契约 progress 为**唯一入口**（`td_rtn_status` 携带进度是历史兼容）。这里读 `progress` 镜像，
 * key = 账户 ID（ADR 0056 决策 7：账户级帧 `instance_id` 就是账户 ID，不再是复合键）。
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
 * **交易接口卡片（ADR 0053 / #439）**：`interfaces` 把进程镜像里的 `dztd_*` 条目与账户行按
 * `gateway` 组合成页面渲染单元；卡片本身没有 REST 列表（契约 rest §4）。
 *
 * **接口生命周期（契约 rest §2.10）**：`addGateway` / `startGateway` / `removeGateway` 写
 * `/api/td/gateways/*`，pending key = `tdgw:{gateway}:{op}`，清除信号是 **进程镜像**
 * （`process_config` 条目出现/消失、`process_status` 到 Running / 操作 event）——与账户写
 * pending（`td:`，由 `td_rtn_config` 清）和会话 pending（`td_sess:`，由 `progress` 清）分离。
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
 * pending key 前缀（`td:{gateway}:{account}:{op}`）。三处使用点——`opKey()` 构造、
 * `addPendingFor()` 的前缀扫描、`applyTdConfig` 的整网关清除——必须共用本常量：
 * 各写一份字面量时，改前缀会静默漏改其中一处（例如清除用了新前缀而构造还在用旧前缀，
 * pending 就永远清不掉、只能等 5s 超时）。
 *
 * 为什么不用 mdConfig 的 `CLEAR_BY_RTN` 声明表：那张表按「哪个 op 由哪类 RTN 清除」建模，
 * 粒度是 op；本域的语义是**整网关一次清除**（配置帧恒为该网关全量，一个帧收口该网关全部
 * 在途写操作，契约 td-config §4 末条），按 op 建表会表达不出「任意 op 都被同一个帧清掉」。
 */
export const TD_PENDING_PREFIX = 'td:'

/**
 * 会话控制 pending 的独立前缀（契约 td-connection）：`td_sess:{gateway}:{account}:{login|logout}`。
 *
 * **必须与配置写 pending 分开**：配置帧到达时按 `td:{gateway}:` 前缀整组清在途写操作；
 * 若会话 pending 共用同一前缀，一次无关的配置回报就会把登录/登出 pending 误清。会话 pending
 * 的唯一清除来源是进度端点（登录 → max、登出 → min）、`current` 回退（失败/中止）与兜底超时。
 */
export const TD_SESSION_PENDING_PREFIX = 'td_sess:'

/**
 * 本领域独立 pending 实例（账户配置页无跨 store pending 交互，与 settings/sysmon 同型）。
 * 模块级单例导出供 store 与测试共享同一实例（可重置/可断言）。
 */
export const tdAccountsPending = usePending()

/**
 * 账户配置写操作（pending key 的 op 段，契约 rest §2.9.7）。
 * 会话控制（login/logout，契约 td-connection）是另一类 op——key 前缀不同、清除来源不同。
 */
export type TdConfigOp = 'add' | 'update' | 'remove' | 'toggle_enabled'

/** 账户会话控制操作（契约 td-connection：登录/登出） */
export type TdSessionOp = 'login' | 'logout'

export type TdAccountOp = TdConfigOp | TdSessionOp

/** 配置写操作全集（行级 spinner 判定只扫这一组，不扫会话 pending） */
const CONFIG_OPS: readonly TdConfigOp[] = ['add', 'update', 'remove', 'toggle_enabled']

/** 会话控制 pending 兜底超时（契约 td-connection 前端义务）：登录 45s（连接 30s + 登录 10s + 余量） */
export const TD_SESSION_LOGIN_TIMEOUT_MS = 45_000
/** 登出 pending 兜底超时：释放 API 属本地动作，10s 足够 */
export const TD_SESSION_LOGOUT_TIMEOUT_MS = 10_000

/** 各操作的可读超时文案（usePending opLabel，超时 toast 用「{opLabel}超时」） */
const OP_LABELS: Record<TdAccountOp, string> = {
  add: '添加交易账户',
  update: '修改交易账户',
  remove: '删除交易账户',
  toggle_enabled: '启停交易账户',
  login: '登录交易账户',
  logout: '登出交易账户',
}

/**
 * 写操作结果：
 * - `ok:true` = HTTP 已受理（pending 保持挂起，等对应 WS 生效信号清）；
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
  return `账户 ID「${conflict.account_id}」已被交易接口 ${conflict.gateway} 占用（含已禁用/离线账户），请换一个账户 ID`
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
  '账户注册表暂不可用（尚未收到任何交易接口配置），列表可能不完整，正在自动重试'

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
  /** 进度（契约 progress；账户级 `instance_id` = 账户 ID）；未到达为 null */
  progress: ProgressView | null
  /** 网关进程是否 Running（进程镜像）；镜像未到达为 false（页面据此提示先启动网关） */
  gatewayRunning: boolean
}

/**
 * 交易接口卡片（页面渲染单元，ADR 0053）：进程镜像里每个 `dztd_*` 条目 = 一张卡片，
 * 卡片内账户 = {@link TdAccountRow} 按 `gateway` 分组的子集。
 *
 * 为什么不新增列表端点（契约 rest §4）：接口身份 = 进程名，进程配置/状态镜像已是权威
 * 来源；REST 只用于生命周期写操作（§2.10）。
 */
export interface TdInterfaceView {
  /** 进程名（= exe stem = 接口身份，不可改名） */
  name: string
  /** 显示名：进程配置 `display_name` 优先，状态帧值次之，最后回退进程名（契约 process） */
  displayName: string
  /** 进程状态（契约 process ChildState：Starting/Running/Stopping/Stopped/Crashed）；无镜像为 null */
  state: ProcessState | null
  /** 进程是否 Running；账户写操作的门槛（后端 §2.9.7 预检 3 兜底） */
  running: boolean
  /** 归本接口的账户行（按 account_id 升序，继承 rows 的排序） */
  accounts: TdAccountRow[]
  accountCount: number
  /** 已启用账户数（卡片头聚合） */
  enabledCount: number
  /** 就绪（细状态 Ready）账户数（卡片头聚合） */
  readyCount: number
}

/** 交易接口生命周期操作（契约 rest §2.10；UI 只用 start / remove，stop 为 API 完整面） */
export type TdGatewayOp = 'start' | 'remove'

/**
 * 接口生命周期 pending 的 key 前缀（`tdgw:{name}:{op}`）。
 * 与账户写 pending（`td:`）和会话 pending（`td_sess:`）物理隔离：三者清除信号不同
 *（接口 → process_config/process_status；账户写 → td_rtn_config；会话 → progress）。
 */
export const TD_GATEWAY_PENDING_PREFIX = 'tdgw:'

/** 接口启动/删除 pending 兜底超时（进程操作口径与行情源一致：30s） */
export const TD_GATEWAY_OP_TIMEOUT_MS = 30_000

/**
 * 接口生命周期 pending 实例。模块级单例导出供 store 与测试共享同一实例
 *（与 `tdAccountsPending` 同型；测试可重置/断言）。
 */
export const tdGatewayPending = usePending()

/** 接口生命周期写操作结果（语义对齐 TdAccountSubmitResult，只少 conflict 态） */
export type TdGatewaySubmitResult =
  | { ok: true }
  | { ok: false; kind: 'failed'; message: string }
  | { ok: false; kind: 'busy' }
  | { ok: false; kind: 'timeout' }

export const useTdAccountsStore = defineStore('tdAccounts', () => {
  // === 账户注册表（REST 基线，契约 rest §2.9.9）===
  /** 注册表条目（跨网关全集，按 account_id 升序） */
  const registry = ref<TdConfigAccountList[]>([])
  /** 注册表可用性；false = 降级（契约 rest §2.9.9，**不是**空列表） */
  const available = ref(true)
  const degradedReason = ref<TdAccountsDegradedReason | null>(null)
  const initialized = ref(false)
  const error = ref<string | null>(null)

  // === WS 镜像 ===
  /** 交易网关配置镜像（key = 网关进程名；契约 td-config §4 全量覆盖） */
  const configs = ref<Record<string, TdConfigPayload>>({})
  /** 账户级细状态镜像（key = 账户 ID = 帧头 instance_id；契约 td-status §3 全量覆盖） */
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

  // === 展示名解析层（ADR 0056 决策 9；契约 td-config §8；契约 rest §2.9.9）===

  /**
   * `account_id → account_name` 解析源（纯展示，**不参与任何路由/过滤**，契约 td-config §8）。
   *
   * 两个来源、合并顺序有讲究：
   * 1. **注册表**（REST `GET /api/td/accounts`）：跨网关全集，也是账户被删除后仍可查的兜底
   *    （历史委托/成交行按 id 解析，账户已删则解析不到 → 回落 id，ADR 0056 Consequences）；
   * 2. **配置镜像**（WS `td_rtn_config`）**覆盖**注册表：改名只经该帧到达，dzweb 的注册表
   *    重拉要等 `data_changed{scope:"td"}`（账户集合**增减**才广播，改名不触发），因此
   *    以前者为准才能做到「改名立刻全站生效（含历史行）」。
   *
   * 覆盖是**整值覆盖**（含空串）：配置帧是整网关全量真相，帧里名字为空 = 清空名字，
   * 必须把注册表里的旧名一并覆盖掉——只写非空值会让「清空」变成「沿用旧名」。
   * 空串 = 未设置（ADR 0056 决策 3「不做写入时规范化」），由 {@link displayName} 回落 id。
   */
  const accountNames = computed<Map<string, string>>(() => {
    const names = new Map<string, string>()
    for (const entry of registry.value) {
      if (typeof entry.account_name === 'string') {
        names.set(entry.account_id, entry.account_name)
      }
    }
    for (const config of Object.values(configs.value)) {
      for (const account of config.accounts ?? []) {
        if (!account || typeof account.account_id !== 'string') continue
        // 缺字段与空串同义（未设置）：配置帧是该网关的全量真相，必须覆盖注册表旧值
        names.set(
          account.account_id,
          typeof account.account_name === 'string' ? account.account_name : '',
        )
      }
    }
    return names
  })

  /**
   * 账户展示名原值（编辑表单回填用；未设置 = 空串，**不**回落 `account_id`）。
   * 表单必须能区分「没起名字」与「名字恰好等于 id」，否则一次无意保存会把后者抹掉。
   */
  function accountName(accountId: string): string {
    return accountNames.value.get(accountId) ?? ''
  }

  /**
   * 账户展示名（所有展示位的唯一入口，契约 rest §2.9.9「账户显示名的唯一来源」+ 配置镜像）：
   * 有名字显示名字（去首尾空白；全空白按未设置处理），否则回落 `account_id`。
   *
   * 为什么在渲染时按 id 解析，而不是把名字写进行/事件里：历史行（委托/成交/持仓/资金）来自
   * REST 基线或交易库，改名后不会重推；只有「按 id 现查」才能让它们立刻显示新名字
   * （ADR 0056 决策 9「改名立刻全站生效（含历史行）」）。**任何行都不得缓存名字副本**。
   */
  function displayName(accountId: string): string {
    const name = accountName(accountId).trim()
    return name === '' ? accountId : name
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

    // 3) 状态镜像（key = 账户 ID）+ 进度镜像（同 key，契约 progress 为进度唯一入口）：
    //    账户级帧的 instance_id 就是账户 ID（ADR 0056 决策 7），直接按 id 关联行，无需拆键。
    //    进度镜像里同时有**进程级**条目（key = 进程名，如 dztd_ctp / dzmd_ctp / dztraderd）：
    //    它们不在账户骨架里，`merged.get` 查不到自然跳过，不会串到账户行上。
    for (const [accountId, status] of Object.entries(statuses.value)) {
      const row = merged.get(accountId)
      if (!row) continue  // 状态镜像里的账户不在注册表/配置镜像中：等配置帧到达，不自造行
      row.status = status
    }
    for (const [accountId, progress] of Object.entries(progressStore.progress)) {
      const row = merged.get(accountId)
      if (!row) continue
      row.progress = progress
    }

    // 注册表已按 account_id 升序返回；配置镜像骨架的降级态也按同一口径排序，避免顺序抖动
    return [...merged.values()].sort((a, b) =>
      a.account_id === b.account_id ? 0 : (a.account_id < b.account_id ? -1 : 1))
  })

  /** 降级提示文案（可用时为空串） */
  const degradedText = computed<string>(() => (available.value ? '' : TD_ACCOUNTS_DEGRADED_TEXT))

  /**
   * 接口显示名（契约 process）：**配置镜像为唯一真相源**（`display_name` 可被 Start / SET_PROCESS_CONFIG
   * 覆盖）；`process_status` 的 `display_name` 只作顺序竞态窗口的兜底展示，不回写配置镜像。
   */
  function gatewayDisplayName(name: string): string {
    const configName = processStore.configs[name]?.['display_name']
    if (typeof configName === 'string' && configName !== '') return configName
    const statusName = processStore.statuses[name]?.display_name
    if (typeof statusName === 'string' && statusName !== '') return statusName
    return name
  }

  /**
   * 交易接口卡片列表（ADR 0053 / 契约 rest §4）：来自 WS `process_config` + `process_status`
   * 的 `dztd_*` 条目，按进程名升序；卡片内账户 = rows 按 gateway 分组。
   *
   * 取两个镜像的**并集**：两者由不同帧独立推送，启动/重连窗口内可能只到达其一
   *（process_status 先于 process_config 是契约 process 允许的时序）。条目消失由 config 帧
   * 全量覆盖清理，status 镜像随之被 process store 清理，不会留下幽灵卡片。
   */
  const interfaces = computed<TdInterfaceView[]>(() => {
    const names = new Set<string>()
    for (const name of Object.keys(processStore.configs)) {
      if (name.startsWith('dztd_')) names.add(name)
    }
    for (const name of Object.keys(processStore.statuses)) {
      if (name.startsWith('dztd_')) names.add(name)
    }
    const grouped = new Map<string, TdAccountRow[]>()
    for (const row of rows.value) {
      const list = grouped.get(row.gateway)
      if (list) list.push(row)
      else grouped.set(row.gateway, [row])
    }
    return [...names].sort().map(name => {
      const accounts = grouped.get(name) ?? []
      const state = processStore.statuses[name]?.state ?? null
      return {
        name,
        displayName: gatewayDisplayName(name),
        state,
        running: state === 'Running',
        accounts,
        accountCount: accounts.length,
        enabledCount: accounts.filter(row => row.enabled).length,
        readyCount: accounts.filter(row => row.status?.state === 'Ready').length,
      }
    })
  })

  // === pending 查询（页面用） ===

  function opKey(gateway: string, accountId: string, op: TdAccountOp): string {
    const prefix = op === 'login' || op === 'logout' ? TD_SESSION_PENDING_PREFIX : TD_PENDING_PREFIX
    return `${prefix}${gateway}:${accountId}:${op}`
  }

  /** 该账户是否有任一**配置写**在途操作（行级 spinner；会话控制在按钮上单独展示） */
  function accountPending(accountId: string): boolean {
    const gateway = gatewayOf(accountId)
    if (gateway === null) return false
    return CONFIG_OPS.some(op => tdAccountsPending.pending[opKey(gateway, accountId, op)] === true)
  }

  /** 该账户的会话控制在途态（登录/登出按钮 spinner） */
  function sessionPending(accountId: string, op: TdSessionOp): boolean {
    const gateway = gatewayOf(accountId)
    if (gateway === null) return false
    return tdAccountsPending.pending[opKey(gateway, accountId, op)] === true
  }

  /** 账户在指定网关下是否有在途的某类操作 */
  function isOpPending(gateway: string, accountId: string, op: TdAccountOp): boolean {
    return tdAccountsPending.pending[opKey(gateway, accountId, op)] === true
  }

  /**
   * 指定交易接口的「添加账户」在途态（新账户尚未进注册表，不能用 accountPending 判定）。
   *
   * **按网关隔离**：key 前缀 = `td:{gateway}:`，A 卡添加中不应禁用 B 卡的添加入口
   *（多张卡片同页，全局 addPending 会把所有卡片一起锁住）。
   */
  function addPendingFor(gateway: string): boolean {
    const prefix = `${TD_PENDING_PREFIX}${gateway}:`
    return Object.keys(tdAccountsPending.pending).some(key =>
      key.startsWith(prefix) && key.endsWith(':add') && tdAccountsPending.pending[key] === true)
  }

  // === 会话 pending 清除（契约 td-connection 前端义务 2 / 契约 progress 网关会话进度骨架） ===

  /** 账户 ID（账户级 progress 的 instance_id）→ 上一次 `current`：判定"进度回退 = 一次尝试失败/中止" */
  const lastSessionCurrent = new Map<string, number>()

  function clearSessionPending(gateway: string, accountId: string, op: TdSessionOp): void {
    tdAccountsPending.clearByPrefix(opKey(gateway, accountId, op))
  }

  // 清除信号（唯一来源是进度端点/回退；NOTIFY_UI 是纯通知，永不清 pending，契约 notify-ui）：
  //   - 到 max → 清登录 pending（登录完成）；
  //   - 到 min → 清登录 + 登出 pending（登出完成；或登录在途时被登出/中止，避免挂到 45s 超时）；
  //   - 中间态且 current 相对上次回退 → 清登录 pending（失败回滚，进度骨架声明建立期单调）；
  //   - 兜底超时（登录 45s / 登出 10s，见 TD_SESSION_*_TIMEOUT_MS）。
  watch(
    () => progressStore.progress,
    (progress) => {
      for (const [instanceId, p] of Object.entries(progress)) {
        // 账户级 progress 的 instance_id = 账户 ID（ADR 0056 决策 7）；先解析归属网关，
        // 解不出该账户的条目是**进程级**进度（进程名，如 dztd_ctp），不参与会话 pending 清除。
        const gateway = gatewayOf(instanceId)
        if (gateway === null) continue
        const state = sessionStateOfProgress(p)
        if (state === null) continue
        const previous = lastSessionCurrent.get(instanceId)
        lastSessionCurrent.set(instanceId, p.current)
        // 清除范围判定与 md 共用 sessionProgress（ready→login、absent→both、回退→login）
        const clear = sessionPendingClear(previous, state, p.current)
        if (clear === 'login') {
          clearSessionPending(gateway, instanceId, 'login')
        } else if (clear === 'both') {
          clearSessionPending(gateway, instanceId, 'login')
          clearSessionPending(gateway, instanceId, 'logout')
        }
      }
    },
    { deep: true },
  )

  // === 交易接口生命周期（契约 rest §2.10 / ADR 0053） ===
  //
  // 页面级「添加交易接口」与卡片级「启动进程 / 删除交易接口」共用本段。pending 语义：
  // HTTP 200 只表示已写入事件通道（契约 rest §2.10），清除信号是 WS 进程镜像——
  //   - start：`process_config` 出现该条目，或 `process_status` 到 Running；
  //   - remove：`process_config` 条目消失（移除完成的权威信号，契约 process），或 RemoveSucceeded。
  // 失败（StartFailed / RemoveFailed）静默清 pending：失败反馈由 NOTIFY_UI 弹窗承载
  //（与 process store 的进程操作同一口径，避免双提示）。
  //
  // **注意未注册的 start（添加新接口）**：process store 在 `configsInitialized` 后过滤
  // 不在配置镜像里的进程帧（`applyProcessStatus` 对未注册进程不写镜像），因此新接口的
  // StartFailed 不会进入本 store 的 status 镜像——该路径没有失败信号，只能等
  // {@link TD_GATEWAY_OP_TIMEOUT_MS} 兜底超时（`usePending` 会 toast「启动交易接口超时」），
  // 服务端的失败原因由 NOTIFY_UI 弹窗承载。start 的**恢复**（已注册接口）以 status Running 为主。

  /** 可用交易接口列表（`GET /api/td/gateways/available`；添加下拉数据源） */
  const gatewayAvailable = ref<TdGatewayAvailable[]>([])
  const gatewayAvailableLoading = ref(false)

  /** 从任意失败对象取出用户可读文案（不泄露英文 error / snake_case code） */
  function gatewayFailureMessage(failure: unknown): string {
    const code = tdAccountErrorCodeOf(failure)
    if (code !== undefined) return tdGatewayErrorText(code)
    const status = tdAccountErrorStatusOf(failure)
    if (status !== undefined) return tdGatewayServerErrorText(status)
    return TD_GATEWAY_NETWORK_ERROR_TEXT
  }

  /**
   * 拉可用交易接口列表（页面下拉打开时刷新）。
   * 失败写 `error`（页面统一 toast + 清空），不清空已有列表（旧列表比假空态安全）。
   */
  async function refreshAvailableGateways(): Promise<void> {
    gatewayAvailableLoading.value = true
    try {
      gatewayAvailable.value = await tdGatewaysApi.listAvailable()
    } catch (e) {
      error.value = gatewayFailureMessage(e)
    } finally {
      gatewayAvailableLoading.value = false
    }
  }

  function gatewayOpKey(name: string, op: TdGatewayOp): string {
    return `${TD_GATEWAY_PENDING_PREFIX}${name}:${op}`
  }

  /** 该接口是否有在途「启动/添加」操作（卡片「启动进程」与页面添加入口用） */
  function gatewayStartPending(name: string): boolean {
    return tdGatewayPending.pending[gatewayOpKey(name, 'start')] === true
  }

  /** 该接口是否有在途「删除」操作（卡片删除按钮用） */
  function gatewayRemovePending(name: string): boolean {
    return tdGatewayPending.pending[gatewayOpKey(name, 'remove')] === true
  }

  /**
   * 提交一个接口生命周期操作：pending 保持到进程镜像确认（见本段头注释），HTTP 失败按 code 映射文案。
   * `usePending.run` 会吞异常，因此错误先在 `fn` 内捕获进闭包（与账户写 `submit` 同一理由）。
   */
  async function submitGateway(
    name: string,
    op: TdGatewayOp,
    request: () => Promise<unknown>,
  ): Promise<TdGatewaySubmitResult> {
    const key = gatewayOpKey(name, op)
    if (tdGatewayPending.pending[key] === true) return { ok: false, kind: 'busy' }
    let failure: unknown = null
    let failed = false
    const result = await tdGatewayPending.run(key, async () => {
      try {
        return await request()
      } catch (e) {
        failure = e
        failed = true
        throw e
      }
    }, {
      timeout: TD_GATEWAY_OP_TIMEOUT_MS,
      opLabel: op === 'start' ? '启动交易接口' : '删除交易接口',
      distinguishTimeout: true,
    })
    if (result === PENDING_TIMEOUT) return { ok: false, kind: 'timeout' }
    if (failed) return { ok: false, kind: 'failed', message: gatewayFailureMessage(failure) }
    return { ok: true }
  }

  /**
   * 添加交易接口（页面级唯一入口）：动态注册 + 启动（契约 rest §2.10 `start` 端点）。
   * `name` 必须匹配 `dztd_*`（后端 400 `invalid_gateway` 兜底，这里只挡明显写错的输入）。
   */
  async function addGateway(name: string, displayName?: string): Promise<TdGatewaySubmitResult> {
    if (!name.startsWith('dztd_')) {
      return { ok: false, kind: 'failed', message: tdGatewayErrorText('invalid_gateway') }
    }
    const trimmed = displayName?.trim() ?? ''
    return submitGateway(name, 'start', () =>
      tdGatewaysApi.start(name, trimmed === '' ? undefined : { display_name: trimmed }))
  }

  /** 恢复非 Running 接口（崩溃重启用；ADR 0053 明确不得以删除重加代替恢复） */
  async function startGateway(name: string): Promise<TdGatewaySubmitResult> {
    return submitGateway(name, 'start', () => tdGatewaysApi.start(name))
  }

  /**
   * 移除交易接口（停止进程 + 删除进程配置条目）。前端只在卡片无账户时提供入口，
   * 权威 guard 在服务端 409 `gateway_has_accounts`（契约 rest §2.10 预检 5）。
   */
  async function removeGateway(name: string): Promise<TdGatewaySubmitResult> {
    return submitGateway(name, 'remove', () => tdGatewaysApi.remove(name))
  }

  // 清除信号 1：process_status —— start 到 Running 清；StartFailed/Remove 结果清并静默失败
  watch(
    () => processStore.statuses,
    (statuses) => {
      for (const [name, status] of Object.entries(statuses)) {
        if (!name.startsWith('dztd_')) continue
        const startKey = gatewayOpKey(name, 'start')
        if (tdGatewayPending.pending[startKey] === true) {
          if (status.state === 'Running') tdGatewayPending.resolve(startKey)
          else if (status.event === 'StartFailed') tdGatewayPending.fail(startKey)
        }
        const removeKey = gatewayOpKey(name, 'remove')
        if (tdGatewayPending.pending[removeKey] === true) {
          if (status.event === 'RemoveSucceeded') tdGatewayPending.resolve(removeKey)
          else if (status.event === 'RemoveFailed') tdGatewayPending.fail(removeKey)
        }
      }
    },
    { deep: true },
  )

  // 清除信号 2：process_config —— 只认**真实的状态迁移**，不认当前快照（权威信号，契约 process）：
  //   - start：仅 `未注册 → 已注册`（`!old.has → new.has`）清 pending——接口本就在配置里时
  //     任何无关配置帧都不该把在途 start 提前清掉（恢复路径以 status Running 为主）；
  //   - remove：仅 `已注册 → 未注册`（`old.has → !new.has`）清 pending——条目一直不存在的
  //     配置帧不能冒充「移除完成」。
  watch(
    () => processStore.configs,
    (processConfigs, previousConfigs) => {
      const has = (map: Record<string, unknown> | undefined, name: string): boolean =>
        map !== undefined && Object.prototype.hasOwnProperty.call(map, name)
      for (const key of Object.keys(tdGatewayPending.pending)) {
        if (tdGatewayPending.pending[key] !== true) continue
        const rest = key.slice(TD_GATEWAY_PENDING_PREFIX.length)
        const separator = rest.lastIndexOf(':')
        if (separator <= 0) continue
        const name = rest.slice(0, separator)
        const op = rest.slice(separator + 1)
        const wasPresent = has(previousConfigs, name)
        const isPresent = has(processConfigs, name)
        if (op === 'start' && !wasPresent && isPresent) tdGatewayPending.resolve(key)
        else if (op === 'remove' && wasPresent && !isPresent) tdGatewayPending.resolve(key)
      }
    },
    { deep: true },
  )

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
    timeoutMs: number = TD_ACCOUNT_OP_TIMEOUT_MS,
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
    }, { timeout: timeoutMs, opLabel, distinguishTimeout: true })
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

  // === 会话控制（契约 rest §2.9.10 / 契约 td-connection / ADR 0051） ===

  /**
   * 登录交易账户（`POST /api/td/accounts/{id}/login`）：建立会话并重走完整登录链。
   *
   * 按钮可用性由页面按进度端点判定（登录仅完全未登录可发，见 `composables/tdAccountSession`）；
   * 这里不做状态裁决——最终由 dzweb 守卫与目标网关配置校验兜底。pending 以 `progress`
   * 到 `max`、`current` 回退或 45s 超时清除（契约 td-connection 前端义务）。
   */
  async function login(accountId: string): Promise<TdAccountSubmitResult> {
    const gateway = gatewayOf(accountId)
    if (gateway === null) {
      const message = TD_ACCOUNT_ERROR_TEXT.account_not_found
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return submit(opKey(gateway, accountId, 'login'), OP_LABELS.login, () =>
      tdApi.loginAccount(accountId), TD_SESSION_LOGIN_TIMEOUT_MS)
  }

  /**
   * 登出交易账户（`POST /api/td/accounts/{id}/logout`）：任意非未登录状态可发（= 强制中断）。
   * pending 以 `progress` 到 `min` 或 10s 超时清除（登出不看 `current` 回退）。
   */
  async function logout(accountId: string): Promise<TdAccountSubmitResult> {
    const gateway = gatewayOf(accountId)
    if (gateway === null) {
      const message = TD_ACCOUNT_ERROR_TEXT.account_not_found
      error.value = message
      return { ok: false, kind: 'failed', message }
    }
    return submit(opKey(gateway, accountId, 'logout'), OP_LABELS.logout, () =>
      tdApi.logoutAccount(accountId), TD_SESSION_LOGOUT_TIMEOUT_MS)
  }

  // === WS 镜像 apply（快照分发与增量共用，幂等覆盖）===

  /**
   * `td_rtn_config`（契约 td-config §4 / 契约 webui-ws §2.2）：该网关配置**整体覆盖**，
   * 并清除该网关下全部**配置写** pending（key 前缀 `td:{gateway}:`）。
   *
   * 会话控制 pending 用独立前缀 `td_sess:`（契约 td-connection），**不受配置帧影响**——它的
   * 清除信号只有进度端点/回退/超时（见上方 pending 清除 watcher）。
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
    tdAccountsPending.clearByPrefix(`${TD_PENDING_PREFIX}${gateway}:`)
  }

  /**
   * `td_rtn_status`（契约 td-status §3 / 契约 webui-ws §2.2）：按 `instance_id` = **账户 ID**
   * 全量覆盖（ADR 0056 决策 7：账户级帧不再用 `网关名:账户ID` 复合键）。
   * 缺 instance_id、payload 自校验不一致或字段非法的帧忽略；**不做字段清空推断**
   *（契约 td-status §2.4/§3：清空时机由发送方决定，前端以字段值变化为准）。
   */
  function applyTdStatus(instanceId: string | undefined, payload: TdStatusPayload | undefined): void {
    if (!instanceId) return
    if (!payload || typeof payload !== 'object') return
    if (typeof payload.account_id !== 'string' || payload.account_id === '') return
    // payload 冗余携带 account_id 供消费方定位与自校验（契约 td-status §3）：与帧头不一致
    // 说明来源错乱，按非法帧丢弃，避免把 A 账户的状态挂到 B 账户行上。
    if (payload.account_id !== instanceId) return
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
      if (seq === requestSeq) initialized.value = true
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
    initialized,
    error,
    configs,
    statuses,
    rows,
    interfaces,
    degradedText,
    accountIdConflict,
    registryEntry,
    gatewayOf,
    // 展示名解析层（ADR 0056 决策 9）：所有展示位按 id 现查，不缓存名字副本
    accountName,
    displayName,
    accountPending,
    sessionPending,
    isOpPending,
    addPendingFor,
    addAccount,
    updateAccount,
    removeAccount,
    setAccountEnabled,
    login,
    logout,
    applyTdConfig,
    applyTdStatus,
    loadRegistry,
    init,
    handleDataChanged,
    dispose,
    isGatewayRunning,
    // 交易接口生命周期（契约 rest §2.10 / ADR 0053）
    gatewayAvailable,
    gatewayAvailableLoading,
    refreshAvailableGateways,
    gatewayStartPending,
    gatewayRemovePending,
    addGateway,
    startGateway,
    removeGateway,
    clearError(): void { error.value = null },
  }
})
