<script setup lang="ts">
/**
 * 交易接口卡片（#439；ADR 0053；契约 rest §2.10/§4；契约 process、td-config、td-status、
 * 契约 td-connection）。
 *
 * 一张卡片 = 一个交易接口进程（`dztd_*`）+ 其内部账户表：
 * - 头部：显示名 + 进程名 + 进程状态（色语义沿用 processNaming/useProcessState）+
 *   「启动进程」（仅非 Running 且 admin 可见，调 `start` 恢复；ADR 0053 禁止删除重加）；
 * - 体：账户表（账户 ID / 启用 / 连接状态含登录登出 / 交易日 / 登录时间 / 进度 / 操作）+
 *   卡片内「添加账户」；接口可零账户运行（空态提示）；
 * - 底部：「删除交易接口」整行 danger 按钮，二次确认（停止进程 + 移除启动配置 + 不可撤销）；
 *   卡片内仍有账户时前端禁用（title「先删除账户」），权威 guard 在服务端 409。
 *
 * 数据/写入全部经 tdAccounts store（单一写入点）：本组件不直接调 API，也不复制 store 的
 * 账户写/会话/pending 逻辑（那些语义已在 store 中由 #251/#409 固定）。
 */
import { computed, ref } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TdAccountFormModal from '@/components/trading/TdAccountFormModal.vue'
import type { TdAccountFormValue } from '@/components/trading/TdAccountFormModal.vue'
import {
  useTdAccountsStore,
  tdStateText,
  tdLastErrorText,
  type TdAccountRow,
  type TdLastErrorView,
  type TdInterfaceView,
} from '@/stores/tdAccounts'
import { canLoginTd, canLogoutTd } from '@/composables/tdAccountSession'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import { formatTdDate } from '@/composables/tdPositionView'
import { TD_UNAVAILABLE_TEXT } from '@/composables/tdOrderView'
import { processStateColor, processStateText } from '@/composables/useProcessState'
import type { TdUpdateAccountBody } from '@/types/api'

const props = defineProps<{
  /** 卡片数据（页面从 store.interfaces 派生后传入；卡片不自己推导接口列表） */
  tdInterface: TdInterfaceView
}>()

const store = useTdAccountsStore()
const auth = useAuthStore()
const toast = useToastStore()

/** 写操作权限：契约 rest §2.9.7 预检 1 / §2.10 预检 1——admin；非 admin 只读 */
const canWrite = computed(() => auth.isAdmin)
const READ_ONLY_HINT = '仅管理员可添加/修改/删除交易账户'
const GATEWAY_NOT_RUNNING_HINT = '交易接口未运行，请先点击「启动进程」恢复'
const REMOVING_HINT = '交易接口移除请求已下发，正在等待进程配置回报'

const running = computed(() => props.tdInterface.running)
const startPending = computed(() => store.gatewayStartPending(props.tdInterface.name))
const removePending = computed(() => store.gatewayRemovePending(props.tdInterface.name))
/** 本接口「添加账户」在途态（按网关隔离，见 store.addPendingFor） */
const addingAccount = computed(() => store.addPendingFor(props.tdInterface.name))
const stateText = computed(() => processStateText(props.tdInterface.state))
const stateColor = computed(() => processStateColor(props.tdInterface.state))

/**
 * 账户写操作被禁止的原因（空串 = 可写）。
 *
 * 非 Running 时禁用是契约义务（契约 rest §4）：后端 §2.9.7 预检 3 会 409
 * `gateway_not_running`，前端提前禁用并说明原因，避免用户发出必然失败的请求。
 */
const accountWriteBlockedText = computed<string>(() => {
  if (!canWrite.value) return READ_ONLY_HINT
  // 移除在途时接口即将消失：同页再下发 Start / 账户写会与 Remove 竞态（契约 rest §2.10 无此序）
  if (removePending.value) return REMOVING_HINT
  if (!running.value) return GATEWAY_NOT_RUNNING_HINT
  return ''
})

const canAddAccount = computed(
  () => accountWriteBlockedText.value === '' && !addingAccount.value)

/** 账户行内写按钮的 title：被禁用时给原因，可用时给动作说明 */
function accountActionTitle(actionText: string): string {
  return accountWriteBlockedText.value !== '' ? accountWriteBlockedText.value : actionText
}

// ===== 启动进程（崩溃/重启耗尽恢复；ADR 0053：不得以删除重加代替恢复） =====
const startProcessTitle = computed(() => {
  if (removePending.value) return REMOVING_HINT
  if (startPending.value) return '启动请求已下发，等待进程状态回报'
  return '启动该交易接口进程（崩溃重启耗尽或 spawn 失败后的恢复入口）'
})

