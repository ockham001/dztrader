<script setup lang="ts">
/**
 * 交易账户配置页（#251；契约 rest §2.9.7/§2.9.9/§4，契约 td-config §1/§2/§5，契约 td-status）。
 *
 * 页面口径（全部来自契约，不是自由设计）：
 * - **账户列表 = 注册表视角**（`GET /api/td/accounts`，契约 rest §2.9.9）：跨网关账户全集，
 *   不是单网关配置镜像。注册表 `available:false` 时展示降级提示，**绝不**把列表当空
 *   （"无账户"在降级时不可信，契约 rest §2.9.9 约束）；降级期间仍展示各网关已推来的配置账户，
 *   并允许提交（预检跳过，见 store 的取舍注释）。
 * - **细状态**（登录态/交易日/登录时间/进度）取 WS `td_rtn_status` 与 `progress`，
 *   不从 `td_rtn_config` 推导（契约 rest §4 前端义务）。
 * - **密码恒不回显**：配置镜像里是 `"****"`，页面不提供明文查看入口；表单编辑时留空 = 不修改。
 * - **唯一性冲突**：添加前查注册表，冲突直接提示且不发请求（体验优化；权威在校验端 dzweb）。
 * - 删除必须二次确认（含影响范围：不删除交易数据，ADR 0045）。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TdAccountFormModal from '@/components/trading/TdAccountFormModal.vue'
import type { TdAccountFormValue } from '@/components/trading/TdAccountFormModal.vue'
import { useTdAccountsStore, tdStateText, type TdAccountRow } from '@/stores/tdAccounts'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import { formatTdDate } from '@/composables/tdPositionView'
import { TD_UNAVAILABLE_TEXT } from '@/composables/tdOrderView'
import type { TdUpdateAccountBody } from '@/types/api'

const store = useTdAccountsStore()
const auth = useAuthStore()
const toast = useToastStore()

// ===== 生命周期：挂载重拉注册表，卸载停自愈重试 =====
onMounted(() => { void store.init() })
onBeforeUnmount(() => { store.dispose() })

// store.error（HTTP 失败链路）统一 error toast 并清空，页面内不重复展示
watch(() => store.error, (msg) => {
  if (msg) {
    toast.error(msg)
    store.clearError()
  }
})

/** 写操作权限：契约 rest §2.9.7 预检 1 —— admin 角色；非 admin 只读（按钮禁用并给出原因） */
const canWrite = computed(() => auth.isAdmin)
const readOnlyHint = '仅管理员可添加/修改/删除交易账户'

// ===== 表单（添加 / 编辑共用）=====
const formOpen = ref(false)
const editingRow = ref<TdAccountRow | null>(null)
const formSubmitting = ref(false)
const formError = ref('')

function openAdd(): void {
  editingRow.value = null
  formError.value = ''
  formOpen.value = true
}

