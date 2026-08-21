<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import SyncSelect from '@/components/shared/SyncSelect.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
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
      <div class="batch-bar">
        <div class="batch-bar__actions">
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-8)', flexWrap: 'nowrap' }">
            <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="store.batchLogin">批量登录</button>
            <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="store.batchLogout">批量登出</button>
          </div>
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-8)', flexWrap: 'nowrap' }">
            <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="store.batchToggleAutoLogin(true)">批量开启自动登录</button>
            <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="store.batchToggleAutoLogin(false)">批量关闭自动登录</button>
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
  font-weight: var(--font-weight-medium, 500);
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
  padding-left: 122px; /* align with control column (label width + gap) */
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

/* Responsive */
@media (max-width: 768px) {
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
