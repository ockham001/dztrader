<script setup lang="ts">
import { ref, computed } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import { isStateIdle } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'
import type { BrokerEntry } from '@/types/api'

// FrontendTable — 单个经纪商的前置地址表格 + 添加/删除前置 Modal
// 直接调 store (与现状 CtpCard 一致), props 传 source + broker
const props = defineProps<{
  source: MarketSourceView
  broker: BrokerEntry
}>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// ===== Add frontend modal (F-C10: address + label) =====
const frontendModalOpen = ref(false)
const frontendModalSourceId = ref<number | null>(null)
const frontendModalBrokerName = ref('')
const newFrontendAddress = ref('')
const newFrontendLabel = ref('')
const frontendSubmitting = ref(false)

function openFrontendModal(sourceId: number, brokerName: string): void {
  frontendModalSourceId.value = sourceId
  frontendModalBrokerName.value = brokerName
  newFrontendAddress.value = ''
  newFrontendLabel.value = ''
  frontendModalOpen.value = true
}

function closeFrontendModal(): void {
  frontendModalOpen.value = false
  frontendModalSourceId.value = null
  frontendModalBrokerName.value = ''
  newFrontendAddress.value = ''
  newFrontendLabel.value = ''
  frontendSubmitting.value = false
}

/// F-C10: try/catch 包裹, 失败时保持 Modal 打开
async function confirmAddFrontend(): Promise<void> {
  if (!frontendModalSourceId.value || !frontendModalBrokerName.value || !newFrontendAddress.value.trim()) return
  frontendSubmitting.value = true
  try {
    await store.addFrontend(
      frontendModalSourceId.value,
      frontendModalBrokerName.value,
      newFrontendAddress.value.trim(),
      newFrontendLabel.value.trim(),
    )
    closeFrontendModal()
  } catch {
    frontendSubmitting.value = false
  }
}

// ===== Delete frontend confirmation modal (F-C2: 二次确认) =====
const frontendDeleteModalOpen = ref(false)
const frontendDeleteSourceId = ref<number | null>(null)
const frontendDeleteBrokerName = ref('')
const frontendDeleteAddress = ref('')

function openFrontendDeleteModal(sourceId: number, brokerName: string, address: string): void {
  frontendDeleteSourceId.value = sourceId
  frontendDeleteBrokerName.value = brokerName
  frontendDeleteAddress.value = address
  frontendDeleteModalOpen.value = true
}

function closeFrontendDeleteModal(): void {
  frontendDeleteModalOpen.value = false
  frontendDeleteSourceId.value = null
  frontendDeleteBrokerName.value = ''
  frontendDeleteAddress.value = ''
}

async function confirmDeleteFrontend(): Promise<void> {
  if (!frontendDeleteSourceId.value || !frontendDeleteBrokerName.value || !frontendDeleteAddress.value) return
  try {
    await store.removeFrontend(frontendDeleteSourceId.value, frontendDeleteBrokerName.value, frontendDeleteAddress.value)
    closeFrontendDeleteModal()
  } catch {
    // 错误已由 store 设置; 保持 Modal 打开让用户看到 toast 并可重试
  }
}

// ===== Frontend address blur handler (F-C2: 失焦且值改变才下发) =====
function onFrontendAddressBlur(
  sourceId: number,
  brokerName: string,
  oldAddress: string,
  event: Event
): void {
  const newAddress = (event.target as HTMLInputElement).value
  if (newAddress === oldAddress) return  // 值未改变不下发
  void store.editFrontend(sourceId, brokerName, oldAddress, newAddress)
}
</script>

