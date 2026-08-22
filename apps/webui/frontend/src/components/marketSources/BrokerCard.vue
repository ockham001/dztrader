<script setup lang="ts">
import { ref, computed } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import FrontendTable from './FrontendTable.vue'
import { isStateIdle } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'
import type { BrokerEntry } from '@/types/api'

// BrokerCard — 单个经纪商条目: 头部(折叠行) + 展开后(前置地址表格 + 登录字段)
// 折叠状态由父级 CtpCard 持有(手风琴), 本组件通过 expanded prop + toggle 事件交互
const props = defineProps<{
  source: MarketSourceView
  broker: BrokerEntry
  expanded: boolean
}>()

const emit = defineEmits<{ toggle: [] }>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// ===== Broker radio change (非乐观强制回滚) =====
// 原生 radio 点击时浏览器立即置 DOM checked; 在非乐观契约下需立即改回镜像值
// (src.selectedBrokerId 由 WS md_rtn_config 镜像驱动, 仅 RTN 回推后才真正选中)。
// 断网/失败时镜像不变 → 手动回滚的 false 不被 Vue 覆盖(vnode false→false 不写 DOM) → 保持原样。
function onBrokerRadioChange(sourceId: number, brokerName: string, event: Event): void {
  const input = event.target as HTMLInputElement
  input.checked = src.value.selectedBrokerId === brokerName
  void store.selectBroker(sourceId, brokerName)
}

// ===== Delete broker confirmation modal (F-C1: 二次确认) =====
const brokerDeleteModalOpen = ref(false)
const brokerDeleteSourceId = ref<number | null>(null)
const brokerDeleteName = ref('')
const brokerDeleteSubmitting = ref(false)

function openBrokerDeleteModal(sourceId: number, brokerName: string): void {
  brokerDeleteSourceId.value = sourceId
  brokerDeleteName.value = brokerName
  brokerDeleteModalOpen.value = true
}

function closeBrokerDeleteModal(): void {
  brokerDeleteModalOpen.value = false
  brokerDeleteSourceId.value = null
  brokerDeleteName.value = ''
  brokerDeleteSubmitting.value = false
}

async function confirmDeleteBroker(): Promise<void> {
  if (!brokerDeleteSourceId.value || !brokerDeleteName.value) return
  if (brokerDeleteSubmitting.value) return  // 防重入
  brokerDeleteSubmitting.value = true
  try {
    await store.removeBroker(brokerDeleteSourceId.value, brokerDeleteName.value)
    closeBrokerDeleteModal()
  } catch {
    // 错误已由 store 设置; 保持 Modal 打开让用户看到 toast 并可重试
    brokerDeleteSubmitting.value = false
  }
}

// ===== Broker field blur handlers (F-C3: 失焦且值改变才下发) =====
function onBrokerFieldBlur(
  sourceId: number,
  broker: BrokerEntry,
  field: 'broker_id' | 'user_id' | 'password' | 'product_info',
  event: Event
): void {
  const newValue = (event.target as HTMLInputElement).value
  if (newValue === broker[field]) return  // 值未改变不下发
  const updatedBroker: BrokerEntry = { ...broker, [field]: newValue }
  void store.updateBroker(sourceId, broker.name, updatedBroker)
}
</script>

