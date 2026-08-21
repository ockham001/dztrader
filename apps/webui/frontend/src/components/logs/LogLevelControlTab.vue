<script setup lang="ts">
import { useLogsStore, LOG_LEVELS } from '@/stores/logs'
import { LEVEL_COLORS } from '@/composables/useLogLevelColors'
import { useToastStore } from '@/stores/toast'
import { useAuthStore } from '@/stores/auth'
import Icon from '@/components/shared/Icon.vue'
import Dropdown from '@/components/shared/Dropdown.vue'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import type { LogProcess } from '@/types/api'

const store = useLogsStore()
const toast = useToastStore()
const auth = useAuthStore()

const isAdmin = () => auth.isAdmin

const levelItems: DropdownItem[] = LOG_LEVELS.map(lvl => ({
  value: lvl,
  label: lvl,
  dot: LEVEL_COLORS[lvl] ?? 'var(--text-tertiary)',
}))

// Check if a process is a strategy (controls disabled per spec §5.5/§6.4)
function isStrategy(proc: LogProcess): boolean {
  return proc.type === '策略'
}

// === Per-row level change ===
async function handleLevelChange(proc: LogProcess, value: string | number): Promise<void> {
  const level = String(value)
  const ok = await store.setProcessLevel(proc.name, level)
  if (ok) {
    toast.success(`${proc.name} 级别设置已下发`)
  } else {
    toast.error(`${proc.name} 级别设置失败`)
  }
}

// === Per-row flush ===
async function handleFlush(proc: LogProcess): Promise<void> {
  const ok = await store.flushProcess(proc.name)
  if (ok) {
    toast.success(`${proc.name} 日志已刷新`)
  } else {
    toast.error(`${proc.name} 日志刷新失败`)
  }
}

// === Batch set level ===
async function handleBatchSetLevel(): Promise<void> {
  if (store.selectedProcesses.size === 0) {
    toast.warning('请先选择进程')
    return
  }
  const result = await store.batchSetLevel()
  if (result.fail === 0) {
    toast.success(`已下发 ${result.ok} 个进程级别为 ${store.batchLevel}`)
  } else if (result.ok > 0) {
    toast.warning(`${result.ok} 已下发, ${result.fail} 失败`)
  } else {
    toast.error('批量设置级别失败')
  }
}

// === Batch flush ===
async function handleBatchFlush(): Promise<void> {
  if (store.selectedProcesses.size === 0) {
    toast.warning('请先选择进程')
    return
  }
  const result = await store.batchFlush()
  if (result.fail === 0) {
    toast.success(`已刷新 ${result.ok} 个进程日志`)
  } else if (result.ok > 0) {
    toast.warning(`${result.ok} 成功, ${result.fail} 失败`)
  } else {
    toast.error('批量刷新失败')
  }
}

// === Select all ===
function handleSelectAll(event: Event): void {
  if ((event.target as HTMLInputElement).checked) {
    store.selectAllProcesses()
  } else {
    store.clearProcessSelection()
  }
}
</script>

