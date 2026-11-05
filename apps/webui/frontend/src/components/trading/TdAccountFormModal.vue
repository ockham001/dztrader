<script setup lang="ts">
/**
 * 交易账户表单（添加 / 编辑共用，契约 td-config §2.2/§2.3，契约 rest §2.9.7）。
 *
 * 为什么添加与编辑共用一个组件：两者字段集几乎相同，差别只有两处契约语义——
 * - **`account_id` 是不可变 key**（契约 td-config §2.1）：编辑时只读展示，不随请求提交；
 * - **密码是「缺失/空串/`****` = 保留旧值」**（契约 td-config §2.3）：编辑时留空即不修改，
 *   占位提示 `****`（回报恒脱敏），前端**从不**回填明文（镜像里的 password 恒为 `"****"`）。
 *
 * 字段按接口类型选择（契约 rest §4 前端义务）：本期只有 `ctp`，`ext` 字段集由
 * {@link CTP_FIELD_KEYS} 单一登记；未来新增接口类型时在这里按 interfaceType 分派，
 * 通用层字段（account_id/enabled/risk_control_enabled/currency_id）不动。
 *
 * 组件只负责**收集与校验**，不发起请求、不碰 store：提交由父页面调 store（单一写入点）。
 */
import { computed, ref, watch } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import Dropdown from '@/components/shared/Dropdown.vue'
import Icon from '@/components/shared/Icon.vue'
import { useFieldId } from '@/composables/useFieldId'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import type { TdAccountRow } from '@/stores/tdAccounts'

export interface TdAccountFormValue {
  account_id: string
  gateway: string
  enabled: boolean
  risk_control_enabled: boolean
  currency_id: string
  /** CTP ext（契约 td-config §2.3） */
  broker_id: string
  user_id: string
  /** 空串 = 不修改（编辑）/ 未填（添加）；永远不会是 `****`（那表示保留旧值） */
  password: string
  product_info: string
  frontends: { address: string; label: string; enabled: boolean }[]
  auth_code: string
  app_id: string
  flow_dir: string
}

const props = defineProps<{
  open: boolean
  /** null = 添加；非 null = 编辑该行 */
  editing: TdAccountRow | null
  /** 可选的交易网关进程名（来自 process_config 镜像的 dztd_*） */
  gateways: { name: string; running: boolean }[]
  /** 提交中（按钮 spinner + 禁用） */
  submitting: boolean
  /** 提交失败文案（表单内展示，不清空用户输入） */
  errorText?: string
}>()

const emit = defineEmits<{
  close: []
  submit: [value: TdAccountFormValue]
}>()

const fieldId = useFieldId()

/** 空白表单（添加态的初值：默认启用、人民币、无前置） */
function emptyForm(gateway: string): TdAccountFormValue {
  return {
    account_id: '',
    gateway,
    enabled: true,
    risk_control_enabled: false,
    currency_id: 'CNY',
    broker_id: '',
    user_id: '',
    password: '',
    product_info: '',
    frontends: [],
    auth_code: '',
    app_id: '',
    flow_dir: '',
  }
}

const form = ref<TdAccountFormValue>(emptyForm(''))

/**
 * 用行数据回填编辑表单。
 *
 * 密码**不回填**：配置镜像的 `ext.password` 恒为 `"****"`（契约 td-config §2.3/§4 脱敏），
 * 回填它只会诱导用户把掩码当密码提交；留空并在占位里说明「留空 = 不修改」。
 */
function formFromRow(row: TdAccountRow): TdAccountFormValue {
  const base = emptyForm(row.gateway)
  const config = row.config
  const ext = config?.ext
  return {
    ...base,
    account_id: row.account_id,
    enabled: config?.enabled ?? row.enabled,
    risk_control_enabled: config?.risk_control_enabled ?? row.riskControlEnabled,
    currency_id: config?.currency_id ?? row.currencyId,
    broker_id: ext?.broker_id ?? '',
    user_id: ext?.user_id ?? '',
    password: '',
    product_info: ext?.product_info ?? '',
    frontends: (ext?.frontends ?? []).map(fe => ({
      address: fe.address,
      label: fe.label ?? '',
      enabled: fe.enabled !== false,
    })),
    auth_code: ext?.auth_code ?? '',
    app_id: ext?.app_id ?? '',
    flow_dir: ext?.flow_dir ?? '',
  }
}