async function startProcess(): Promise<void> {
  if (startPending.value || removePending.value) return
  const result = await store.startGateway(props.tdInterface.name)
  if (result.ok) {
    toast.success(`已提交启动请求：${props.tdInterface.name}`)
  } else if (result.kind === 'failed') {
    toast.error(result.message)
  }
  // timeout：usePending 已 toast，调用方不重复提示
}

// ===== 表单（添加 / 编辑共用；gateway 由本卡片固定传入）=====
const formOpen = ref(false)
const editingRow = ref<TdAccountRow | null>(null)
const formSubmitting = ref(false)
const formError = ref('')

/**
 * 编辑行的展示名**原值**（交给表单回填，不回落 `id`）：
 * 「未设置」与「名字恰好等于 id」是两件事（ADR 0056 决策 3「配置文件不存派生态」），
 * 回填回落后的值会让一次无意保存把前者变成后者。
 */
const editingAccountName = computed(() => (
  editingRow.value === null ? '' : store.accountName(editingRow.value.account_id)
))

function openAdd(): void {
  if (!canAddAccount.value) return
  editingRow.value = null
  formError.value = ''
  formOpen.value = true
}

function openEdit(row: TdAccountRow): void {
  if (accountWriteBlockedText.value !== '' || store.accountPending(row.account_id)) return
  editingRow.value = row
  formError.value = ''
  formOpen.value = true
}

function closeForm(): void {
  if (formSubmitting.value) return
  formOpen.value = false
  editingRow.value = null
  formError.value = ''
}

/**
 * 表单提交 → store 写操作（添加/编辑）。
 *
 * 网关由卡片固定：添加用 `props.tdInterface.name`，不信任表单里的 gateway 字段
 *（该字段只用于接口类型派生与展示）。
 */
async function submitForm(value: TdAccountFormValue): Promise<void> {
  formSubmitting.value = true
  formError.value = ''
  try {
    if (editingRow.value === null) {
      const result = await store.addAccount(props.tdInterface.name, {
        account_id: value.account_id,
        account_name: value.account_name,
        enabled: value.enabled,
        risk_control_enabled: value.risk_control_enabled,
        currency_id: value.currency_id,
        ext: {
          broker_id: value.broker_id,
          password: value.password,
          product_info: value.product_info,
          frontends: value.frontends,
          auth_code: value.auth_code,
          app_id: value.app_id,
          // flow_dir 不提交：缺失 = 网关自动构造（契约 td-config §2.3），用户不管理网关本地文件布局
        },
      })
      if (!result.ok) {
        if (result.kind === 'conflict' || result.kind === 'failed') formError.value = result.message
        if (result.kind === 'timeout') formError.value = '提交超时：交易接口未在预期时间内回报配置，请稍后刷新确认'
        return
      }
      formOpen.value = false
      toast.success('已提交添加请求，等待交易接口回报配置')
      return
    }

    const patch = buildUpdatePatch(value, editingRow.value)
    const result = await store.updateAccount(editingRow.value.account_id, patch)
    if (!result.ok) {
      if (result.kind === 'conflict' || result.kind === 'failed') formError.value = result.message
      if (result.kind === 'timeout') formError.value = '提交超时：交易接口未在预期时间内回报配置，请稍后刷新确认'
      return
    }
    formOpen.value = false
    toast.success('已提交修改请求，等待交易接口回报配置')
  } finally {
    formSubmitting.value = false
  }
}

/** 前置列表的可比签名（地址 trim、label 归一为空串、enabled 归一为布尔；顺序敏感——提供即整体替换） */
function frontendsSignature(
  frontends: { address: string; label?: string; enabled?: boolean }[],
): string {
  return JSON.stringify(frontends.map(fe => ({
    address: fe.address.trim(),
    label: (fe.label ?? '').trim(),
    enabled: fe.enabled !== false,
  })))
}

/**
 * 构造 `UpdateAccount` 的 params 子集（契约 td-config §2.1）：
 * 只包含**确实要改**的字段——空字符串表示「置空该字段」（如清空产品信息/认证码/账户名称），
 * 但密码例外（空 = 不修改，契约 §2.3）。`frontends` 提供即整体替换。
 *
 * **`ext` 只在连接参数确实变化时才下发**（关键）：网关的 `requires_idle()` 对
 * 「params 含非空 `ext`」的 UpdateAccount 要求账户 `Idle`（契约 td-config §2.1 状态保护、
 * `apps/ctp/td/td_config.h`），而 `account_name` 是纯展示字段、任何会话状态都可改
 * （ADR 0056 决策 3 / #470 AC4）。恒下发 `ext` 会让「在线改名」被网关以 `session_active`
 * 拒绝——这正是本票修复的缺陷；因此这里与 `editingRow.config.ext` 逐字段 diff，只提交
 * 真正变化的连接参数（再叠加字段级 merge：未提交的字段由网关保留旧值）。
 * 配置镜像缺失（降级态骨架行）时宁可不下发 ext：猜一组空值下发会覆盖真实连接参数。
 *
 * `account_name` 总随表单提交（改名走同一端点，不要求账户 `Idle`）。
 * `flow_dir` 不下发：网关自动构造（契约 td-config §2.3），避免误清网关的本地文件布局。
 */
