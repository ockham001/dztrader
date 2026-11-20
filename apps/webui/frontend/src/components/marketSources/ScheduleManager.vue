<script setup lang="ts">
import { ref, computed } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TimePicker from '@/components/shared/TimePicker.vue'
import { useFieldId } from '@/composables/useFieldId'
import { useMarketSourcesStore, RESET_TO_DEFAULT } from '@/stores/marketSources'
import { useToastStore } from '@/stores/toast'
import type { MarketSourceView } from '@/stores/marketSources'

// ScheduleManager — 自动登录时段的列表/添加/删除 + 添加时段 Modal
// 直接调 store (与现状 CtpCard 一致), props 仅传 source
const props = defineProps<{
  source: MarketSourceView
}>()

// label↔控件配对（#233）：TimePicker 的根是 div，可标注元素是其内部 button，
// 故 id 走 prop 传进去；id 前缀按组件实例区分（每个行情源一份本组件）
const fieldId = useFieldId()

const store = useMarketSourcesStore()
// toast 在组件层调用：mdConfig/marketSources 两个 store 从不 import useToastStore
// （唯一例外是 notify store 的推送分发），提示属展示职责，留在组件层维持既有分层。
const toast = useToastStore()
const src = computed(() => props.source)

// auto_login 域互斥的 UI 侧: 域内任一操作在途时入口全部禁用（全量提交期间禁止并发修改）
const autoLoginBusy = computed(() =>
  src.value.autoLoginPending || src.value.scheduleAddPending || src.value.scheduleRemovePending)

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

/// #394: 删除时段；本次提交把排程删空时提示「已恢复默认排程」。
/// 契约 auto-login：空数组 = 网关回落内置默认排程并回推 RTN_AUTO_LOGIN，列表由镜像自动恢复。
/// 提示条件由提交侧返值承载（RESET_TO_DEFAULT 仅在下发成功后出现）——RTN 侧不判断，
/// 否则每次上报都会提示；提交失败/超时（已有超时 toast）也不提示。
/// 已知边界：enabled=false 时删空同样提示「已恢复默认排程」——排程确实被恢复，
/// 但自动登录仍关着。如需按状态区分文案（如附「自动登录当前为关闭状态」），另行开票。
async function removeSchedule(sourceId: number, loginTime: string, logoutTime: string): Promise<void> {
  const result = await store.removeSchedule(sourceId, loginTime, logoutTime)
  if (result === RESET_TO_DEFAULT) toast.success('已恢复默认排程')
}

const scheduleAddPending = computed(() => {
  if (!scheduleModalSourceId.value) return false
  return store.sources.find(s => s.id === scheduleModalSourceId.value)?.scheduleAddPending ?? false
})
</script>

<template>
  <div class="card-section">
    <div class="card-section__row">
      <button class="ds-btn ds-btn--tertiary ds-btn--sm card-section__row-end" type="button" :disabled="autoLoginBusy" @click="openScheduleModal(src.id)">
        添加时段
      </button>
    </div>
    <div v-if="src.schedules.length > 0" class="schedule-list">
      <div v-for="sch in src.schedules" :key="`${sch.login_time}-${sch.logout_time}`" class="schedule-item">
        <span class="schedule-item__login">{{ sch.login_time }}</span>
        <span class="schedule-item__arrow">→</span>
        <span class="schedule-item__logout">{{ sch.logout_time }}</span>
        <span v-if="isOvernight(sch)" class="schedule-item__overnight" title="跨午夜时段：[登录,24:00) ∪ [00:00,登出)">跨午夜</span>
        <button
          class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--icon ds-btn--danger-icon schedule-item__remove"
          type="button"
          :disabled="autoLoginBusy"
          :title="`删除时段 ${sch.login_time}→${sch.logout_time}`"
          :aria-label="`删除时段 ${sch.login_time}→${sch.logout_time}`"
          @click="removeSchedule(src.id, sch.login_time, sch.logout_time)"
        >
          <span v-if="src.scheduleRemovePending" class="ds-btn__spinner"></span>
          <Icon v-else name="Delete" :size="14" />
        </button>
      </div>
    </div>
    <!-- #394: 新语义下网关永不上报空排程（空数组=归一为内置默认表），空列表只剩「镜像还没到」 -->
    <div v-else class="card-hint">
      等待网关上报排程
    </div>
  </div>

  <!-- ===== Add Schedule Modal ===== -->
  <Modal :open="scheduleModalOpen" title="添加自动登录登出" @close="closeScheduleModal">
    <div class="dialog-form">
      <div class="dialog-row">
        <label class="dialog-row__label" :for="fieldId('schedule-login-time')">登录时间</label>
        <div class="dialog-row__control">
          <TimePicker :id="fieldId('schedule-login-time')" v-model="scheduleLoginTime" />
        </div>
      </div>
      <div class="dialog-row">
        <label class="dialog-row__label" :for="fieldId('schedule-logout-time')">登出时间</label>
        <div class="dialog-row__control">
          <TimePicker :id="fieldId('schedule-logout-time')" v-model="scheduleLogoutTime" />
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

/* ── #346：模板静态内联样式归位（只搬运不改值）── */
/* 行尾「添加时段」按钮（原内联 style="margin-left: auto"） */
.card-section__row-end { margin-left: auto; }
/* 行尾删除按钮（原内联 style="margin-left: auto"） */
.schedule-item__remove { margin-left: auto; }

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