// 打开/切换编辑目标时重建表单（关闭时保留最后一次输入，避免误关后重填；父组件负责重置）
watch(() => [props.open, props.editing], () => {
  if (!props.open) return
  if (props.editing) {
    form.value = formFromRow(props.editing)
    return
  }
  // 添加：默认选第一个可用网关（有 Running 的优先），无网关时留空由用户选择
  const preferred = props.gateways.find(g => g.running)?.name ?? props.gateways[0]?.name ?? ''
  form.value = emptyForm(preferred)
}, { immediate: true })

const isEditing = computed(() => props.editing !== null)
const isCtp = computed(() => {
  const gateway = form.value.gateway
  if (!gateway.startsWith('dztd_')) return false
  const tail = gateway.slice('dztd_'.length)
  const underscore = tail.indexOf('_')
  return (underscore < 0 ? tail : tail.slice(0, underscore)) === 'ctp'
})

const gatewayItems = computed<DropdownItem[]>(() => props.gateways.map(g => ({
  value: g.name,
  label: g.running ? g.name : `${g.name}（未运行）`,
  dot: g.running ? 'var(--status-success-default)' : 'var(--status-warning-default)',
})))

/** account_id 长度上限：`DzAccountId` 容量 31 字节（契约 rest §2.9.7 预检 4） */
const ACCOUNT_ID_MAX_BYTES = 31

const accountIdBytes = computed<number>(() => new TextEncoder().encode(form.value.account_id.trim()).length)
const accountIdTooLong = computed(() => accountIdBytes.value > ACCOUNT_ID_MAX_BYTES)
const accountIdEmpty = computed(() => form.value.account_id.trim() === '')

/** 表单本地校验（提交前置；服务端仍是权威校验点） */
const validationText = computed<string>(() => {
  if (!isEditing.value && accountIdEmpty.value) return '账户 ID 不能为空'
  if (accountIdTooLong.value) return `账户 ID 不能超过 ${ACCOUNT_ID_MAX_BYTES} 字节（当前 ${accountIdBytes.value}）`
  if (form.value.gateway === '') return '请选择交易网关'
  if (isCtp.value) {
    if (form.value.broker_id.trim() === '') return 'CTP 经纪公司代码（broker_id）不能为空'
    if (form.value.user_id.trim() === '') return 'CTP 用户代码（user_id）不能为空'
    // 密码必填只在添加态：编辑态留空 = 保留旧值（契约 td-config §2.3）
    if (!isEditing.value && form.value.password === '') return 'CTP 登录密码不能为空'
  }
  return ''
})

const canSubmit = computed(() => validationText.value === '' && !props.submitting)

function submit(): void {
  if (!canSubmit.value) return
  const raw = form.value
  // 规范化：字符串去首尾空白；未启用的空地址前置丢弃（契约 td-config §7：address 非空才注册）
  emit('submit', {
    ...raw,
    account_id: raw.account_id.trim(),
    broker_id: raw.broker_id.trim(),
    user_id: raw.user_id.trim(),
    product_info: raw.product_info.trim(),
    auth_code: raw.auth_code.trim(),
    app_id: raw.app_id.trim(),
    flow_dir: raw.flow_dir.trim(),
    frontends: raw.frontends
      .map(fe => ({ address: fe.address.trim(), label: fe.label.trim(), enabled: fe.enabled }))
      .filter(fe => fe.address !== ''),
  })
}

// ===== 前置列表编辑（contract td-config §2.3 Frontend）=====
function addFrontend(): void {
  form.value.frontends.push({ address: '', label: '', enabled: true })
}

function removeFrontend(index: number): void {
  form.value.frontends.splice(index, 1)
}

/** 已启用且地址非空的前置数量（0 = 网关拒连，契约 td-config §7，这里只做提示不拦截保存） */
const activeFrontendCount = computed(() => form.value.frontends.filter(
  fe => fe.enabled && fe.address.trim() !== '').length)

const hasFrontendWarning = computed(() => isCtp.value && activeFrontendCount.value === 0)
</script>

