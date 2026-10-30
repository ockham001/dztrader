<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import SyncSelect from '@/components/shared/SyncSelect.vue'
import { useMarketSourcesStore, type MarketSourceView } from '@/stores/marketSources'
import { resolveCard } from '@/stores/marketSourcesCardRegistry'
import { useToastStore } from '@/stores/toast'
import type { SelectItem } from '@/components/shared/SyncSelect.vue'

const store = useMarketSourcesStore()
const toast = useToastStore()

// 镜像驱动架构下, store.error 仅由 HTTP 失败链路 (薄代理 catch) 写入,
// 统一用 error toast 提示 (notify_ui 已走 NotifyStore, 不再经 store.error)
watch(() => store.error, (msg) => {
  if (msg) {
    toast.error(msg)
    store.clearError()
  }
})

// Add source dropdown —— SyncSelect 模式
// modelValue 始终保持空字符串：每次选择都触发添加，添加完成后回到 placeholder
const addSourceSelected = ref<string>('')
// 真实可用行情源列表（来自后端 /api/market-sources/available 扫描 dzmd_* exe）
// display_name 为空时回退到 name (进程名), 与 source-card__info-name 行为一致
const addSourceItems = computed<SelectItem[]>(() =>
  store.availableSources.map(s => {
    const label = s.display_name || s.name
    return {
      value: s.name,
      label: s.added ? `${label} (已添加)` : label,
    }
  })
)
const addSourcePlaceholder = computed(() => store.availableLoading ? '加载中…' : '添加行情源')

/// SyncSelect 菜单打开时刷新后端真实可用列表
async function onAddSourceOpen(): Promise<void> {
  await store.refreshAvailable()
}

/// 选择某项时打开对话框（设计文档 §3.1：选择行情源后弹对话框）
function onAddSourceChange(val: string | number): void {
  const name = String(val)
  const available = store.availableSources.find(s => s.name === name)
  if (!available || available.added) {
    addSourceSelected.value = ''
    return
  }
  addSourceTargetName.value = name
  addSourceTargetDisplayName.value = available.display_name
  addSourceModalOpen.value = true
  addSourceSelected.value = ''
}

// Add source modal (显示名)
const addSourceModalOpen = ref(false)
const addSourceTargetName = ref('')
const addSourceTargetDisplayName = ref('')
const addSourceSubmitting = ref(false)

function closeAddSourceModal(): void {
  addSourceModalOpen.value = false
  addSourceTargetName.value = ''
  addSourceTargetDisplayName.value = ''
  addSourceSubmitting.value = false
}

/// 对话框"添加"按钮：创建（如未在 DB）+ 启动子进程
async function confirmAddSource(): Promise<void> {
  if (!addSourceTargetName.value || !addSourceTargetDisplayName.value.trim()) return
  addSourceSubmitting.value = true
  try {
    await store.addAndStartSource(
      addSourceTargetName.value,
      addSourceTargetDisplayName.value.trim(),
    )
    closeAddSourceModal()
  } catch {
    addSourceSubmitting.value = false
  }
}

onMounted(() => {
  store.loadSources()
})

// ===== 批量操作确认（#207 C3） =====
// 旧实现点一下就对全部非在线源并发执行，无选择、无确认、无影响范围提示。
// 现改为：按钮先给出影响范围，点击后弹确认框列出目标源名称与数量，确认才下发；
// 无目标时按钮禁用（不弹空确认框）。
type BatchAction = 'login' | 'logout' | 'autoLoginOn' | 'autoLoginOff'

interface BatchActionSpec {
  title: string
  /** 动作在界面上的名字（确认框正文用） */
  verb: string
  targets: () => MarketSourceView[]
}

const BATCH_ACTIONS: Record<BatchAction, BatchActionSpec> = {
  login: { title: '批量登录', verb: '登录', targets: () => store.batchLoginTargets },
  logout: { title: '批量登出', verb: '登出', targets: () => store.batchLogoutTargets },
  autoLoginOn: { title: '批量开启自动登录', verb: '开启自动登录', targets: () => store.batchAutoLoginTargets(true) },
  autoLoginOff: { title: '批量关闭自动登录', verb: '关闭自动登录', targets: () => store.batchAutoLoginTargets(false) },
}

const batchConfirm = ref<BatchAction | null>(null)