function openEdit(row: TdAccountRow): void {
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
 * 表单提交 → store 写操作。
 *
 * 添加：`{gateway, account}`（契约 rest §2.9.7 POST）。
 * 编辑：`UpdateAccount` params 子集——**不含 account_id**（路径是唯一来源，出现即 400）。
 * 密码留空/`****` 一律不提交（= 保留旧值，契约 td-config §2.3）；前端永不把掩码当密码发出去。
 */
async function submitForm(value: TdAccountFormValue): Promise<void> {
  formSubmitting.value = true
  formError.value = ''
  try {
    if (editingRow.value === null) {
      const result = await store.addAccount(value.gateway, {
        account_id: value.account_id,
        enabled: value.enabled,
        risk_control_enabled: value.risk_control_enabled,
        currency_id: value.currency_id,
        ext: {
          broker_id: value.broker_id,
          user_id: value.user_id,
          password: value.password,
          product_info: value.product_info,
          frontends: value.frontends,
          auth_code: value.auth_code,
          app_id: value.app_id,
          flow_dir: value.flow_dir,
        },
      })
      if (!result.ok) {
        if (result.kind === 'conflict' || result.kind === 'failed') formError.value = result.message
        if (result.kind === 'timeout') formError.value = '提交超时：交易网关未在预期时间内回报配置，请稍后刷新确认'
        return
      }
      formOpen.value = false
      toast.success('已提交添加请求，等待交易网关回报配置')
      return
    }

    const patch = buildUpdatePatch(value)
    const result = await store.updateAccount(editingRow.value.account_id, patch)
    if (!result.ok) {
      if (result.kind === 'conflict' || result.kind === 'failed') formError.value = result.message
      if (result.kind === 'timeout') formError.value = '提交超时：交易网关未在预期时间内回报配置，请稍后刷新确认'
      return
    }
    formOpen.value = false
    toast.success('已提交修改请求，等待交易网关回报配置')
  } finally {
    formSubmitting.value = false
  }
}

/**
 * 构造 `UpdateAccount` 的 params 子集（契约 td-config §2.1）：
 * 只包含**确实要改**的字段——空字符串表示「置空该字段」（如清空 flow_dir），
 * 但密码例外（空 = 不修改，契约 §2.3）。`frontends` 提供即整体替换。
 */
function buildUpdatePatch(value: TdAccountFormValue): TdUpdateAccountBody {
  const patch: TdUpdateAccountBody = {
    enabled: value.enabled,
    risk_control_enabled: value.risk_control_enabled,
    currency_id: value.currency_id,
    ext: {
      broker_id: value.broker_id,
      user_id: value.user_id,
      product_info: value.product_info,
      frontends: value.frontends,
      auth_code: value.auth_code,
      app_id: value.app_id,
      flow_dir: value.flow_dir,
    },
  }
  // 密码留空或残留掩码 = 保留旧值，绝不提交（契约 td-config §2.3；避免把 **** 写成新密码）
  if (value.password !== '' && value.password !== '****') {
    patch.ext!.password = value.password
  }
  return patch
}

// ===== 启停（契约 rest §2.9.7：走 UpdateAccount，body {enabled}）=====
async function toggleEnabled(row: TdAccountRow): Promise<void> {
  const result = await store.setAccountEnabled(row.account_id, !row.enabled)
  if (!result.ok && (result.kind === 'conflict' || result.kind === 'failed')) {
    toast.error(result.message)
  } else if (!result.ok && result.kind === 'timeout') {
    toast.warning('启停请求已下发，但交易网关未在预期时间内回报配置')
  } else if (result.ok) {
    toast.success(`已提交${row.enabled ? '禁用' : '启用'}请求，等待交易网关回报配置`)
  }
}

// ===== 删除二次确认（含影响范围）=====
const removingRow = ref<TdAccountRow | null>(null)
const removeSubmitting = ref(false)

function openRemove(row: TdAccountRow): void {
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
      toast.success(`已提交删除请求：${row.account_id}`)
      return
    }
    if (result.kind === 'conflict' || result.kind === 'failed') {
      toast.error(result.message)
      removingRow.value = null
    } else if (result.kind === 'timeout') {
      toast.warning('删除请求已下发，但交易网关未在预期时间内回报配置')
      removingRow.value = null
    }
  } finally {
    removeSubmitting.value = false
  }
}

// ===== 展示辅助 =====
const rowCount = computed(() => store.rows.length)
const enabledCount = computed(() => store.rows.filter(r => r.enabled).length)
/** 就绪 = 细状态 Ready（契约 td-status §2.2），用于页头「就绪数」聚合 */
const readyCount = computed(() => store.rows.filter(r => r.status?.state === 'Ready').length)

/** 细状态文案（未上报 = 占位；契约 td-status §2.2 的 11 态映射） */
function statusText(row: TdAccountRow): string {
  return tdStateText(row.status?.state ?? null)
}