<template>
  <div class="log-control">
    <!-- Admin check: non-admin sees read-only -->
    <div v-if="!isAdmin()" class="log-control__admin-warning ds-card">
      <Icon name="Tips" :size="16" />
      <span>仅管理员可修改日志级别和刷新日志</span>
    </div>

    <!-- Toolbar -->
    <div class="log-control__toolbar">
      <div class="log-control__batch">
        <button
          class="ds-btn ds-btn--secondary ds-btn--sm"
          type="button"
          :disabled="!isAdmin()"
          @click="handleBatchFlush"
        >
          <Icon name="Refresh" :size="14" />
          批量刷新
        </button>
        <span class="log-control__batch-label">已选 {{ store.selectedProcesses.size }} 个</span>
        <Dropdown
          :items="levelItems"
          v-model="store.batchLevel"
          :disabled="!isAdmin()"
          placeholder="级别"
        />
        <button
          class="ds-btn ds-btn--primary ds-btn--sm"
          type="button"
          :disabled="!isAdmin() || store.selectedProcesses.size === 0"
          @click="handleBatchSetLevel"
        >批量设置级别</button>
      </div>
      <span class="log-control__batch-label">进程列表由 WebSocket 实时同步</span>
    </div>

    <!-- Process Table -->
    <div class="log-control__table-wrap">
      <table class="ds-table">
        <thead>
          <tr>
            <th style="width: 40px">
              <input
                type="checkbox"
                :checked="store.selectedProcesses.size === store.processes.length && store.processes.length > 0"
                :disabled="!isAdmin()"
                @change="handleSelectAll"
              >
            </th>
            <th>进程名</th>
            <th style="width: 100px">类型</th>
            <th style="width: 120px">当前级别</th>
            <th style="width: 200px">操作</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="store.processes.length === 0">
            <td colspan="5" style="text-align: center; color: var(--text-tertiary); padding: var(--spacer-24)">
              暂无进程数据（等待 WebSocket 推送进程级别信息）
            </td>
          </tr>
          <tr v-for="proc in store.processes" :key="proc.name">
            <td>
              <input
                v-if="!isStrategy(proc)"
                type="checkbox"
                :checked="store.selectedProcesses.has(proc.name)"
                :disabled="!isAdmin()"
                @change="store.toggleProcessSelection(proc.name)"
              >
            </td>
            <td>{{ proc.name }}</td>
            <td>
              <span class="ds-tag">{{ proc.type }}</span>
            </td>
            <td>
              <span class="ds-tag" :class="{
                'ds-tag--success': proc.level === 'info',
                'ds-tag--warning': proc.level === 'warning' || proc.level === 'debug',
                'ds-tag--danger': proc.level === 'error' || proc.level === 'critical',
              }">{{ proc.level }}</span>
            </td>
            <td>
              <div class="log-control__actions">
                <template v-if="isStrategy(proc)">
                  <span class="log-control__strategy-hint">策略日志由策略自行管理</span>
                </template>
                <template v-else>
                  <Dropdown
                    :items="levelItems"
                    :model-value="proc.level"
                    :disabled="!isAdmin() || store.isProcessPending(proc.name)"
                    placeholder="级别"
                    @update:model-value="(v: string | number) => handleLevelChange(proc, v)"
                  />
                  <button
                    class="ds-btn ds-btn--secondary ds-btn--sm"
                    type="button"
                    :disabled="!isAdmin() || store.isProcessPending(proc.name)"
                    @click="handleFlush(proc)"
                  >
                    <span v-if="store.isProcessPending(proc.name)" class="icon ds-btn__icon-spin" :style="{ width: '12px', height: '12px', display: 'inline-flex' }">
                      <Icon name="Refresh" :size="12" />
                    </span>
                    <Icon v-else name="Refresh" :size="12" />
                    Flush
                  </button>
                </template>
              </div>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<style>
@keyframes log-control-spin {
  to { transform: rotate(360deg); }
}
</style>

<style scoped>
.log-control {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
}

.log-control__admin-warning {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  padding: var(--spacer-12) var(--spacer-16);
  color: var(--status-warning-default);
  font-size: var(--body-sm-font-size);
}

.log-control__toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-12);
  flex-wrap: wrap;
}

.log-control__batch {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  flex-wrap: wrap;
}

.log-control__batch-label {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

.log-control__batch .ds-dropdown {
  width: auto;
  flex-shrink: 0;
  max-width: 160px;
}

.log-control__table-wrap {
  overflow-x: auto;
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-12);
}

.log-control__actions {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.log-control__actions .ds-dropdown {
  width: auto;
  flex-shrink: 0;
  max-width: 160px;
}

.log-control__strategy-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  font-style: italic;
}

.ds-btn__icon-spin {
  animation: log-control-spin 0.6s linear infinite;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

/* Responsive */
@media (max-width: 768px) {
  .log-control__toolbar {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