<template>
  <div class="card-subsection">
    <div class="card-subsection__row">
      <span class="card-subsection__title">前置地址</span>
      <!-- F-C4: 添加前置受状态保护 (前置列表增删改切换属连接参数变更) -->
      <button
        class="ds-btn ds-btn--tertiary ds-btn--sm"
        type="button"
        :disabled="!isStateIdle(src.process_state, src.loginState) || src.frontendAddPending"
        @click="openFrontendModal(src.id, broker.name)"
      >
        {{ src.frontendAddPending ? '添加中…' : '添加前置' }}
      </button>
    </div>
    <div class="ds-table-card front-table-wrap">
      <table class="ds-table">
        <thead>
          <tr>
            <th :style="{ width: '60px' }">启用</th>
            <th>地址</th>
            <th :style="{ width: '120px' }">标签</th>
            <th :style="{ width: '90px', textAlign: 'right' }">操作</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="fe in broker.frontends" :key="fe.address">
            <!-- enabled 为 checkbox 语义, 多个前置可同时启用, CTP 自动故障切换 -->
            <td>
              <input
                type="checkbox"
                :checked="fe.enabled"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.frontendTogglePending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @change="store.setFrontendEnabled(src.id, broker.name, fe.address, ($event.target as HTMLInputElement).checked)"
              >
            </td>
            <!-- F-C2: 地址可编辑, 失焦且值改变才下发 -->
            <td>
              <input
                class="fe-address-input"
                type="text"
                :value="fe.address"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.frontendEditPending"
                :title="!isStateIdle(src.process_state, src.loginState) ? '需在未登录状态下操作' : ''"
                @blur="onFrontendAddressBlur(src.id, broker.name, fe.address, $event)"
              >
            </td>
            <td><code class="mono" :style="{ fontSize: 'var(--body-sm-font-size)' }">{{ fe.label || '--' }}</code></td>
            <td :style="{ textAlign: 'right' }">
              <button
                class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--danger-subtle"
                type="button"
                :disabled="!isStateIdle(src.process_state, src.loginState) || src.frontendRemovePending"
                @click="openFrontendDeleteModal(src.id, broker.name, fe.address)"
              >
                {{ src.frontendRemovePending ? '删除中…' : '删除' }}
              </button>
            </td>
          </tr>
          <tr v-if="broker.frontends.length === 0">
            <td colspan="4" :style="{ textAlign: 'center', color: 'var(--text-tertiary)', padding: 'var(--spacer-16)' }">暂无前置地址</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>

  <!-- ===== Add Frontend Modal (F-C10: address + label) ===== -->
  <Modal :open="frontendModalOpen" title="添加前置地址" @close="closeFrontendModal">
    <div class="dialog-form">
      <div class="dialog-row">
        <label class="dialog-row__label">地址</label>
        <div class="ds-input dialog-row__control">
          <input v-model="newFrontendAddress" type="text" placeholder="例如：tcp://180.168.146.187:10211">
        </div>
      </div>
      <div class="dialog-row">
        <label class="dialog-row__label">标签</label>
        <div class="ds-input dialog-row__control">
          <input v-model="newFrontendLabel" type="text" placeholder="例如：电信线路（可选）">
        </div>
      </div>
    </div>
    <template #footer>
      <button class="ds-btn ds-btn--secondary" type="button" @click="closeFrontendModal">取消</button>
      <button class="ds-btn ds-btn--primary" type="button"
        :disabled="frontendSubmitting || !newFrontendAddress.trim()"
        @click="confirmAddFrontend">
        <span v-if="frontendSubmitting" class="ds-btn__spinner"></span>
        {{ frontendSubmitting ? '添加中…' : '添加' }}
      </button>
    </template>
  </Modal>

  <!-- ===== Delete Frontend Confirmation Modal (F-C2: 二次确认) ===== -->
  <Modal :open="frontendDeleteModalOpen" title="确认删除前置地址" @close="closeFrontendDeleteModal">
    <div :style="{ color: 'var(--text-default)' }">
      确认删除前置地址 <code class="mono" :style="{ fontWeight: 500 }">{{ frontendDeleteAddress }}</code> 吗？
    </div>
    <template #footer>
      <button class="ds-btn ds-btn--secondary" type="button" @click="closeFrontendDeleteModal">取消</button>
      <button class="ds-btn ds-btn--danger" type="button" @click="confirmDeleteFrontend">删除</button>
    </template>
  </Modal>
</template>

<style scoped>
/* Sub-section */
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

/* Frontend table wrapper */
.front-table-wrap .ds-table {
  min-width: 560px;
}

/* Frontend address inline-edit input */
.fe-address-input {
  width: 100%;
  border: 1px solid transparent;
  background: transparent;
  padding: var(--spacer-2) var(--spacer-6);
  border-radius: var(--radius-4);
  font-family: var(--code-editor-font-family);
  font-variant-numeric: tabular-nums;
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  transition: border-color 0.1s ease, background 0.1s ease;
}

.fe-address-input:disabled {
  cursor: not-allowed;
  opacity: 0.7;
}

.fe-address-input:hover:not(:disabled) {
  border-color: var(--border-neutral-l2);
}

.fe-address-input:focus:not(:disabled) {
  border-color: var(--border-focus-l1, var(--status-success-default));
  background: var(--bg-base-primary);
  outline: none;
}

/* Dialog form */
.dialog-form {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
}

.dialog-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
}

.dialog-row__label {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  font-weight: var(--font-weight-medium, 500);
  flex-shrink: 0;
  width: 110px;
  white-space: nowrap;
}

.dialog-row__control {
  flex: 1;
  min-width: 0;
}

/* Spinner for buttons */
.ds-btn__spinner {
  display: inline-block;
  width: 12px;
  height: 12px;
  border: 1.5px solid currentcolor;
  border-top-color: transparent;
  border-radius: 50%;
  animation: ds-btn-spin 0.6s linear infinite;
  opacity: 0.7;
  flex-shrink: 0;
}

@keyframes ds-btn-spin {
  to { transform: rotate(360deg); }
}
</style>