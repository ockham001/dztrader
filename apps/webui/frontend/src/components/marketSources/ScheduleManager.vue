<script setup lang="ts">
import { ref, computed } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import TimePicker from '@/components/shared/TimePicker.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// ScheduleManager — 自动登录时段的列表/添加/删除 + 添加时段 Modal
// 直接调 store (与现状 CtpCard 一致), props 仅传 source
const props = defineProps<{
  source: MarketSourceView
}>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// ===== Add schedule modal =====
const scheduleModalOpen = ref(false)
const scheduleModalSourceId = ref<number | null>(null)
const scheduleLoginTime = ref('09:00')
const scheduleLogoutTime = ref('15:30')
const scheduleError = ref<string | null>(null)

function openScheduleModal(sourceId: number): void {
  scheduleModalSourceId.value = sourceId
  scheduleLoginTime.value = '09:00'
  scheduleLogoutTime.value = '15:30'
  scheduleError.value = null
  scheduleModalOpen.value = true
}

function closeScheduleModal(): void {
  scheduleModalOpen.value = false
  scheduleModalSourceId.value = null
  scheduleError.value = null
}

/// F-C10: try/catch 包裹, 失败时保持 Modal 打开 (与 addBroker/addFrontend 一致)
/// 前置校验 (契约 auto-login): login == logout 非法（会话区间必须非空）, 跨午夜（login > logout）合法
async function confirmAddSchedule(): Promise<void> {
  if (!scheduleModalSourceId.value) return
  if (scheduleLoginTime.value === scheduleLogoutTime.value) {
    scheduleError.value = '登录时间与登出时间不能相同（会话区间必须非空）'
    return
  }
  try {
    await store.addSchedule(scheduleModalSourceId.value, scheduleLoginTime.value, scheduleLogoutTime.value)
    closeScheduleModal()
  } catch {
    // 错误已由 store 设置 error 字段; 保持 Modal 打开让用户修改后重试
  }
}

// 跨午夜判定 (契约 auto-login): HH:MM 字符串词典序 = 时间序
const isOvernight = (sch: { login_time: string; logout_time: string }): boolean =>
  sch.login_time > sch.logout_time

const scheduleAddPending = computed(() => {
  if (!scheduleModalSourceId.value) return false
  return store.sources.find(s => s.id === scheduleModalSourceId.value)?.scheduleAddPending ?? false
})
</script>

<template>
  <div class="card-section">
    <div class="card-section__row">
      <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" style="margin-left: auto" @click="openScheduleModal(src.id)">
        添加时段
      </button>
    </div>
    <div v-if="src.schedules.length > 0" class="schedule-list">
      <div v-for="sch in src.schedules" :key="`${sch.login_time}-${sch.logout_time}`" class="schedule-item">
        <span class="schedule-item__login">{{ sch.login_time }}</span>
        <span class="schedule-item__arrow">→</span>
        <span class="schedule-item__logout">{{ sch.logout_time }}</span>
        <span v-if="isOvernight(sch)" class="schedule-item__overnight" title="跨午夜时段：[登录,24:00) ∪ [00:00,登出)">跨午夜</span>
        <button class="schedule-item__remove" type="button" title="此时段"
          :disabled="src.scheduleRemovePending"
          @click="store.removeSchedule(src.id, sch.login_time, sch.logout_time)">
          {{ src.scheduleRemovePending ? '删除中…' : '删除' }}
        </button>
      </div>
    </div>
    <div v-else class="card-hint">
      未设置时段{{ src.auto_login ? '，自动登录已开启，添加时段后将按时段自动登录/登出' : '，开启自动登录后将按时段自动登录/登出' }}
    </div>
  </div>

  <!-- ===== Add Schedule Modal ===== -->
  <Modal :open="scheduleModalOpen" title="添加自动登录登出" @close="closeScheduleModal">
    <div class="dialog-form">
      <div class="dialog-row">
        <label class="dialog-row__label">登录时间</label>
        <div class="dialog-row__control">
          <TimePicker v-model="scheduleLoginTime" />
        </div>
      </div>
      <div class="dialog-row">
        <label class="dialog-row__label">登出时间</label>
        <div class="dialog-row__control">
          <TimePicker v-model="scheduleLogoutTime" />
        </div>
      </div>
      <div v-if="scheduleError" class="dialog-row__error">{{ scheduleError }}</div>
    </div>
    <template #footer>
      <button class="ds-btn ds-btn--secondary" type="button" :disabled="scheduleAddPending" @click="closeScheduleModal">取消</button>
      <button class="ds-btn ds-btn--primary" type="button" :disabled="scheduleAddPending" @click="confirmAddSchedule">
        <span v-if="scheduleAddPending" class="ds-btn__spinner"></span>
        {{ scheduleAddPending ? '添加中…' : '添加' }}
      </button>
    </template>
  </Modal>
</template>

<style scoped>
/* Section */
.card-section {
  padding-bottom: var(--spacer-12);
  border-bottom: 1px solid var(--border-neutral-l1);
  margin-bottom: var(--spacer-12);
}

.card-section__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-8);
}

/* Hint text */
.card-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  padding: var(--spacer-4) 0;
}

/* Schedule list */
.schedule-list {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  margin-top: var(--spacer-8);
}

.schedule-item {
  display: flex;
  align-items: center;
  gap: var(--spacer-10);
  height: 30px;
  padding: 0 var(--spacer-4);
  border-radius: var(--radius-6);
  transition: background 0.1s ease;
}

.schedule-item:hover {
  background: var(--bg-overlay-l1);
}

.schedule-item__login,
.schedule-item__logout {
  font-family: var(--code-editor-font-family);
  font-variant-numeric: tabular-nums;
  font-size: var(--body-base-font-size);
  color: var(--text-default);
}

.schedule-item__arrow {
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
  flex-shrink: 0;
}

.schedule-item__remove {
  margin-left: auto;
  background: none;
  border: none;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
  cursor: pointer;
  padding: var(--spacer-2) var(--spacer-6);
  border-radius: var(--radius-4);
  transition: color 0.1s ease, background 0.1s ease;
}

.schedule-item__remove:disabled {
  cursor: default;
  opacity: 0.6;
}

.schedule-item__remove:not(:disabled):hover {
  color: var(--status-error-default);
  background: var(--status-error-surface-l1);
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

/* 跨午夜标识 */
.schedule-item__overnight {
  font-size: var(--body-xs-font-size);
  color: var(--status-alert-default);
  background: var(--bg-overlay-l1);
  padding: 0 var(--spacer-4);
  border-radius: var(--radius-4);
  white-space: nowrap;
}

/* 对话框错误提示 */
.dialog-row__error {
  font-size: var(--body-sm-font-size);
  color: var(--status-error-default);
  padding-left: var(--form-label-col-width); /* 与 control 列对齐 (label 宽度 + gap) */
}

/* Spinner: 复用全局 components.css 的 .ds-btn__spinner（P4 T4 去重复，统一 14px） */
</style>