/** 状态样式：Ready 正常、Idle 中性、其余中间态提示（与进程状态色口径一致） */
function statusClass(row: TdAccountRow): string {
  const state = row.status?.state
  if (state === 'Ready') return 'td-accounts__state--ok'
  if (state === undefined || state === 'Idle') return 'td-accounts__state--idle'
  return 'td-accounts__state--busy'
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

/** 页面级空态：注册表可用、无错误、且没有任何账户行（降级不算空） */
const showEmpty = computed(() => !store.loading && store.isEmpty)
</script>

<template>
  <div class="td-accounts">
    <div class="td-accounts__header">
      <div class="td-accounts__title-group">
        <h1 class="td-accounts__title">交易账户</h1>
        <span class="ds-tag td-accounts__count" data-test="td-account-count">
          共 {{ rowCount }} 个账户 · 启用 {{ enabledCount }} · 就绪 {{ readyCount }}
        </span>
      </div>
      <div class="td-accounts__actions">
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          data-test="td-add-account"
          :disabled="!canWrite || store.addPending"
          :title="canWrite ? '添加交易账户配置' : readOnlyHint"
          @click="openAdd"
        >
          <span v-if="store.addPending" class="ds-btn__spinner"></span>
          <Icon v-else name="Plus" :size="16" />
          添加账户
        </button>
      </div>
    </div>

    <!-- 降级提示（契约 rest §2.9.9：不得把降级当空列表；非 admin 只读提示） -->
    <div v-if="!store.available" class="ds-alert ds-alert--warning td-accounts__banner" role="status" data-test="td-accounts-degraded">
      <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
      <span>{{ store.degradedText }}（当前展示的是各网关最近一次上报的配置账户）</span>
    </div>
    <div v-if="!canWrite" class="ds-alert ds-alert--info td-accounts__banner" role="status" data-test="td-accounts-readonly">
      <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
      <span>{{ readOnlyHint }}；你仍可查看账户配置与连接状态</span>
    </div>

    <!-- 账户表：注册表为视角，配置/状态/进度镜像补全 -->
    <div class="ds-table-card td-accounts__table-card">
      <table class="ds-table td-accounts__table">
        <thead>
          <tr>
            <th>账户 ID</th>
            <th>交易网关</th>
            <th>启用</th>
            <th>连接状态</th>
            <th>交易日</th>
            <th>登录时间</th>
            <th>进度</th>
            <th class="td-accounts__col-actions">操作</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="row in store.rows"
            :key="row.account_id"
            :class="{ 'td-accounts__row--disabled': !row.enabled }"
            :data-test="`td-account-row-${row.account_id}`"
          >
            <td>
              <div class="td-accounts__account-cell">
                <span class="td-accounts__account-id">{{ row.account_id }}</span>
                <span v-if="row.riskControlEnabled" class="ds-tag td-accounts__badge">风控</span>
              </div>
              <div class="td-accounts__account-meta">{{ row.currencyId }}</div>
            </td>
            <td>
              <div class="td-accounts__gateway">{{ row.gateway }}</div>
              <div class="td-accounts__account-meta">
                <span :class="row.interfaceType === 'ctp' ? '' : 'td-accounts__account-meta--warn'">
                  {{ row.interfaceType === '' ? '未知接口类型' : row.interfaceType }}
                </span>
                <span v-if="!row.gatewayRunning" class="td-accounts__account-meta--warn"> · 网关未运行</span>
              </div>
            </td>
            <td>
              <span class="ds-tag" :class="row.enabled ? '' : 'td-accounts__badge--muted'" data-test="td-account-enabled">
                {{ row.enabled ? '已启用' : '已禁用' }}
              </span>
            </td>
            <td>
              <span class="td-accounts__state" :class="statusClass(row)" data-test="td-account-state">
                {{ statusText(row) }}
              </span>
              <div v-if="row.status?.progress_desc" class="td-accounts__account-meta">{{ row.status.progress_desc }}</div>
            </td>
            <td class="td-accounts__mono">{{ tradingDayText(row) }}</td>
            <td class="td-accounts__mono">{{ loginTimeText(row) }}</td>
            <td class="td-accounts__progress-cell">
              <span class="td-accounts__mono">{{ progressText(row) }}</span>
              <div class="td-accounts__progress-track" role="presentation">
                <div class="td-accounts__progress-fill" :style="{ width: `${progressPercent(row)}%` }"></div>
              </div>
            </td>
            <td class="td-accounts__col-actions">
              <div class="td-accounts__row-actions">
                <button
                  class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                  type="button"
                  :disabled="!canWrite || store.accountPending(row.account_id)"
                  :title="canWrite ? `编辑账户 ${row.account_id}` : readOnlyHint"
                  :aria-label="`编辑账户 ${row.account_id}`"
                  :data-test="`td-edit-${row.account_id}`"
                  @click="openEdit(row)"
                >
                  <Icon name="edit" :size="14" />
                </button>
                <button
                  class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                  type="button"
                  :disabled="!canWrite || store.accountPending(row.account_id)"
                  :title="canWrite
                    ? (row.enabled ? `禁用账户 ${row.account_id}（已有会话不会被强制断开）` : `启用账户 ${row.account_id}`)
                    : readOnlyHint"
                  :aria-label="`${row.enabled ? '禁用' : '启用'}账户 ${row.account_id}`"
                  :data-test="`td-toggle-${row.account_id}`"
                  @click="toggleEnabled(row)"
                >
                  <span v-if="store.isOpPending(row.gateway, row.account_id, 'toggle_enabled')" class="ds-btn__spinner"></span>
                  <!-- 静态 name 而非 :name 三元：图标名测试只静态扫描字面量，
                       动态 name 必须登记登记表（本页不值得为两个固定图标引入登记项） -->
                  <Icon v-else-if="row.enabled" name="pause" :size="14" />
                  <Icon v-else name="play-filled" :size="14" />
                </button>
                <button
                  class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon"
                  type="button"
                  :disabled="!canWrite || store.accountPending(row.account_id)"
                  :title="canWrite ? `删除账户 ${row.account_id}` : readOnlyHint"
                  :aria-label="`删除账户 ${row.account_id}`"
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

      <!-- 空态：**成功**读到注册表且没有账户；降级/加载中不是空态（契约 rest §2.9.9） -->
      <div v-if="showEmpty" class="td-accounts__empty" data-test="td-accounts-empty">
        还没有配置任何交易账户
        <span class="td-accounts__empty-hint">点击右上角「添加账户」创建第一个账户配置；账户 ID 在全部交易网关间唯一</span>
      </div>
      <div v-else-if="store.loading && rowCount === 0" class="td-accounts__empty" data-test="td-accounts-loading">
        <span class="ds-btn__spinner"></span> 正在读取账户注册表…
      </div>
    </div>

    <!-- 添加 / 编辑表单 -->
    <TdAccountFormModal
      :open="formOpen"
      :editing="editingRow"
      :gateways="store.gatewayOptions"
      :submitting="formSubmitting"
      :error-text="formError"
      @close="closeForm"
      @submit="submitForm"
    />

    <!-- 删除二次确认：必须写清影响范围（不删交易数据 + 有会话会被销毁并推 Offline） -->
    <Modal
      :open="removingRow !== null"
      title="删除交易账户"
      @close="closeRemove"
    >
      <div v-if="removingRow" class="td-accounts__confirm">
        <p class="td-accounts__confirm-line" data-test="td-remove-confirm-text">
          确认删除账户 <strong>{{ removingRow.account_id }}</strong>（交易网关 {{ removingRow.gateway }}）？
        </p>
        <ul class="td-accounts__confirm-list">
          <li>配置将从该网关的配置文件中移除，账户随即停止参与交易与查询。</li>
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
  </div>
</template>

<style scoped>
.td-accounts {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
  max-width: 1600px;
  width: 100%;
  margin: 0 auto;
  padding: var(--spacer-32) var(--spacer-24);
  overflow: auto;
}

.td-accounts__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
  flex-wrap: wrap;
}