/** 当前按钮对应的目标集合（渲染按钮文案与确认框共用同一份推导） */
function batchTargets(action: BatchAction): MarketSourceView[] {
  return BATCH_ACTIONS[action].targets()
}

function requestBatch(action: BatchAction): void {
  if (batchTargets(action).length === 0) return
  batchConfirm.value = action
}

async function confirmBatch(): Promise<void> {
  const action = batchConfirm.value
  if (!action) return
  // 目标在确认瞬间重算：弹框期间状态可能变化（如某源自己登录上了），以点击确认时为准
  const targets = batchTargets(action)
  batchConfirm.value = null
  if (targets.length === 0) return
  if (action === 'login') await store.batchLogin(targets)
  else if (action === 'logout') await store.batchLogout(targets)
  else await store.batchToggleAutoLogin(action === 'autoLoginOn', targets)
}
</script>

<template>
  <div :style="{ flex: 1, overflow: 'auto', maxWidth: '1200px', width: '100%', margin: '0 auto', padding: 'var(--spacer-32) var(--spacer-24)' }">

      <!-- ===== Page Header ===== -->
      <div class="page-header">
        <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-16)' }">
          <h1 :style="{
            margin: 0,
            fontSize: 'var(--heading-xl-font-size)',
            lineHeight: 'var(--heading-xl-line-height)',
            fontWeight: 'var(--heading-xl-font-weight)',
            color: 'var(--text-default)',
          }">行情源管理</h1>
          <span class="ds-tag" :style="{ fontVariantNumeric: 'tabular-nums' }">共 {{ store.sourceCount }} 个行情源</span>
        </div>
        <div :style="{ display: 'flex', gap: 'var(--spacer-8)', alignItems: 'center' }">
          <SyncSelect
            :items="addSourceItems"
            :model-value="addSourceSelected"
            :placeholder="addSourcePlaceholder"
            :pending="store.availableLoading"
            @open="onAddSourceOpen"
            @change="onAddSourceChange"
          >
            <template #triggerPrefix>
              <Icon name="Plus" :size="16" />
            </template>
          </SyncSelect>
        </div>
      </div>

      <!-- ===== Batch Operations Bar ===== -->
      <!-- #207 C3：按钮上直接标出将影响的源数量，点击后弹确认框列出具体目标；无目标时禁用 -->
      <div class="batch-bar">
        <div class="batch-bar__actions">
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-8)', flexWrap: 'nowrap' }">
            <button
              class="ds-btn ds-btn--secondary ds-btn--sm"
              type="button"
              data-test="batch-login"
              :disabled="batchTargets('login').length === 0"
              :title="batchTargets('login').length === 0
                ? '没有需要登录的行情源（在线、正在连接/重连的源不计入）'
                : `将登录 ${batchTargets('login').length} 个可登录行情源`"
              @click="requestBatch('login')"
            >批量登录{{ batchTargets('login').length > 0 ? `（${batchTargets('login').length}）` : '' }}</button>
            <button
              class="ds-btn ds-btn--secondary ds-btn--sm"
              type="button"
              data-test="batch-logout"
              :disabled="batchTargets('logout').length === 0"
              :title="batchTargets('logout').length === 0
                ? '没有在线且未在登出的行情源'
                : `将登出 ${batchTargets('logout').length} 个在线行情源`"
              @click="requestBatch('logout')"
            >批量登出{{ batchTargets('logout').length > 0 ? `（${batchTargets('logout').length}）` : '' }}</button>
          </div>
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-8)', flexWrap: 'nowrap' }">
            <button
              class="ds-btn ds-btn--secondary ds-btn--sm"
              type="button"
              data-test="batch-auto-login-on"
              :disabled="batchTargets('autoLoginOn').length === 0"
              :title="batchTargets('autoLoginOn').length === 0
                ? '所有行情源都已开启自动登录'
                : `将为 ${batchTargets('autoLoginOn').length} 个行情源开启自动登录`"
              @click="requestBatch('autoLoginOn')"
            >批量开启自动登录{{ batchTargets('autoLoginOn').length > 0 ? `（${batchTargets('autoLoginOn').length}）` : '' }}</button>
            <button
              class="ds-btn ds-btn--secondary ds-btn--sm"
              type="button"
              data-test="batch-auto-login-off"
              :disabled="batchTargets('autoLoginOff').length === 0"
              :title="batchTargets('autoLoginOff').length === 0
                ? '所有行情源都已关闭自动登录'
                : `将为 ${batchTargets('autoLoginOff').length} 个行情源关闭自动登录`"
              @click="requestBatch('autoLoginOff')"
            >批量关闭自动登录{{ batchTargets('autoLoginOff').length > 0 ? `（${batchTargets('autoLoginOff').length}）` : '' }}</button>
          </div>
        </div>
      </div>

      <!-- ===== Source Cards List (动态渲染: ui_card -> Component, 契约 md-config) ===== -->
      <div>
        <component
          v-for="src in store.sources"
          :key="src.id"
          :is="resolveCard(src.ui_card)"
          :source="src"
        />
      </div>

    </div>

    <!-- ===== Add Source Modal ===== -->
    <Modal :open="addSourceModalOpen" :title="`启动行情源: ${addSourceTargetName}`" @close="closeAddSourceModal">
      <div class="dialog-form">
        <div class="dialog-row">
          <label class="dialog-row__label">显示名</label>
          <div class="ds-input dialog-row__control">
            <input v-model="addSourceTargetDisplayName" type="text" placeholder="例如：CTP主行情">
          </div>
        </div>
        <div class="dialog-row__hint">行情通道 page 大小由配置文件决定（configs/dzmd_&lt;type&gt;.json 的 shm.page_size_mb，默认 1024MB）</div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" :disabled="addSourceSubmitting" @click="closeAddSourceModal">取消</button>
        <button class="ds-btn ds-btn--primary" type="button"
          :disabled="addSourceSubmitting || !addSourceTargetDisplayName.trim()"
          @click="confirmAddSource">
          <span v-if="addSourceSubmitting" class="ds-btn__spinner"></span>
          {{ addSourceSubmitting ? '启动中…' : '添加' }}
        </button>
      </template>
    </Modal>

    <!-- ===== Batch Confirm Modal（#207 C3：列出影响范围后再执行，无目标时不弹） ===== -->
    <Modal
      :open="batchConfirm !== null"
      :title="batchConfirm ? BATCH_ACTIONS[batchConfirm].title : ''"
      @close="batchConfirm = null"
    >
      <div v-if="batchConfirm" class="dialog-form" data-test="batch-confirm">
        <div class="dialog-row__hint" data-test="batch-confirm-summary">
          将对 <strong>{{ batchTargets(batchConfirm).length }}</strong> 个行情源执行「{{ BATCH_ACTIONS[batchConfirm].verb }}」：
        </div>
        <ul class="batch-confirm__list" data-test="batch-confirm-targets">
          <li v-for="src in batchTargets(batchConfirm)" :key="src.id">{{ src.display_name || src.source_name }}</li>
        </ul>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" data-test="batch-confirm-cancel" @click="batchConfirm = null">取消</button>
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          data-test="batch-confirm-ok"
          :disabled="batchConfirm !== null && batchTargets(batchConfirm).length === 0"
          @click="confirmBatch"
        >确认{{ batchConfirm ? BATCH_ACTIONS[batchConfirm].verb : '' }}</button>
      </template>
    </Modal>
</template>

<style scoped>
/* Page-specific styles */

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
  flex-wrap: wrap;
}

/* Batch operations bar */
.batch-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-16);
  padding: var(--spacer-12) var(--spacer-16);
  background: var(--bg-base-secondary);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-12);
  flex-wrap: wrap;
}

.batch-bar__actions {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  flex-wrap: wrap;
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

/* Spinner: 样式回归全局组件库 components.css 的 .ds-btn__spinner（P4 T4 去重复）。
   此前本页 scoped 重复定义同一原子，属绕过 pattern 的冗余复制，已删除。 */

/* 批量操作确认框的目标列表（#207 C3）：长列表可滚动，不把确认框撑高到遮住按钮 */
.batch-confirm__list {
  max-height: 240px;
  overflow-y: auto;
  margin: 0;
  padding-left: var(--spacer-20);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-default);
}

/* Responsive */
@media (max-width: 767.98px) {
  .page-header {
    flex-direction: column;
    align-items: flex-start;
  }

  .batch-bar {
    flex-direction: column;
    align-items: flex-start;
    padding: var(--spacer-8) var(--spacer-12);
  }
}
</style>
