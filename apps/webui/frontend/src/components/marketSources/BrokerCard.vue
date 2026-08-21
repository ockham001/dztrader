<script setup lang="ts">
import { ref, computed } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import FrontendTable from './FrontendTable.vue'
import { isStateIdle } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'
import type { BrokerEntry } from '@/types/api'

// BrokerCard — 单个经纪商卡片: 头部(名称/当前徽标/删除) + 前置地址表格 + 登录字段
// 直接调 store (与现状 CtpCard 一致), props 传 source + broker
const props = defineProps<{
  source: MarketSourceView
  broker: BrokerEntry
}>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// ===== Delete broker confirmation modal (F-C1: 二次确认) =====
const brokerDeleteModalOpen = ref(false)
const brokerDeleteSourceId = ref<number | null>(null)
const brokerDeleteName = ref('')

function openBrokerDeleteModal(sourceId: number, brokerName: string): void {
  brokerDeleteSourceId.value = sourceId
  brokerDeleteName.value = brokerName
  brokerDeleteModalOpen.value = true
}

function closeBrokerDeleteModal(): void {
  brokerDeleteModalOpen.value = false
  brokerDeleteSourceId.value = null
  brokerDeleteName.value = ''
}

async function confirmDeleteBroker(): Promise<void> {
  if (!brokerDeleteSourceId.value || !brokerDeleteName.value) return
  try {
    await store.removeBroker(brokerDeleteSourceId.value, brokerDeleteName.value)
    closeBrokerDeleteModal()
  } catch {
    // 错误已由 store 设置; 保持 Modal 打开让用户看到 toast 并可重试
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
    <div class="broker-card__header">
      <span class="broker-card__name">{{ broker.name }}</span>
      <span v-if="broker.name === src.selectedBrokerId" class="broker-card__badge">当前</span>
      <!-- F-C1: 删除经纪商按钮 + 二次确认; F-C4: 受状态保护 -->
      <button
        class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--danger-subtle"
        type="button"
        :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerRemovePending"
        @click="openBrokerDeleteModal(src.id, broker.name)"
      >
        {{ src.brokerRemovePending ? '删除中…' : '删除经纪商' }}
      </button>
    </div>

    <div class="broker-card__body">
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
      <button class="ds-btn ds-btn--danger" type="button" @click="confirmDeleteBroker">删除</button>
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
  margin-bottom: var(--spacer-8);
}

.broker-card__name {
  font-size: var(--body-base-font-size);
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-default);
}

.broker-card__badge {
  display: inline-block;
  padding: 0 var(--spacer-6);
  font-size: var(--body-xs-font-size);
  color: var(--status-success-default);
  background: var(--status-success-surface-l1);
  border-radius: var(--radius-4);
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