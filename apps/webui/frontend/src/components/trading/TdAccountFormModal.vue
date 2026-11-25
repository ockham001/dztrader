<script setup lang="ts">
/**
 * 交易账户表单（添加 / 编辑共用，契约 td-config §2.2/§2.3，契约 rest §2.9.7）。
 *
 * 为什么添加与编辑共用一个组件：两者字段集几乎相同，差别只有两处契约语义——
 * - **`account_id` 是不可变 key**（契约 td-config §2.1）：编辑时只读展示，不随请求提交；
 *   它是**柜台真实账户号**（ADR 0056 决策 1：CTP 的 `InvestorID` 与 `UserID` 都取它）；
 * - **密码是「缺失/空串/`****` = 保留旧值」**（契约 td-config §2.3）：编辑时留空即不修改，
 *   占位提示 `****`（回报恒脱敏），前端**从不**回填明文（镜像里的 password 恒为 `"****"`）。
 *
 * 身份字段只有「账户 ID」+「账户名称」两个（`account_name` 选填、可中文、可改，ADR 0056
 * 决策 3）：`ext.user_id`（旧「用户代码」）已删除——同一件事有两个名字正是 #467 要消灭的歧义。
 *
 * **所属交易接口由卡片传入（`gateway` prop）**，不再有网关下拉（ADR 0053 / #439）：
 * 账户是接口内部配置项，归属由所在卡片固定，用户没有"把账户配到别的接口"这个动作。
 *
 * 字段按接口类型选择（契约 rest §4 前端义务）：本期只有 `ctp`，`ext` 字段集由
 * {@link CTP_FIELD_KEYS} 单一登记；未来新增接口类型时在这里按 interfaceType 分派，
 * 通用层字段（account_id/enabled/risk_control_enabled/currency_id）不动。
 *
 * 组件只负责**收集与校验**，不发起请求、不碰 store：提交由父组件（交易接口卡片）调 store（单一写入点）。
 */
import { computed, ref, watch } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import Icon from '@/components/shared/Icon.vue'
import { useFieldId } from '@/composables/useFieldId'
import type { TdAccountRow } from '@/stores/tdAccounts'

export interface TdAccountFormValue {
  account_id: string
  /** 展示名（选填；空串 = 未设置，界面回落 account_id，ADR 0056 决策 3） */
  account_name: string
  gateway: string
  enabled: boolean
  risk_control_enabled: boolean
  currency_id: string
  /** CTP ext（契约 td-config §2.3）；CTP 的 UserID/InvestorID 都取 account_id，无独立字段（ADR 0056） */
  broker_id: string
  /** 空串 = 不修改（编辑）/ 未填（添加）；永远不会是 `****`（那表示保留旧值） */
  password: string
  product_info: string
  frontends: { address: string; label: string; enabled: boolean }[]
  auth_code: string
  app_id: string
}

