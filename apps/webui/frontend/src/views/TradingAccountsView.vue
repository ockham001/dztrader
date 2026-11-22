<script setup lang="ts">
/**
 * 交易账户页（#436 / #439；ADR 0053；契约 rest §2.9.7/§2.9.9/§2.10/§4）。
 *
 * 页面口径（全部来自契约，不是自由设计）：
 * - **页面 = 交易接口卡片列表**（导航与页头文案保持「交易账户」）：每张卡片是一个
 *   `dztd_*` 交易接口进程 + 其内部账户表。卡片来源 = WS `process_config` / `process_status`
 *   镜像（契约 rest §4，不新增列表端点）；卡片内账户 = 账户注册表按网关分组。
 * - **页面级只有 1 个入口**「添加交易接口」（Q6 最小按钮集）：`GET /api/td/gateways/available`
 *   下拉 + 显示名弹窗 + 添加即 `start`（动态注册并启动），与行情源页同构（契约 rest §2.10）。
 * - 接口生命周期写端点（start/stop/remove）与账户写端点的 pending 都以 WS 生效信号清除，
 *   HTTP 200 只表示已写入事件通道（契约 rest §2.10/§1）。
 * - **账户列表 = 注册表视角**（`GET /api/td/accounts`，契约 rest §2.9.9）：注册表
 *   `available:false` 时展示降级提示，**绝不**把列表当空；降级期间各网关已推来的配置账户
 *   照常展示（契约 rest §2.9.9 约束）。
 * - 非 admin 只读：不渲染接口级操作（添加/启动/删除），账户写按钮在卡片内禁用并给出原因
 *   （契约 rest §2.9.7 预检 1 / §2.10 预检 1）。
 */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import SyncSelect from '@/components/shared/SyncSelect.vue'
import TdInterfaceCard from '@/components/trading/TdInterfaceCard.vue'
import { useFieldId } from '@/composables/useFieldId'
import { useTdAccountsStore } from '@/stores/tdAccounts'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import type { SelectItem } from '@/components/shared/SyncSelect.vue'

const store = useTdAccountsStore()
const auth = useAuthStore()
const toast = useToastStore()

// label↔input 配对（#233）：id 按组件实例前缀生成
const fieldId = useFieldId()

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

/** 接口级写操作权限：契约 rest §2.10 预检 1——admin；非 admin 不渲染接口级按钮 */
const canWrite = computed(() => auth.isAdmin)
const READ_ONLY_HINT = '仅管理员可添加/移除交易接口'

// ===== 卡片列表与页头聚合 =====
const interfaces = computed(() => store.interfaces)
const interfaceCount = computed(() => interfaces.value.length)
const accountCount = computed(
  () => interfaces.value.reduce((sum, item) => sum + item.accountCount, 0))
const showEmpty = computed(() => interfaces.value.length === 0)

// ===== 添加交易接口（与行情源页同构：SyncSelect + 显示名弹窗 + 添加即 start）=====
// modelValue 始终保持空字符串：每次选择都触发添加弹窗，完成后回到 placeholder
const addSelected = ref<string>('')

/** 可用接口下拉项：已添加的标注「已添加」（选择被忽略，避免重复添加） */
const availableItems = computed<SelectItem[]>(() => store.gatewayAvailable.map(item => {
  const label = item.display_name || item.name
  return { value: item.name, label: item.added ? `${label} (已添加)` : label }
}))

const addPlaceholder = computed(() => (
  store.gatewayAvailableLoading ? '加载中…' : '添加交易接口'))

/** 下拉打开时刷新后端真实可用列表（扫描 App Root 的 dztd_* exe + 进程配置镜像） */
async function onAddOpen(): Promise<void> {
  await store.refreshAvailableGateways()
}

/** 选择某项 → 打开显示名弹窗；已添加的项直接忽略（恢复走卡片上的「启动进程」） */
function onAddChange(value: string | number): void {
  const name = String(value)
  const available = store.gatewayAvailable.find(item => item.name === name)
  addSelected.value = ''
  if (!available || available.added) return
  addTargetName.value = name
  addTargetDisplayName.value = available.display_name || name
  addError.value = ''
  addOpen.value = true
}

// ===== 添加弹窗（显示名）=====
const addOpen = ref(false)
const addTargetName = ref('')
const addTargetDisplayName = ref('')
const addSubmitting = ref(false)
const addError = ref('')

function closeAdd(): void {
  if (addSubmitting.value) return
  addOpen.value = false
  addTargetName.value = ''
  addTargetDisplayName.value = ''
  addError.value = ''
}

/**
 * 确认添加：`start` 即动态注册 + 启动（契约 rest §2.10）。
 * HTTP 200 只表示已写入事件通道——卡片在该接口的 `process_config` 到达后由 WS 镜像生成，
 * 因此成功后只关弹窗，不本地伪造卡片。
 */
