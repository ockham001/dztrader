<script setup lang="ts">
/**
 * 交易接口的自动登录/登出排程编辑（契约 rest §2.10 / 契约 auto-login，ADR 0060 无关）。
 *
 * 与行情侧 `marketSources/ScheduleManager.vue` 的形态一致（同一套视觉与交互），
 * 但**数据源不同**：本组件吃 `useTdAccountsStore()` 的 `autoLogins` 镜像，只提交
 * `dztd_*` 网关的排程。刻意不复用行情侧组件——那个组件绑死 `useMarketSourcesStore()`
 * 与 `MarketSourceView` 类型，泛化会触及行情侧 3 个既有测试文件（#524「需实现时判断」）。
 * 两套控制在「逻辑复用」范围内（都是列表增删 + 时间选择），不构成能力重复：
 * 分别是两个接口类型的配置面，就像 md-config 与 td-config 是两份契约。
 *
 * 语义要点：
 * - **进程级**排程（一个网关一份），不是账户级——登出会断开该网关全部 session；
 * - 非乐观：提交后 pending 保持到 `RTN_AUTO_LOGIN` 到达（WS `auto_login` 推送）；
 * - `source=unavailable`（网关未运行/镜像未到）时不展示可编辑列表，只给提示。
 */
import { computed, ref } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TimePicker from '@/components/shared/TimePicker.vue'
import { useFieldId } from '@/composables/useFieldId'
import { useToastStore } from '@/stores/toast'
import { useTdAccountsStore } from '@/stores/tdAccounts'
import type { TdAutoLoginSchedule } from '@/api/tdGateways'

const props = defineProps<{
  /** 网关进程名（`dztd_*`） */
  gateway: string
  /** 写权限（admin）；非 admin 只读 */
  canWrite: boolean
}>()

const store = useTdAccountsStore()
const toast = useToastStore()
const fieldId = useFieldId()

const view = computed(() => store.autoLogins[props.gateway])
const pending = computed(() => store.autoLoginPending.has(props.gateway))
const unavailable = computed(() => !view.value || view.value.source === 'unavailable')
const schedules = computed<TdAutoLoginSchedule[]>(() => view.value?.schedules ?? [])
const enabled = computed<boolean>(() => view.value?.enabled ?? false)

/** 跨午夜：login_time > logout_time（契约 auto-login：会话区间跨自然日午夜） */
function isOvernight(sch: TdAutoLoginSchedule): boolean {
  return sch.login_time > sch.logout_time
}

// ===== 添加时段 =====
const modalOpen = ref(false)
const loginTime = ref('08:45')
const logoutTime = ref('15:45')
const modalError = ref<string | null>(null)

function openModal(): void {
  loginTime.value = '08:45'
  logoutTime.value = '15:45'
  modalError.value = null
  modalOpen.value = true
}

function validate(): string | null {
  // 与契约 AutoLoginConfig::validate_schedule 同口径（前端先挡，后端仍会权威校验）
  if (loginTime.value === logoutTime.value) {
    return '登录时间不能等于登出时间（会话区间必须非空）'
  }
  return null
}

async function submit(
  schedulesNext: TdAutoLoginSchedule[],
  enabledNext: boolean,
): Promise<boolean> {
  const ok = await store.setAutoLogin(props.gateway, {
    enabled: enabledNext,
    schedules: schedulesNext,
  })
  // 超时由 usePending 统一 toast；HTTP 失败才由这里出文案，避免双 toast（与 md 侧同口径）
  if (!ok && store.error) toast.error(store.error)
  return ok
}

async function addSchedule(): Promise<void> {
  const err = validate()
  if (err) {
    modalError.value = err
    return
  }
  const next = [
    ...schedules.value,
    { login_time: loginTime.value, logout_time: logoutTime.value },
  ]
  modalOpen.value = false
  await submit(next, enabled.value)
}

async function removeSchedule(sch: TdAutoLoginSchedule): Promise<void> {
  const next = schedules.value.filter(
    (s) => !(s.login_time === sch.login_time && s.logout_time === sch.logout_time),
  )
  const ok = await submit(next, enabled.value)
  // 契约 auto-login：`schedules: []` 的语义是**回落到网关内置默认排程**，不是清空。
  // 删掉最后一条后列表会「自己长回来」——不给提示会被误认为保存失败（#530 M4）。
  if (ok && next.length === 0) {
    toast.info('已清空自定义时段：网关会回落到内置默认排程')
  }
}

async function toggleEnabled(): Promise<void> {
  await submit(schedules.value, !enabled.value)
}
</script>