function buildUpdatePatch(value: TdAccountFormValue, row: TdAccountRow): TdUpdateAccountBody {
  const original = row.config?.ext
  const extPatch: NonNullable<TdUpdateAccountBody['ext']> = {}
  if (value.broker_id !== (original?.broker_id ?? '')) extPatch.broker_id = value.broker_id
  if (value.product_info !== (original?.product_info ?? '')) {
    extPatch.product_info = value.product_info
  }
  if (value.auth_code !== (original?.auth_code ?? '')) extPatch.auth_code = value.auth_code
  if (value.app_id !== (original?.app_id ?? '')) extPatch.app_id = value.app_id
  if (frontendsSignature(value.frontends) !== frontendsSignature(original?.frontends ?? [])) {
    extPatch.frontends = value.frontends
  }
  // 密码留空或残留掩码 = 保留旧值，绝不提交（契约 td-config §2.3；避免把 **** 写成新密码）
  if (value.password !== '' && value.password !== '****') {
    extPatch.password = value.password
  }

  const patch: TdUpdateAccountBody = {
    // 账户名称是纯展示字段：显式空串 = 清空并回落 account_id（契约 td-config §2.1/§2.2）
    account_name: value.account_name,
    enabled: value.enabled,
    risk_control_enabled: value.risk_control_enabled,
    // currency_id 不下发：缺失=保留旧值（契约 td-config §2.1）。币种不做人工配置，
    // 后续由柜台回报推导（#446）；避免注册表骨架行编辑时把非 CNY 账户静默重置为 CNY。
  }
  // 只有连接参数真的变化才带 ext：纯改名/改启用的 patch 因此不触发网关的 Idle 状态保护
  if (Object.keys(extPatch).length > 0) patch.ext = extPatch
  return patch
}

// ===== 启停（契约 rest §2.9.7：走 UpdateAccount，body {enabled}）=====
async function toggleEnabled(row: TdAccountRow): Promise<void> {
  if (accountWriteBlockedText.value !== '' || store.accountPending(row.account_id)) return
  const result = await store.setAccountEnabled(row.account_id, !row.enabled)
  if (!result.ok && (result.kind === 'conflict' || result.kind === 'failed')) {
    toast.error(result.message)
  } else if (!result.ok && result.kind === 'timeout') {
    toast.warning('启停请求已下发，但交易接口未在预期时间内回报配置')
  } else if (result.ok) {
    toast.success(`已提交${row.enabled ? '禁用' : '启用'}请求，等待交易接口回报配置`)
  }
}

// ===== 删除账户二次确认（含影响范围，ADR 0045：不删交易数据）=====
// 与登录状态无关：任何会话状态都先确认（契约 rest §4）；网关侧 RemoveAccount 不受状态保护，
// 有活跃会话时由网关销毁会话并推 Offline（契约 td-config §2.1）。
const removingRow = ref<TdAccountRow | null>(null)
const removeSubmitting = ref(false)

function openRemove(row: TdAccountRow): void {
  if (accountWriteBlockedText.value !== '' || store.accountPending(row.account_id)) return
  removingRow.value = row
}

function closeRemove(): void {
  if (removeSubmitting.value) return
  removingRow.value = null
}

async function confirmRemove(): Promise<void> {
  const row = removingRow.value
  if (!row) return
  removeSubmitting.value = true
  try {
    const result = await store.removeAccount(row.account_id)
    if (result.ok) {
      removingRow.value = null
      toast.success(`已提交删除请求：${store.displayName(row.account_id)}`)
      return
    }
    if (result.kind === 'conflict' || result.kind === 'failed') {
      toast.error(result.message)
      removingRow.value = null
    } else if (result.kind === 'timeout') {
      toast.warning('删除请求已下发，但交易接口未在预期时间内回报配置')
      removingRow.value = null
    }
  } finally {
    removeSubmitting.value = false
  }
}

// ===== 删除交易接口（底部整行 danger；二次确认）=====
const removeInterfaceOpen = ref(false)
const removeInterfaceSubmitting = ref(false)
const removeInterfaceError = ref('')

/** 删除接口被禁止的原因（空串 = 可删）：有账户 → 先删账户（ADR 0053 决策 3） */
const removeInterfaceBlockedText = computed<string>(() => (
  props.tdInterface.accountCount > 0 ? '先删除账户' : ''))