.td-accounts__title-group {
  display: flex;
  align-items: center;
  gap: var(--spacer-16);
}

.td-accounts__title {
  margin: 0;
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
}

.td-accounts__count {
  font-variant-numeric: tabular-nums;
}

.td-accounts__actions {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-accounts__banner {
  margin-bottom: var(--spacer-16);
}

.td-accounts__table-card {
  display: flex;
  flex-direction: column;
}

.td-accounts__table {
  min-width: 100%;
}

.td-accounts__col-actions {
  text-align: right;
}

.td-accounts__row--disabled {
  opacity: 0.62;
}

.td-accounts__account-cell {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-accounts__account-id {
  font-weight: var(--font-weight-medium);
  color: var(--text-default);
}

.td-accounts__badge {
  font-size: var(--body-xs-font-size);
}

.td-accounts__badge--muted {
  color: var(--text-tertiary);
}

.td-accounts__account-meta {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-accounts__account-meta--warn {
  color: var(--status-warning-default);
}

.td-accounts__gateway {
  font-variant-numeric: tabular-nums;
}

.td-accounts__state--ok {
  color: var(--status-success-default);
}

.td-accounts__state--busy {
  color: var(--status-warning-default);
}

.td-accounts__state--idle {
  color: var(--text-tertiary);
}

.td-accounts__mono {
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.td-accounts__progress-cell {
  min-width: 140px;
}

.td-accounts__progress-track {
  height: 4px;
  margin-top: var(--spacer-4);
  border-radius: var(--radius-full);
  background: var(--bg-overlay-l2);
  overflow: hidden;
}

.td-accounts__progress-fill {
  height: 100%;
  border-radius: var(--radius-full);
  background: var(--status-primary-default);
  transition: width 120ms linear;
}

.td-accounts__row-actions {
  display: flex;
  gap: var(--spacer-4);
  justify-content: flex-end;
}

.td-accounts__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacer-8);
  padding: var(--spacer-48) var(--spacer-24);
  color: var(--text-secondary);
}

.td-accounts__empty-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

.td-accounts__confirm {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
}

.td-accounts__confirm-line {
  margin: 0;
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  color: var(--text-default);
}

.td-accounts__confirm-list {
  margin: 0;
  padding-left: var(--spacer-20);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-secondary);
}

@media (max-width: 767.98px) {
  .td-accounts__header {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