const props = defineProps<{
  open: boolean
  /** null = 添加；非 null = 编辑该行 */
  editing: TdAccountRow | null
  /** 编辑行的展示名**原值**（未设置 = 空串，不回落 id；由父组件经 store.accountName 取） */
  editingAccountName?: string
  /** 所属交易接口进程名（所在卡片固定；不可在表单内修改） */
  gateway: string
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

/** 空白表单（添加态的初值：默认启用、无前置）。
 *  `currency_id` 不在表单暴露：币种应由账户/柜台回报决定（见模板注释），协议字段暂按契约
 *  默认 CNY 提交，等「柜台回报推导币种」落地后改由网关填充。 */
function emptyForm(gateway: string): TdAccountFormValue {
  return {
    account_id: '',
    account_name: '',
    gateway,
    enabled: true,
    risk_control_enabled: false,
    currency_id: 'CNY',
    broker_id: '',
    password: '',
    product_info: '',
    frontends: [],
    auth_code: '',
    app_id: '',
  }
}

const form = ref<TdAccountFormValue>(emptyForm(''))

/**
 * 用行数据回填编辑表单。
 *
 * 密码**不回填**：配置镜像的 `ext.password` 恒为 `"****"`（契约 td-config §2.3/§4 脱敏），
 * 回填它只会诱导用户把掩码当密码提交；留空并在占位里说明「留空 = 不修改」。
 *
 * `account_name` 取父组件传入的**原值**（不是回落 id 的展示名）：留空 = 未设置，回填 id
 * 会让一次无意保存把「没起名字」变成「名字 = id」。
 */
function formFromRow(row: TdAccountRow): TdAccountFormValue {
  const base = emptyForm(row.gateway)
  const config = row.config
  const ext = config?.ext
  return {
    ...base,
    account_id: row.account_id,
    account_name: props.editingAccountName ?? '',
    enabled: config?.enabled ?? row.enabled,
    risk_control_enabled: config?.risk_control_enabled ?? row.riskControlEnabled,
    currency_id: config?.currency_id ?? row.currencyId,
    broker_id: ext?.broker_id ?? '',
    password: '',
    product_info: ext?.product_info ?? '',
    frontends: (ext?.frontends ?? []).map(fe => ({
      address: fe.address,
      label: fe.label ?? '',
      enabled: fe.enabled !== false,
    })),
    auth_code: ext?.auth_code ?? '',
    app_id: ext?.app_id ?? '',
  }
}

// 打开/切换编辑目标时重建表单（关闭时保留最后一次输入，避免误关后重填；父组件负责重置）
watch(() => [props.open, props.editing], () => {
  if (!props.open) return
  if (props.editing) {
    form.value = formFromRow(props.editing)
    return
  }
  form.value = emptyForm(props.gateway)
}, { immediate: true })

const isEditing = computed(() => props.editing !== null)

/**
 * 编辑弹窗标题里的账户标识：优先展示名（原值 trim），未设置回落账户 ID（ADR 0056 决策 3）。
 * 标题用**已保存的**名字（`editingAccountName`），不跟表单输入实时变化，避免打字时标题抖动。
 */
const editingTitle = computed(() => {
  const editing = props.editing
  if (editing === null) return ''
  const name = (props.editingAccountName ?? '').trim()
  return name === '' ? editing.account_id : name
})
const isCtp = computed(() => {
  const gateway = form.value.gateway
  if (!gateway.startsWith('dztd_')) return false
  const tail = gateway.slice('dztd_'.length)
  const underscore = tail.indexOf('_')
  return (underscore < 0 ? tail : tail.slice(0, underscore)) === 'ctp'
})

/** account_id 长度上限：`DzAccountId` 容量 31 字节（契约 rest §2.9.7 预检 4 / ADR 0056 决策 6） */
const ACCOUNT_ID_MAX_BYTES = 31
/** account_name trim 后字节上限（契约 td-config §2.2 / §7） */
const ACCOUNT_NAME_MAX_BYTES = 64
/** account_id 字符集：ASCII 可见字符 `0x21`–`0x7E`，不含 `:`、`/`、`\`、`.`（契约 td-config
 *  §2.2 / §7、ADR 0057：后三者会破坏网关按 account_id 构造的 CTP 流文件目录） */
const ACCOUNT_ID_CHARSET = /^[\x21-\x2D\x30-\x39\x3B-\x5B\x5D-\x7E]*$/
/** UTF-8 字节数：长度上限是**字节**不是字符（中文名 3 字节/字），必须按字节拦 */
function utf8Bytes(text: string): number {
  return new TextEncoder().encode(text).length
}

/**
 * 是否含控制字符（契约 td-config §2.2）：ASCII < 0x20（含 `\t\r\n`）、DEL 0x7F、
 * C1 段 U+0080–U+009F。按码点判而不写控制字符正则：后者在 eslint 的 `no-control-regex`
 * 下必须豁免，且逐段写清楚「哪些码点非法」比正则更贴近契约原文。
 */
function hasControlChars(text: string): boolean {
  for (const char of text) {
    const code = char.codePointAt(0) ?? 0
    if (code < 0x20 || code === 0x7f || (code >= 0x80 && code <= 0x9f)) return true
  }
  return false
}

/** `account_id` 按 trim 后校验（服务端以路径/配置值为准，提交时也 trim） */
const accountIdTrimmed = computed(() => form.value.account_id.trim())
const accountIdBytes = computed<number>(() => utf8Bytes(accountIdTrimmed.value))
const accountIdTooLong = computed(() => accountIdBytes.value > ACCOUNT_ID_MAX_BYTES)
const accountIdEmpty = computed(() => accountIdTrimmed.value === '')
const accountIdCharsetInvalid = computed(() => !ACCOUNT_ID_CHARSET.test(accountIdTrimmed.value))

/**
 * `account_name` 按 trim 后校验（契约 §2.2「trim 后 ≤64 字节、无控制字符」）：
 * trim 掉的 ASCII 空白里虽有控制字符（`\t\r\n`），但服务端也是 trim 后判定，口径保持一致。
 */
const accountNameTrimmed = computed(() => form.value.account_name.trim())
const accountNameBytes = computed<number>(() => utf8Bytes(accountNameTrimmed.value))
const accountNameTooLong = computed(() => accountNameBytes.value > ACCOUNT_NAME_MAX_BYTES)
const accountNameControlChar = computed(() => hasControlChars(accountNameTrimmed.value))

/**
 * 表单本地校验（提交前置；服务端仍是权威校验点）。
 *
 * 这里只挡**字段自身**可判定的错误（契约 rest §2.9.7 预检 4 的通用层规则）：字符集与长度。
 * 跨账户唯一性（`account_exists` / `account_name_exists`）、进程名冲突、NFC 规范化后的比较
 * 都在服务端裁决——前端不重复实现一套可能漂移的规则。
 */
const validationText = computed<string>(() => {
  if (!isEditing.value && accountIdEmpty.value) return '账户 ID 不能为空'
  if (accountIdCharsetInvalid.value) return '账户 ID 只能使用可见 ASCII 字符（不含空格、冒号、点号与路径分隔符「/」「\\」）'
  if (accountIdTooLong.value) return `账户 ID 不能超过 ${ACCOUNT_ID_MAX_BYTES} 字节（当前 ${accountIdBytes.value}）`
  if (accountNameControlChar.value) return '账户名称不能包含控制字符'
  if (accountNameTooLong.value) return `账户名称不能超过 ${ACCOUNT_NAME_MAX_BYTES} 字节（当前 ${accountNameBytes.value}）`
  if (isCtp.value) {
    if (form.value.broker_id.trim() === '') return 'CTP 经纪公司代码（broker_id）不能为空'
    // 密码必填只在添加态：编辑态留空 = 保留旧值（契约 td-config §2.3）
    if (!isEditing.value && form.value.password === '') return 'CTP 登录密码不能为空'
  }
  return ''
})

const canSubmit = computed(() => validationText.value === '' && !props.submitting)

function submit(): void {
  if (!canSubmit.value) return
  const raw = form.value
  // 规范化：字符串去首尾空白；未启用的空地址前置丢弃（契约 td-config §7：address 非空才注册）。
  // `account_name` 的 trim 只是表单收集口径，唯一性比较的 NFC 规范化仍在服务端。
  emit('submit', {
    ...raw,
    account_id: raw.account_id.trim(),
    account_name: raw.account_name.trim(),
    broker_id: raw.broker_id.trim(),
    product_info: raw.product_info.trim(),
    auth_code: raw.auth_code.trim(),
    app_id: raw.app_id.trim(),
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
    :title="isEditing ? `编辑交易账户：${editingTitle}` : '添加交易账户'"
    @close="emit('close')"
  >
    <div class="td-form dialog-form">
      <div class="dialog-field">
        <span class="dialog-field__label">所属交易接口</span>
        <span class="td-form__gateway" data-test="td-form-gateway">{{ gateway }}</span>
        <span class="dialog-field__hint">账户归属于所在交易接口，不可在此修改</span>
      </div>

      <div class="dialog-field">
        <label class="dialog-field__label" :for="fieldId('account-id')">账户 ID</label>
        <div class="ds-input">
          <input
            :id="fieldId('account-id')"
            v-model="form.account_id"
            type="text"
            data-test="td-form-account-id"
            :readonly="isEditing"
            :placeholder="isEditing ? '' : '柜台真实账户号（创建后不可修改）'"
            autocomplete="off"
          >
        </div>
        <span
          class="dialog-field__hint"
          :class="{ 'td-form__hint--warn': accountIdTooLong || accountIdCharsetInvalid }"
        >
          <template v-if="isEditing">账户 ID 是柜台真实账户号，创建后不可修改</template>
          <template v-else>
            柜台真实账户号，创建后不可修改；全平台唯一，仅限可见 ASCII 字符（不含空格、冒号、点号与路径分隔符），最多 {{ ACCOUNT_ID_MAX_BYTES }} 字节
            · 当前 {{ accountIdBytes }}/{{ ACCOUNT_ID_MAX_BYTES }}
          </template>
          <template v-if="isCtp">；CTP 账户号另有 12 字节上限（超限由网关报错，平台不会截断）</template>
        </span>
      </div>

      <div class="dialog-field">
        <label class="dialog-field__label" :for="fieldId('account-name')">账户名称</label>
        <div class="ds-input">
          <input
            :id="fieldId('account-name')"
            v-model="form.account_name"
            type="text"
            data-test="td-form-account-name"
            placeholder="选填，例如：主账户"
            autocomplete="off"
          >
        </div>
        <span
          class="dialog-field__hint"
          :class="{ 'td-form__hint--warn': accountNameTooLong || accountNameControlChar }"
        >
          留空则显示账户 ID；可中文；全平台唯一（与账户 ID 及其它账户名称不冲突）；可随时修改
          · 当前 {{ accountNameBytes }}/{{ ACCOUNT_NAME_MAX_BYTES }} 字节
        </span>
      </div>

      <div class="td-form__row">
        <label class="ds-check td-form__check">
          <input v-model="form.enabled" type="checkbox">
          <span class="ds-check__box"></span> 启用该账户
        </label>
        <label class="ds-check td-form__check">
          <input v-model="form.risk_control_enabled" type="checkbox">
          <span class="ds-check__box"></span> 账户级风控
        </label>
      </div>

      <!-- 账户币种：不做用户配置。币种应由账户/柜台给出（CTP 资金账户查询回报自带
           CurrencyID），平台侧人工填写没有消费者且会引入错配；协议字段 currency_id
           暂保留（契约 td-config §2.2 通用层，默认 CNY），推导落地前由网关按 CNY 处理。 -->

      <!-- ===== CTP ext（interface_type = ctp，契约 td-config §2.3） ===== -->
      <template v-if="isCtp">
        <div class="td-form__section-title">CTP 连接参数</div>

        <!-- 账户 ID 已承载 CTP 的 UserID/InvestorID（ADR 0056 决策 2），不再有「用户代码」字段 -->
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('ctp-broker-id')">经纪公司代码</label>
          <div class="ds-input">
            <input :id="fieldId('ctp-broker-id')" v-model="form.broker_id" type="text" placeholder="broker_id" autocomplete="off">
          </div>
          <span class="dialog-field__hint">CTP 的登录账号与投资者代码都取上面的「账户 ID」，无需另填</span>
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

        <!-- 流文件目录不在表单提供：留空/缺失 = 网关自动构造（契约 td-config §2.3），
             用户不需要也不应该管理网关的本地文件布局。 -->

        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('ctp-product-info')">产品信息</label>
          <div class="ds-input">
            <input :id="fieldId('ctp-product-info')" v-model="form.product_info" type="text" placeholder="选填" autocomplete="off">
          </div>
        </div>

        <div class="td-form__grid">
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-app-id')">AppID</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-app-id')" v-model="form.app_id" type="text" placeholder="选填" autocomplete="off">
            </div>
          </div>
          <div class="dialog-field">
            <label class="dialog-field__label" :for="fieldId('ctp-auth-code')">认证码</label>
            <div class="ds-input">
              <input :id="fieldId('ctp-auth-code')" v-model="form.auth_code" type="text" placeholder="选填" autocomplete="off">
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
          <label class="ds-check td-form__check td-form__check--fixed">
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
/* 最小 layout 修复（2026-11-23 验收发现）：全局 `.dialog-field` 是为「label 80px 右对齐 +
   输入框同行 + hint 同行」的窄单行弹窗设计的；本表单标签长（经纪公司代码/账户 ID）、
   字段多、还有 hint，沿用同一行会把标签挤成逐字换行、把输入框挤扁。这里把字段改成纵向：
   label 在上、输入框独占一行、hint 在下。仅作用于本弹窗（scoped），不改全局组件。 */
.td-form .dialog-field {
  flex-direction: column;
  align-items: stretch;
  gap: var(--spacer-4);
}

.td-form .dialog-field__label {
  width: auto;
  text-align: left;
}

/* 纵向排列后不再让输入框按 flex 主轴纵向拉伸，宽度撑满字段列；
   select/textarea 与 input 同规则（当前表单未用，保持与全局 .dialog-field 子项对称）。 */
.td-form .dialog-field > .ds-input,
.td-form .dialog-field > .ds-select-wrap,
.td-form .dialog-field > .ds-textarea {
  flex: none;
  width: 100%;
}

.td-form .dialog-field__hint {
  margin-top: var(--spacer-4);
}

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

/* 所属交易接口：只读展示（不再是下拉，ADR 0053） */
.td-form__gateway {
  font-size: var(--body-base-font-size);
  font-variant-numeric: tabular-nums;
  color: var(--text-default);
}

/* #346：原模板内联 minHeight: 'auto' 归位（压住 .ds-check 的 min-height: 40px）；
   --fixed 对应原内联 flexShrink: 0（仅前置行内的勾选标签需要，其他两处保持可收缩）。 */
.td-form__check {
  min-height: auto;
}

.td-form__check--fixed {
  flex-shrink: 0;
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