const removeInterfaceTitle = computed(() => {
  if (removeInterfaceBlockedText.value !== '') return removeInterfaceBlockedText.value
  if (removePending.value) return '移除请求已下发，等待进程配置回报'
  return '停止该交易接口进程并从 master 启动配置中移除（不可撤销）'
})

function openRemoveInterface(): void {
  if (removeInterfaceBlockedText.value !== '' || removePending.value) return
  removeInterfaceError.value = ''
  removeInterfaceOpen.value = true
}

function closeRemoveInterface(): void {
  if (removeInterfaceSubmitting.value) return
  removeInterfaceOpen.value = false
}

async function confirmRemoveInterface(): Promise<void> {
  if (removeInterfaceSubmitting.value) return
  removeInterfaceSubmitting.value = true
  removeInterfaceError.value = ''
  try {
    const result = await store.removeGateway(props.tdInterface.name)
    if (result.ok) {
      removeInterfaceOpen.value = false
      toast.success(`已提交移除请求：${props.tdInterface.name}`)
      return
    }
    if (result.kind === 'failed') {
      // 服务端 guard（409 gateway_has_accounts）等失败：留在确认框内说明原因
      removeInterfaceError.value = result.message
      return
    }
    if (result.kind === 'busy') {
      removeInterfaceError.value = '该操作正在进行中，请稍候'
      return
    }
    if (result.kind === 'timeout') {
      removeInterfaceError.value = '移除请求已下发，但未在预期时间内收到进程配置回报，请稍后刷新确认'
    }
  } finally {
    removeInterfaceSubmitting.value = false
  }
}

// ===== 展示辅助（细状态/交易日/登录时间/进度；口径同 #251 页面）=====

/** 细状态文案（未上报 = 占位；契约 td-status §2.2 的 13 态映射） */
function statusText(row: TdAccountRow): string {
  return tdStateText(row.status?.state ?? null)
}

// 契约 td-status §2.5 前端义务③：卡片持久展示「上次失败：message + 时间」。
// timestamp 为 Unix 秒，转本地时间；message 空时 tdLastErrorText 按 code 映射中文。
// timestamp<=0（非法/缺失）不展示时间，避免渲染 1970 时间。
const LAST_ERROR_TIME_FORMAT = { hour12: false } as const

function lastErrorTime(e: TdLastErrorView): string {
  if (!(e.timestamp > 0)) return ''
  // 秒 → 毫秒；只取时分秒，与卡片内其它时间字段同粒度
  return new Date(e.timestamp * 1000).toLocaleTimeString('zh-CN', LAST_ERROR_TIME_FORMAT)
}

/** 完整一句「上次失败：<原因>（时间）」；时间非法时只给原因（契约 td-status §2.5） */
function lastErrorText(e: TdLastErrorView): string {
  const time = lastErrorTime(e)
  const text = `上次失败：${tdLastErrorText(e)}`
  return time ? `${text}（${time}）` : text
}

/** 可选 tooltip：CTP ErrorID 仅 *_rejected 非 0，其余不给 title（避免误导） */
function lastErrorTitle(e: TdLastErrorView): string | undefined {
  return e.errorId ? `CTP 错误码：${e.errorId}` : undefined
}

/** 状态样式：Ready 正常、Idle 中性、其余中间态提示（与进程状态色口径一致） */
function statusClass(row: TdAccountRow): string {
  const state = row.status?.state
  if (state === 'Ready') return 'td-card__state--ok'
  if (state === undefined || state === 'Idle') return 'td-card__state--idle'
  return 'td-card__state--busy'
}

// ===== 会话控制（ADR 0051 / 契约 td-connection / 契约 rest §2.9.10）=====
// 双按钮固定位置：登录仅完全未登录可用（`current <= min`），登出在任意非未登录态可用
// （连接中/登录中/重连中/失败态/就绪 = 强制中断）。判定在 composables/tdAccountSession。

/** 登录按钮可点：admin + 网关 Running + 账户启用 + 会话未登录 */
function canLoginRow(row: TdAccountRow): boolean {
  return auth.isAdmin && row.gatewayRunning && row.enabled && canLoginTd(row)
}

/** 登出按钮可点：admin + 网关 Running + 会话非未登录（账户禁用不影响登出） */
function canLogoutRow(row: TdAccountRow): boolean {
  return auth.isAdmin && row.gatewayRunning && canLogoutTd(row)
}

function loginDisabledTitle(row: TdAccountRow): string {
  if (!auth.isAdmin) return '仅管理员可登录/登出交易账户'
  if (!row.gatewayRunning) return GATEWAY_NOT_RUNNING_HINT
  if (!row.enabled) return '账户已禁用，不能登录'
  if (!canLoginTd(row)) return '当前不是未登录状态；如需重新登录请先登出（失败恢复 = 先登出再登录）'
  return '登录该交易账户（建立会话并重走完整登录链）'
}