<template>
  <div class="broker-card">
    <div class="broker-card__header" @click="emit('toggle')">
      <!-- 当前选中 radio: 受控 checked + change 下发; click.stop 防触发折叠;
           title 与 FrontendTable 启用 checkbox 同款状态保护提示 -->
      <label
        class="ds-radio broker-card__radio"
        :class="{ 'is-pending': src.brokerSelectPending }"
        :style="{ minHeight: 'auto' }"
        :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
        @click.stop
      >
        <input
          type="radio"
          :name="`current-broker-${src.id}`"
          :checked="broker.name === src.selectedBrokerId"
          :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerSelectPending"
          :aria-label="`设为当前选中：${broker.name}`"
          @change="onBrokerRadioChange(src.id, broker.name, $event)"
        >
        <span class="ds-radio__dot"></span>
      </label>
      <span class="broker-card__name">{{ broker.name }}</span>
      <span v-if="!expanded" class="broker-card__summary">{{ broker.frontends.length }} 个前置</span>
      <!-- F-C1: 删除经纪商按钮 + 二次确认; F-C4: 受状态保护 -->
      <button
        class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--danger-subtle"
        type="button"
        :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerRemovePending"
        @click.stop="openBrokerDeleteModal(src.id, broker.name)"
      >
        {{ src.brokerRemovePending ? '删除中…' : '删除经纪商' }}
      </button>
    </div>

    <div v-if="expanded" class="broker-card__body">
      <!-- 前置地址列表 -->
      <FrontendTable :source="src" :broker="broker" />

      <!-- 登录字段 (per broker) — F-C3: 失焦且值改变才下发; F-C4: 受状态保护 -->
      <div class="card-subsection">
        <div class="card-subsection__row">
          <span class="card-subsection__title">登录字段</span>
          <span v-if="src.brokerFieldEditPending" class="card-subsection__title">保存中…</span>
        </div>
        <div class="login-fields">
          <div class="login-field">
            <label class="login-field__label">BrokerID</label>
            <div class="ds-input">
              <input
                type="text"
                :value="broker.broker_id"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerFieldEditPending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @blur="onBrokerFieldBlur(src.id, broker, 'broker_id', $event)"
              >
            </div>
          </div>
          <div class="login-field">
            <label class="login-field__label">UserID</label>
            <div class="ds-input">
              <input
                type="text"
                :value="broker.user_id"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerFieldEditPending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @blur="onBrokerFieldBlur(src.id, broker, 'user_id', $event)"
              >
            </div>
          </div>
          <div class="login-field">
            <label class="login-field__label">Password</label>
            <div class="ds-input">
              <input
                type="password"
                :value="broker.password"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerFieldEditPending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @blur="onBrokerFieldBlur(src.id, broker, 'password', $event)"
              >
            </div>
          </div>
          <div class="login-field">
            <label class="login-field__label">ProductInfo</label>
            <div class="ds-input">
              <input
                type="text"
                :value="broker.product_info"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerFieldEditPending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @blur="onBrokerFieldBlur(src.id, broker, 'product_info', $event)"
              >
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- ===== Delete Broker Confirmation Modal (F-C1: 二次确认) ===== -->
  <Modal :open="brokerDeleteModalOpen" title="确认删除经纪商" @close="closeBrokerDeleteModal">
    <div :style="{ color: 'var(--text-default)' }">
      确认删除经纪商 <code class="mono" :style="{ fontWeight: 500 }">{{ brokerDeleteName }}</code> 吗？
      <div :style="{ marginTop: 'var(--spacer-8)', color: 'var(--text-tertiary)', fontSize: 'var(--body-sm-font-size)' }">
        删除后该经纪商的所有前置地址和登录字段将一并移除。若删除的是当前选中经纪商，选中将被置空。
      </div>
    </div>
    <template #footer>
      <button class="ds-btn ds-btn--secondary" type="button" @click="closeBrokerDeleteModal">取消</button>
      <button class="ds-btn ds-btn--danger" type="button" :disabled="brokerDeleteSubmitting" @click="confirmDeleteBroker">
        <span v-if="brokerDeleteSubmitting" class="ds-btn__spinner"></span>
        {{ brokerDeleteSubmitting ? '删除中…' : '删除' }}
      </button>
    </template>
  </Modal>
</template>

<style scoped>
/* Per-broker card */
.broker-card {
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  padding: var(--spacer-12) var(--spacer-16);
  margin-top: var(--spacer-12);
  background: var(--bg-base-secondary);
}

.broker-card__header {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  margin-bottom: 0; /* 原间距移至 __body, 消除折叠态死空间 */
  cursor: pointer; /* 可点击切换展开（与 source-card__header 一致） */
}

.broker-card__body { margin-top: var(--spacer-8); }

.broker-card__summary { font-size: var(--body-xs-font-size); color: var(--text-tertiary); }

/* 当前选中 radio: 对齐头部行高, 避免撑高; disabled 态与 ds-check 一致(去掉 not-allowed 光标) */
.broker-card__radio { flex-shrink: 0; }
.broker-card__radio input:disabled + .ds-radio__dot { opacity: .5; }

.broker-card__name {
  font-size: var(--body-base-font-size);
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-default);
}

.broker-card__header .ds-btn {
  margin-left: auto;
}

/* Sub-section (前置地址, 登录字段) */
.card-subsection {
  margin-top: var(--spacer-16);
}

.card-subsection__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--spacer-8);
}

.card-subsection__title {
  font-size: var(--body-xs-font-size);
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-tertiary);
}

/* Login fields */
.login-fields {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-10);
  margin-top: var(--spacer-4);
}

.login-field {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
}

.login-field__label {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  font-weight: var(--font-weight-medium, 500);
  flex-shrink: 0;
  width: 90px;
}

.login-field .ds-input {
  flex: 1;
  min-width: 0;
  font-size: var(--body-base-font-size);
}

.login-field .ds-input input {
  font-family: var(--code-editor-font-family);
  font-variant-numeric: tabular-nums;
}
</style>