<template>
  <Modal
    :open="open"
    :title="isEditing ? `编辑交易账户：${form.account_id}` : '添加交易账户'"
    @close="emit('close')"
  >
    <div class="dialog-form">
      <div class="dialog-field">
        <label class="dialog-field__label" :for="fieldId('account-gateway')">交易网关</label>
        <Dropdown
          :id="fieldId('account-gateway')"
          :items="gatewayItems"
          v-model="form.gateway"
          :placeholder="gateways.length === 0 ? '无已注册交易网关' : '请选择交易网关'"
          :disabled="isEditing || gateways.length === 0"
        />
        <span v-if="gateways.length === 0" class="dialog-field__hint">
          未发现已注册的交易网关进程（dztd_*），请先在行情源页添加并启动交易网关
        </span>
        <span v-else-if="isEditing" class="dialog-field__hint">
          网关由账户注册表决定，不可在此修改（修改账户不会迁移归属）
        </span>
      </div>

      <div class="dialog-field">
        <label class="dialog-field__label" :for="fieldId('account-id')">账户 ID</label>
        <div class="ds-input">
          <input
            :id="fieldId('account-id')"
            v-model="form.account_id"
            type="text"
            :readonly="isEditing"
            :placeholder="isEditing ? '' : '账户唯一标识，跨全部交易网关唯一'"
            autocomplete="off"
          >
        </div>
        <span class="dialog-field__hint" :class="{ 'td-form__hint--warn': accountIdTooLong }">
          <template v-if="isEditing">账户 ID 是不可变标识，不可修改</template>
          <template v-else>跨全部交易网关唯一（含已禁用/离线账户），最多 {{ ACCOUNT_ID_MAX_BYTES }} 字节</template>
          <template v-if="!isEditing"> · 当前 {{ accountIdBytes }}/{{ ACCOUNT_ID_MAX_BYTES }}</template>
        </span>
      </div>

      <div class="td-form__row">
        <label class="ds-check" :style="{ minHeight: 'auto' }">
          <input v-model="form.enabled" type="checkbox">
          <span class="ds-check__box"></span> 启用该账户
        </label>
        <label class="ds-check" :style="{ minHeight: 'auto' }">
          <input v-model="form.risk_control_enabled" type="checkbox">
          <span class="ds-check__box"></span> 账户级风控
        </label>
      </div>

      <div class="dialog-field">
        <label class="dialog-field__label" :for="fieldId('account-currency')">账户币种</label>
        <div class="ds-input">
          <input :id="fieldId('account-currency')" v-model="form.currency_id" type="text" placeholder="CNY" autocomplete="off">
        </div>
      </div>

      <!-- ===== CTP ext（interface_type = ctp，契约 td-config §2.3） ===== -->
      <template v-if="isCtp">
        <div class="td-form__section-title">CTP 连接参数</div>

        <div class="td-form__grid">
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-broker-id')">经纪公司代码</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-broker-id')" v-model="form.broker_id" type="text" placeholder="broker_id" autocomplete="off">
            </div>
          </div>
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-user-id')">用户代码</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-user-id')" v-model="form.user_id" type="text" placeholder="user_id" autocomplete="off">
            </div>
          </div>
        </div>

        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('ctp-password')">登录密码</label>
          <div class="ds-input">
            <input
              :id="fieldId('ctp-password')"
              v-model="form.password"
              type="password"
              :placeholder="isEditing ? '留空 = 不修改密码' : '登录密码'"
              autocomplete="new-password"
            >
          </div>
          <span class="dialog-field__hint">
            <template v-if="isEditing">密码不会回显（回报恒为 ****）；留空表示保留现有密码</template>
            <template v-else>保存后不再回显明文，回报中恒为 ****</template>
          </span>
        </div>

        <div class="td-form__grid">
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-product-info')">产品信息</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-product-info')" v-model="form.product_info" type="text" placeholder="选填" autocomplete="off">
            </div>
          </div>
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-flow-dir')">流文件目录</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-flow-dir')" v-model="form.flow_dir" type="text" placeholder="留空 = 网关自动构造" autocomplete="off">
            </div>
          </div>
        </div>

        <div class="td-form__grid">
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-auth-code')">认证码</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-auth-code')" v-model="form.auth_code" type="text" placeholder="选填" autocomplete="off">
            </div>
          </div>
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-app-id')">AppID</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-app-id')" v-model="form.app_id" type="text" placeholder="选填" autocomplete="off">
            </div>
          </div>
        </div>

        <!-- 前置地址（可增删；提供则整体替换，契约 td-config §2.1 UpdateAccount） -->
        <div class="td-form__section-title">
          前置地址
          <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" data-test="td-add-frontend" @click="addFrontend">
            <Icon name="Plus" :size="14" /> 添加前置
          </button>
        </div>
        <div v-if="form.frontends.length === 0" class="dialog-field__hint" data-test="td-no-frontend">
          尚未配置前置地址；连接时至少需要一个「已启用且地址非空」的前置，否则网关拒绝连接
        </div>
        <div v-for="(fe, index) in form.frontends" :key="`fe-${index}`" class="td-form__frontend-row">
          <label class="ds-check" :style="{ minHeight: 'auto', flexShrink: 0 }">
            <input v-model="fe.enabled" type="checkbox" :aria-label="`启用前置 ${index + 1}`">
            <span class="ds-check__box"></span>
          </label>
          <div class="ds-input td-form__frontend-address">
            <input
              :id="fieldId(`frontend-address-${index}`)"
              v-model="fe.address"
              type="text"
              placeholder="tcp://180.168.146.187:10131"
              :aria-label="`前置地址 ${index + 1}`"
              autocomplete="off"
            >
          </div>
          <div class="ds-input td-form__frontend-label">
            <input
              :id="fieldId(`frontend-label-${index}`)"
              v-model="fe.label"
              type="text"
              placeholder="标签（选填）"
              :aria-label="`前置标签 ${index + 1}`"
              autocomplete="off"
            >
          </div>
          <button
            class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon"
            type="button"
            :aria-label="`删除前置 ${index + 1}`"
            data-test="td-remove-frontend"
            @click="removeFrontend(index)"
          >
            <Icon name="Delete" :size="14" />
          </button>
        </div>
        <span v-if="hasFrontendWarning" class="dialog-field__hint td-form__hint--warn" data-test="td-frontend-warning">
          没有「已启用且地址非空」的前置：保存成功也无法连接（网关会拒绝连接并弹窗提示）
        </span>
      </template>

      <!-- 未知接口类型的兜底：不渲染任何柜台字段（契约 td-config §1：payload 不带类型字段，未知字段忽略） -->
      <div v-else-if="form.gateway !== ''" class="ds-alert ds-alert--info" data-test="td-unsupported-gateway">
        <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
        <span>该网关的接口类型暂不支持在页面上配置连接参数，仅可维护通用层字段</span>
      </div>

      <div v-if="errorText" class="ds-alert ds-alert--danger" role="alert" data-test="td-form-error">
        <span class="ds-alert__icon"><Icon name="Error" :size="16" /></span>
        <span>{{ errorText }}</span>
      </div>
      <div v-if="validationText" class="dialog-field__hint td-form__hint--warn" role="status" data-test="td-form-validation">
        {{ validationText }}
      </div>
    </div>
    <template #footer>
      <button class="ds-btn ds-btn--secondary" type="button" :disabled="submitting" @click="emit('close')">取消</button>
      <button
        class="ds-btn ds-btn--primary"
        type="button"
        data-test="td-form-submit"
        :disabled="!canSubmit"
        @click="submit"
      >
        <span v-if="submitting" class="ds-btn__spinner"></span>
        {{ submitting ? '提交中…' : (isEditing ? '保存' : '添加') }}
      </button>
    </template>
  </Modal>
</template>

<style scoped>
.td-form__row {
  display: flex;
  align-items: center;
  gap: var(--spacer-20);
  flex-wrap: wrap;
}

.td-form__grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--spacer-12);
}

.td-form__section-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  margin-top: var(--spacer-4);
  font-size: var(--body-sm-font-size);
  font-weight: var(--font-weight-medium);
  color: var(--text-secondary);
}

.td-form__frontend-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-form__frontend-address {
  flex: 2;
  min-width: 0;
}

.td-form__frontend-label {
  flex: 1;
  min-width: 0;
}

.td-form__hint--warn {
  color: var(--status-warning-default);
}

@media (max-width: 767.98px) {
  .td-form__grid {
    grid-template-columns: 1fr;
  }

  .td-form__frontend-row {
    flex-wrap: wrap;
  }
}
</style>