function logoutDisabledTitle(row: TdAccountRow): string {
  if (!auth.isAdmin) return '仅管理员可登录/登出交易账户'
  if (!row.gatewayRunning) return GATEWAY_NOT_RUNNING_HINT
  if (!canLogoutTd(row)) return '当前未登录'
  return '登出该交易账户（连接中/登录中/重连中时点此可强制中断）'
}

/** 登录请求：HTTP 200 只表示已写通道，成败以 progress / td_status 为准（页面不假定结果） */
async function loginSession(row: TdAccountRow): Promise<void> {
  const result = await store.login(row.account_id)
  if (result.ok) {
    toast.success(`已下发登录请求：${store.displayName(row.account_id)}`)
  } else if (result.kind === 'conflict' || result.kind === 'failed') {
    toast.error(result.message)
  } else if (result.kind === 'timeout') {
    toast.warning('登录请求已下发，但未在预期时间内收到进度回报，请检查网关与柜台连接')
  }
}

/** 登出请求：任意非未登录状态可发（含强制中断在途登录） */
async function logoutSession(row: TdAccountRow): Promise<void> {
  const result = await store.logout(row.account_id)
  if (result.ok) {
    toast.success(`已下发登出请求：${store.displayName(row.account_id)}`)
  } else if (result.kind === 'conflict' || result.kind === 'failed') {
    toast.error(result.message)
  } else if (result.kind === 'timeout') {
    toast.warning('登出请求已下发，但未在预期时间内收到进度回报，请检查网关')
  }
}

/** 交易日：细状态的 date 是会话交易日（YYYYMMDD），空串 = 未登录（契约 td-status §2.1） */
function tradingDayText(row: TdAccountRow): string {
  const date = row.status?.date ?? ''
  return date === '' ? TD_UNAVAILABLE_TEXT : formatTdDate(date)
}

/** 登录时间：空串 = 未登录（契约 td-status §2.1） */
function loginTimeText(row: TdAccountRow): string {
  const text = row.status?.login_time ?? ''
  return text === '' ? TD_UNAVAILABLE_TEXT : text
}

/** 进度：以契约 progress 为唯一入口（契约 td-status §2.1 注）；无进度时占位 */
function progressText(row: TdAccountRow): string {
  const p = row.progress
  if (!p) return TD_UNAVAILABLE_TEXT
  const desc = p.desc ? `${p.desc} · ` : ''
  return `${desc}${p.current}/${p.max}`
}

/** 进度条宽度（不确定进度 max<=min 时给 0） */
function progressPercent(row: TdAccountRow): number {
  const p = row.progress
  if (!p || p.max <= p.min) return 0
  return Math.min(100, Math.max(0, ((p.current - p.min) / (p.max - p.min)) * 100))
}
</script>