async function confirmAdd(): Promise<void> {
  if (addTargetName.value === '' || addTargetDisplayName.value.trim() === '') return
  addSubmitting.value = true
  addError.value = ''
  try {
    const result = await store.addGateway(addTargetName.value, addTargetDisplayName.value)
    if (result.ok) {
      addOpen.value = false
      toast.success(`已提交添加请求：${addTargetName.value}`)
      void store.refreshAvailableGateways()
      return
    }
    if (result.kind === 'failed') {
      addError.value = result.message
      return
    }
    if (result.kind === 'busy') {
      addError.value = '该操作正在进行中，请稍候'
      return
    }
    if (result.kind === 'timeout') {
      addError.value = '添加请求已下发，但未在预期时间内收到进程配置回报，请稍后刷新确认'
    }
  } finally {
    addSubmitting.value = false
  }
}
</script>

<template>
  <div class="td-page">
    <div class="td-page__header">
      <div class="td-page__title-group">
        <h1 class="td-page__title">交易账户</h1>
        <span class="ds-tag td-page__count" data-test="td-interface-count">
          共 {{ interfaceCount }} 个交易接口 · {{ accountCount }} 个账户
        </span>
      </div>
      <div v-if="canWrite" class="td-page__actions">
        <SyncSelect
          :items="availableItems"
          :model-value="addSelected"
          :placeholder="addPlaceholder"
          :pending="store.gatewayAvailableLoading"
          data-test="td-add-interface"
          @open="onAddOpen"
          @change="onAddChange"
        >
          <template #triggerPrefix>
            <Icon name="Plus" :size="16" />
          </template>
        </SyncSelect>
      </div>
    </div>

    <!-- 降级提示（契约 rest §2.9.9：不得把降级当空列表） -->
    <div v-if="!store.available" class="ds-alert ds-alert--warning td-page__banner" role="status" data-test="td-accounts-degraded">
      <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
      <span>{{ store.degradedText }}（当前展示的是各网关最近一次上报的配置账户）</span>
    </div>
    <!-- 非 admin 只读提示 -->
    <div v-if="!canWrite" class="ds-alert ds-alert--info td-page__banner" role="status" data-test="td-accounts-readonly">
      <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
      <span>{{ READ_ONLY_HINT }}；你仍可查看交易接口与账户配置</span>
    </div>

    <!-- 空态：没有任何交易接口（卡片来自进程镜像，镜像到达前短暂为空属正常） -->
    <div v-if="showEmpty" class="td-page__empty" data-test="td-interfaces-empty">
      还没有添加任何交易接口
      <span class="td-page__empty-hint">
        <template v-if="canWrite">点击右上角「添加交易接口」创建第一个接口；接口可零账户运行</template>
        <template v-else>请联系管理员添加交易接口</template>
      </span>
    </div>

    <!-- 交易接口卡片列表 -->
    <div v-else class="td-page__cards">
      <TdInterfaceCard
        v-for="item in interfaces"
        :key="item.name"
        :td-interface="item"
      />
    </div>

    <!-- 添加交易接口弹窗（显示名） -->
    <Modal :open="addOpen" :title="`添加交易接口：${addTargetName}`" @close="closeAdd">
      <div class="dialog-form">
        <div class="dialog-row">
          <label class="dialog-row__label" :for="fieldId('add-interface-display-name')">显示名</label>
          <div class="ds-input dialog-row__control">
            <input
              :id="fieldId('add-interface-display-name')"
              v-model="addTargetDisplayName"
              type="text"
              placeholder="例如：CTP 交易接口"
            >
          </div>
        </div>
        <div class="dialog-row__hint">
          添加后将动态注册到 master 启动配置并立即启动；进程状态与卡片以 WS 回报为准
        </div>
        <div v-if="addError" class="ds-alert ds-alert--danger" role="alert" data-test="td-add-interface-error">
          <span class="ds-alert__icon"><Icon name="Error" :size="16" /></span>
          <span>{{ addError }}</span>
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" :disabled="addSubmitting" @click="closeAdd">取消</button>
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          data-test="td-add-interface-confirm"
          :disabled="addSubmitting || addTargetDisplayName.trim() === ''"
          @click="confirmAdd"
        >
          <span v-if="addSubmitting" class="ds-btn__spinner"></span>
          {{ addSubmitting ? '添加中…' : '添加' }}
        </button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.td-page {
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

.td-page__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
  flex-wrap: wrap;
}

.td-page__title-group {
  display: flex;
  align-items: center;
  gap: var(--spacer-16);
}

.td-page__title {
  margin: 0;
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
}

.td-page__count {
  font-variant-numeric: tabular-nums;
}

.td-page__actions {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-page__banner {
  margin-bottom: var(--spacer-16);
}

.td-page__cards {
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.td-page__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacer-8);
  padding: var(--spacer-48) var(--spacer-24);
  color: var(--text-secondary);
}

.td-page__empty-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

/* Dialog form（与行情源页同一套弹窗行布局） */
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
  font-weight: var(--font-weight-medium);
  flex-shrink: 0;
  width: 110px;
  white-space: nowrap;
}

.dialog-row__control {
  flex: 1;
  min-width: 0;
}

.dialog-row__hint {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
  margin-top: calc(-1 * var(--spacer-4));
  padding-left: var(--form-label-col-width); /* align with control column (label width + gap) */
}

@media (max-width: 767.98px) {
  .td-page__header {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