<template>
  <div class="td-auto-login">
    <div class="td-auto-login__hd">
      <span class="td-auto-login__title">自动登录 / 登出</span>
      <!-- 非 admin 只读：开关禁用并说明原因（与账户写按钮同口径） -->
      <button
        type="button"
        class="ds-btn ds-btn--sm"
        :class="enabled ? 'ds-btn--primary' : 'ds-btn--tertiary'"
        :disabled="!canWrite || pending || unavailable"
        :title="!canWrite ? '仅管理员可修改排程' : unavailable ? '等待网关上报排程' : ''"
        :aria-pressed="enabled"
        data-test="td-auto-login-toggle"
        @click="toggleEnabled"
      >{{ enabled ? '已启用' : '未启用' }}</button>
    </div>

    <!-- 网关未运行 / 镜像未到：如实说「不知道」，不假装空排程 -->
    <div v-if="unavailable" class="td-auto-login__hint" data-test="td-auto-login-unavailable">
      网关未运行或尚未上报排程，当前值未知。启动网关后可查看与修改。
    </div>
    <template v-else>
      <div v-if="schedules.length > 0" class="td-auto-login__list">
        <div
          v-for="sch in schedules"
          :key="`${sch.login_time}-${sch.logout_time}`"
          class="td-auto-login__item"
        >
          <span class="td-auto-login__time">{{ sch.login_time }}</span>
          <span class="td-auto-login__arrow">→</span>
          <span class="td-auto-login__time">{{ sch.logout_time }}</span>
          <span
            v-if="isOvernight(sch)"
            class="td-auto-login__overnight"
            title="跨午夜时段：[登录,24:00) ∪ [00:00,登出)"
          >跨午夜</span>
          <button
            type="button"
            class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--icon ds-btn--danger-icon td-auto-login__remove"
            :disabled="!canWrite || pending"
            :title="`删除时段 ${sch.login_time}→${sch.logout_time}`"
            :aria-label="`删除时段 ${sch.login_time}→${sch.logout_time}`"
            @click="removeSchedule(sch)"
          >
            <Icon name="Delete" :size="14" />
          </button>
        </div>
      </div>
      <!-- 契约 auto-login：空数组表示**回落到网关内置默认排程**，不是「没有排程」。
           空态文案必须与契约同义，不能写成「不会自动登录/登出」（#530 M4）。 -->
      <div v-else class="td-auto-login__hint" data-test="td-auto-login-empty">
        无自定义时段（使用网关内置默认排程）
      </div>

      <button
        type="button"
        class="ds-btn ds-btn--tertiary ds-btn--sm td-auto-login__add"
        :disabled="!canWrite || pending"
        :title="!canWrite ? '仅管理员可修改排程' : ''"
        data-test="td-auto-login-add"
        @click="openModal"
      >添加时段</button>

      <!-- 交易日窗口说明：排程只在窗口内生效，用户需要知道这条边界 -->
      <div class="td-auto-login__note">
        仅交易日窗口内生效（周一 06:00 至周六 06:00）；窗口外不触发任何登录/登出。
      </div>
    </template>

    <Modal :open="modalOpen" title="添加自动登录登出时段" @close="modalOpen = false">
      <div class="td-auto-login__form">
        <!-- 用 div 而非 label：TimePicker 的根不是原生表单控件（内部是 button），
             label 的 for 关联不成立。可见文案 + 组件自带 aria-label 已足够。 -->
        <div class="td-auto-login__field">
          <span>登录时间</span>
          <TimePicker :id="fieldId('td-auto-login-time')" v-model="loginTime" />
        </div>
        <div class="td-auto-login__field">
          <span>登出时间</span>
          <TimePicker v-model="logoutTime" />
        </div>
        <div v-if="modalError" class="td-auto-login__error" role="alert">{{ modalError }}</div>
        <div class="td-auto-login__form-actions">
          <button type="button" class="ds-btn ds-btn--sm" @click="modalOpen = false">取消</button>
          <button
            type="button"
            class="ds-btn ds-btn--sm ds-btn--primary"
            data-test="td-auto-login-submit"
            @click="addSchedule"
          >添加</button>
        </div>
      </div>
    </Modal>
  </div>
</template>

<style scoped>
.td-auto-login {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-6);
}

.td-auto-login__hd {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
}

.td-auto-login__title {
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-secondary);
}

.td-auto-login__hint {
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  color: var(--text-tertiary);
}

.td-auto-login__list {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
}

.td-auto-login__item {
  display: flex;
  align-items: center;
  gap: var(--spacer-6);
  font-size: var(--body-sm-font-size);
  font-variant-numeric: tabular-nums;
}

.td-auto-login__time {
  color: var(--text-default);
}

.td-auto-login__arrow {
  color: var(--text-tertiary);
}

.td-auto-login__overnight {
  font-size: var(--font-size-10);
  color: var(--status-alert-default);
}

.td-auto-login__remove {
  margin-left: auto;
}

.td-auto-login__add {
  align-self: flex-start;
}

.td-auto-login__note {
  font-size: var(--font-size-10);
  line-height: var(--body-xs-line-height);
  color: var(--text-tertiary);
}

.td-auto-login__form {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
}

.td-auto-login__field {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
}

.td-auto-login__error {
  color: var(--status-error-default);
  font-size: var(--body-sm-font-size);
}

.td-auto-login__form-actions {
  display: flex;
  justify-content: flex-end;
  gap: var(--spacer-8);
}
</style>