<template>
  <article class="td-card ds-card" :data-test="`td-interface-card-${tdInterface.name}`">
    <!-- 头部：显示名 + 进程名 + 进程状态 +（非 Running 时）启动进程 -->
    <div class="td-card__head">
      <div class="td-card__identity">
        <span class="td-card__name" data-test="td-interface-name">{{ tdInterface.displayName }}</span>
        <span class="td-card__process" data-test="td-interface-process">{{ tdInterface.name }}</span>
      </div>
      <div class="td-card__meta">
        <span class="td-card__state" :style="{ color: stateColor }" data-test="td-interface-state">
          进程：{{ stateText }}
        </span>
        <span class="ds-tag td-card__count" data-test="td-interface-account-count">
          账户 {{ tdInterface.accountCount }} · 启用 {{ tdInterface.enabledCount }} · 就绪 {{ tdInterface.readyCount }}
        </span>
        <button
          v-if="canWrite && !running"
          class="ds-btn ds-btn--sm ds-btn--primary"
          type="button"
          data-test="td-interface-start"
          :disabled="startPending || removePending"
          :title="startProcessTitle"
          @click="startProcess"
        >
          <span v-if="startPending" class="ds-btn__spinner"></span>
          <Icon v-else name="play-filled" :size="14" />
          {{ startPending ? '启动中' : '启动进程' }}
        </button>
      </div>
    </div>

    <!-- 体：账户表 + 卡片内添加账户 -->
    <div class="td-card__body">
      <div class="td-card__body-head">
        <span class="td-card__body-title">账户</span>
        <button
          class="ds-btn ds-btn--secondary ds-btn--sm"
          type="button"
          data-test="td-card-add-account"
          :disabled="!canAddAccount"
          :title="accountActionTitle('添加账户到该交易接口')"
          @click="openAdd"
        >
          <span v-if="addingAccount" class="ds-btn__spinner"></span>
          <Icon v-else name="Plus" :size="14" />
          添加账户
        </button>
      </div>

      <div v-if="tdInterface.accountCount === 0" class="td-card__empty" data-test="td-interface-empty">
        该交易接口下还没有账户
        <span class="td-card__empty-hint">
          点击「添加账户」创建第一个账户；账户 ID 是柜台真实账户号，全平台唯一
        </span>
      </div>

      <div v-else class="ds-table-card td-card__table-card">
        <table class="ds-table td-card__table">
          <thead>
            <tr>
              <th>账户名称</th>
              <th>启用</th>
              <th>连接状态</th>
              <th>交易日</th>
              <th>登录时间</th>
              <th>进度</th>
              <th class="td-card__col-actions">操作</th>
            </tr>
          </thead>
          <tbody>
            <tr
              v-for="row in tdInterface.accounts"
              :key="row.account_id"
              :class="{ 'td-card__row--disabled': !row.enabled }"
              :data-test="`td-account-row-${row.account_id}`"
            >
              <td>
                <div class="td-card__account-cell">
                  <div class="td-card__account-names">
                    <!-- 主显展示名（空名回落 id，ADR 0056 决策 3/9）；名字与 id 不同时 id 作副行小字保留可查 -->
                    <span class="td-card__account-name" data-test="td-account-name">{{ store.displayName(row.account_id) }}</span>
                    <span
                      v-if="store.displayName(row.account_id) !== row.account_id"
                      class="td-card__account-id"
                      data-test="td-account-id"
                    >{{ row.account_id }}</span>
                  </div>
                  <span v-if="row.riskControlEnabled" class="ds-tag td-card__badge">风控</span>
                </div>
              </td>
              <td>
                <span class="ds-tag" :class="row.enabled ? '' : 'td-card__badge--muted'" data-test="td-account-enabled">
                  {{ row.enabled ? '已启用' : '已禁用' }}
                </span>
              </td>
              <td>
                <span class="td-card__state" :class="statusClass(row)" data-test="td-account-state">
                  {{ statusText(row) }}
                </span>
                <div v-if="row.status?.progress_desc" class="td-card__account-meta">{{ row.status.progress_desc }}</div>
                <!-- 契约 td-status §2.5 前端义务③：持久展示上次登录链失败的原因 + 时间。
                     失败停在失败态后 state/progress 都不会再变，这一行是用户事后回看的唯一入口；
                     原因空时按 code 映射中文，绝不展示 snake_case 机器码。 -->
                <div
                  v-if="row.lastError"
                  class="td-card__account-meta td-card__account-meta--warn"
                  :title="lastErrorTitle(row.lastError)"
                  :data-test="`td-last-error-${row.account_id}`"
                >{{ lastErrorText(row.lastError) }}</div>
                <!-- 会话控制（ADR 0051）：登录仅在完全未登录可用；登出在任意非未登录态可用（含强制中断） -->
                <div class="td-card__session-actions">
                  <button
                    class="ds-btn ds-btn--sm ds-btn--primary"
                    type="button"
                    :disabled="store.sessionPending(row.account_id, 'login') || !canLoginRow(row)"
                    :title="loginDisabledTitle(row)"
                    :aria-label="`登录账户 ${store.displayName(row.account_id)}`"
                    :data-test="`td-login-${row.account_id}`"
                    @click="loginSession(row)"
                  >
                    <span v-if="store.sessionPending(row.account_id, 'login')" class="ds-btn__spinner"></span>
                    {{ store.sessionPending(row.account_id, 'login') ? '登录中' : '登录' }}
                  </button>
                  <button
                    class="ds-btn ds-btn--sm ds-btn--danger-subtle"
                    type="button"
                    :disabled="store.sessionPending(row.account_id, 'logout') || !canLogoutRow(row)"
                    :title="logoutDisabledTitle(row)"
                    :aria-label="`登出账户 ${store.displayName(row.account_id)}`"
                    :data-test="`td-logout-${row.account_id}`"
                    @click="logoutSession(row)"
                  >
                    <span v-if="store.sessionPending(row.account_id, 'logout')" class="ds-btn__spinner"></span>
                    {{ store.sessionPending(row.account_id, 'logout') ? '登出中' : '登出' }}
                  </button>
                </div>
              </td>
              <td class="td-card__mono">{{ tradingDayText(row) }}</td>
              <td class="td-card__mono">{{ loginTimeText(row) }}</td>
              <td class="td-card__progress-cell">
                <span class="td-card__mono">{{ progressText(row) }}</span>
                <div class="td-card__progress-track" role="presentation">
                  <div class="td-card__progress-fill" :style="{ width: `${progressPercent(row)}%` }"></div>
                </div>
              </td>
              <td class="td-card__col-actions">
                <div class="td-card__row-actions">
                  <button
                    class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                    type="button"
                    :disabled="accountWriteBlockedText !== '' || store.accountPending(row.account_id)"
                    :title="accountActionTitle(`编辑账户 ${store.displayName(row.account_id)}`)"
                    :aria-label="`编辑账户 ${store.displayName(row.account_id)}`"
                    :data-test="`td-edit-${row.account_id}`"
                    @click="openEdit(row)"
                  >
                    <Icon name="edit" :size="14" />
                  </button>
                  <button
                    class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                    type="button"
                    :disabled="accountWriteBlockedText !== '' || store.accountPending(row.account_id)"
                    :title="accountActionTitle(row.enabled
                      ? `禁用账户 ${store.displayName(row.account_id)}（已有会话不会被强制断开）`
                      : `启用账户 ${store.displayName(row.account_id)}`)"
                    :aria-label="`${row.enabled ? '禁用' : '启用'}账户 ${store.displayName(row.account_id)}`"
                    :data-test="`td-toggle-${row.account_id}`"
                    @click="toggleEnabled(row)"
                  >
                    <span v-if="store.isOpPending(row.gateway, row.account_id, 'toggle_enabled')" class="ds-btn__spinner"></span>
                    <!-- 静态 name 而非 :name 三元：图标名测试只静态扫描字面量，
                         动态 name 必须登记登记表（本卡不值得为两个固定图标引入登记项） -->
                    <Icon v-else-if="row.enabled" name="pause" :size="14" />
                    <Icon v-else name="play-filled" :size="14" />
                  </button>
                  <button
                    class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon"
                    type="button"
                    :disabled="accountWriteBlockedText !== '' || store.accountPending(row.account_id)"
                    :title="accountActionTitle(`删除账户 ${store.displayName(row.account_id)}`)"
                    :aria-label="`删除账户 ${store.displayName(row.account_id)}`"
                    :data-test="`td-remove-${row.account_id}`"
                    @click="openRemove(row)"
                  >
                    <Icon name="Delete" :size="14" />
                  </button>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- 底部：删除交易接口（整行 danger；有账户时禁用，权威 guard 在服务端 409） -->
    <div v-if="canWrite" class="td-card__foot">
      <button
        class="ds-btn ds-btn--danger-subtle td-card__remove-interface"
        type="button"
        data-test="td-interface-remove"
        :disabled="removeInterfaceBlockedText !== '' || removePending"
        :title="removeInterfaceTitle"
        @click="openRemoveInterface"
      >
        <span v-if="removePending" class="ds-btn__spinner"></span>
        <Icon v-else name="Delete" :size="14" />
        {{ removePending ? '移除中…' : '删除交易接口' }}
      </button>
    </div>

    <!-- 添加 / 编辑账户（gateway 由本卡片固定传入；无网关下拉，ADR 0053） -->
    <TdAccountFormModal
      :open="formOpen"
      :editing="editingRow"
      :editing-account-name="editingAccountName"
      :gateway="tdInterface.name"
      :submitting="formSubmitting"
      :error-text="formError"
      @close="closeForm"
      @submit="submitForm"
    />

    <!-- 删除账户二次确认：写清影响范围（不删交易数据 + 有会话会被销毁并推 Offline） -->
    <Modal :open="removingRow !== null" title="删除交易账户" @close="closeRemove">
      <div v-if="removingRow" class="td-card__confirm">
        <p class="td-card__confirm-line" data-test="td-remove-confirm-text">
          确认删除账户 <strong>{{ store.displayName(removingRow.account_id) }}</strong>（账户 ID {{ removingRow.account_id }} · 交易接口 {{ tdInterface.name }}）？
        </p>
        <ul class="td-card__confirm-list">
          <li>配置将从该交易接口的配置文件中移除，账户随即停止参与交易与查询。</li>
          <li>若该账户当前有活跃会话，会话会被销毁并推送离线状态。</li>
          <li><strong>已存在的交易数据不会被删除</strong>（委托/成交/持仓/资金仍可按锚点日查询）。</li>
        </ul>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" :disabled="removeSubmitting" @click="closeRemove">取消</button>
        <button
          class="ds-btn ds-btn--danger"
          type="button"
          data-test="td-remove-confirm"
          :disabled="removeSubmitting"
          @click="confirmRemove"
        >
          <span v-if="removeSubmitting" class="ds-btn__spinner"></span>
          {{ removeSubmitting ? '删除中…' : '确认删除' }}
        </button>
      </template>
    </Modal>

    <!-- 删除交易接口二次确认：停止进程 + 从启动配置移除 + 不可撤销（ADR 0053） -->
    <Modal :open="removeInterfaceOpen" title="删除交易接口" @close="closeRemoveInterface">
      <div class="td-card__confirm">
        <p class="td-card__confirm-line" data-test="td-interface-remove-confirm-text">
          确认删除交易接口 <strong>{{ tdInterface.displayName }}</strong>（进程 {{ tdInterface.name }}）？
        </p>
        <ul class="td-card__confirm-list">
          <li>该交易接口进程会被<strong>停止</strong>。</li>
          <li>该条目会从 master 的<strong>启动配置中移除</strong>，重启后不再拉起。</li>
          <li><strong>不可撤销</strong>；如需恢复只能重新添加交易接口。</li>
          <li>请确认该接口下已无账户（有账户时删除会被服务端拒绝）。</li>
        </ul>
        <div v-if="removeInterfaceError" class="ds-alert ds-alert--danger" role="alert" data-test="td-interface-remove-error">
          <span class="ds-alert__icon"><Icon name="Error" :size="16" /></span>
          <span>{{ removeInterfaceError }}</span>
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" :disabled="removeInterfaceSubmitting" @click="closeRemoveInterface">取消</button>
        <button
          class="ds-btn ds-btn--danger"
          type="button"
          data-test="td-interface-remove-confirm"
          :disabled="removeInterfaceSubmitting"
          @click="confirmRemoveInterface"
        >
          <span v-if="removeInterfaceSubmitting" class="ds-btn__spinner"></span>
          {{ removeInterfaceSubmitting ? '删除中…' : '确认删除' }}
        </button>
      </template>
    </Modal>
  </article>
</template>

<style scoped>
.td-card {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-12);
  min-width: 0;
}

.td-card__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-12);
  flex-wrap: wrap;
}

.td-card__identity {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.td-card__name {
  font-size: var(--heading-sm-font-size);
  line-height: var(--heading-sm-line-height);
  font-weight: var(--heading-sm-font-weight);
  color: var(--text-default);
}

.td-card__process {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
  font-variant-numeric: tabular-nums;
}

.td-card__meta {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  flex-wrap: wrap;
}

.td-card__state {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
}

.td-card__count {
  font-variant-numeric: tabular-nums;
}

.td-card__body {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  min-width: 0;
}

.td-card__body-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
}

.td-card__body-title {
  font-size: var(--body-sm-font-size);
  font-weight: var(--font-weight-medium);
  color: var(--text-secondary);
}

.td-card__table-card {
  min-width: 0;
}

.td-card__table {
  min-width: 100%;
}

.td-card__col-actions {
  text-align: right;
}

.td-card__row--disabled {
  opacity: 0.62;
}

.td-card__account-cell {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

/* 展示名主行 + 账户 ID 副行（ADR 0056 决策 9：主显 name，id 保留可查） */
.td-card__account-names {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.td-card__account-name {
  font-weight: var(--font-weight-medium);
  color: var(--text-default);
}

.td-card__account-id {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
  font-variant-numeric: tabular-nums;
}

.td-card__badge {
  font-size: var(--body-xs-font-size);
}

.td-card__badge--muted {
  color: var(--text-tertiary);
}

.td-card__account-meta {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

/* 上次失败（契约 td-status §2.5）：持久告警色，与「进度描述」这类中性元信息区分开 */
.td-card__account-meta--warn {
  color: var(--status-warning-default);
}

.td-card__state--ok {
  color: var(--status-success-default);
}

.td-card__state--busy {
  color: var(--status-warning-default);
}

.td-card__state--idle {
  color: var(--text-tertiary);
}

/* 会话控制双按钮（ADR 0051）：操作紧挨它影响的状态，不混进右侧配置操作 */
.td-card__session-actions {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  margin-top: var(--spacer-8);
}

.td-card__mono {
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.td-card__progress-cell {
  min-width: 140px;
}

.td-card__progress-track {
  height: 4px;
  margin-top: var(--spacer-4);
  border-radius: var(--radius-full);
  background: var(--bg-overlay-l2);
  overflow: hidden;
}

.td-card__progress-fill {
  height: 100%;
  border-radius: var(--radius-full);
  background: var(--status-primary-default);
  transition: width 120ms linear;
}

.td-card__row-actions {
  display: flex;
  gap: var(--spacer-4);
  justify-content: flex-end;
}

.td-card__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacer-4);
  padding: var(--spacer-20) var(--spacer-16);
  color: var(--text-secondary);
  font-size: var(--body-sm-font-size);
}

.td-card__empty-hint {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-card__foot {
  display: flex;
  justify-content: stretch;
}

.td-card__remove-interface {
  width: 100%;
  justify-content: center;
}

.td-card__confirm {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
}

.td-card__confirm-line {
  margin: 0;
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  color: var(--text-default);
}

.td-card__confirm-list {
  margin: 0;
  padding-left: var(--spacer-20);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-secondary);
}

@media (max-width: 767.98px) {
  .td-card__head {
    align-items: flex-start;
    flex-direction: column;
  }

  .td-card__empty {
    padding: var(--spacer-16) var(--spacer-12);
  }
}
</style>
