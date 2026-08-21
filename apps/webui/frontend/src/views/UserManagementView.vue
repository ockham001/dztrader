<script setup lang="ts">
import { ref, computed, reactive, watch, onMounted, onUnmounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import Dropdown from '@/components/shared/Dropdown.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import SyncSelect from '@/components/shared/SyncSelect.vue'
import { sourceTypeText, sourceTypeClass } from '@/composables/useSourceType'
import {
  useUserManagementStore,
  ACCOUNT_PERMISSIONS,
  STRATEGY_PERMISSIONS,
  getAccessModeDescription,
} from '@/stores/userManagement'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import type { SelectItem } from '@/components/shared/SyncSelect.vue'
import type { UserView } from '@/stores/userManagement'

const store = useUserManagementStore()

// 登录历史无限滚动：IntersectionObserver 监听哨兵元素进入视口时加载下一页
const loginHistorySentinel = ref<HTMLElement | null>(null)
let loginHistoryObserver: IntersectionObserver | null = null

function setupLoginHistoryObserver(): void {
  if (loginHistoryObserver) loginHistoryObserver.disconnect()
  loginHistoryObserver = new IntersectionObserver(
    (entries) => {
      if (entries[0]?.isIntersecting && store.loginHistoryHasMore && !store.loginHistoryLoadingMore) {
        store.loadMoreLoginHistory()
      }
    },
    { rootMargin: '100px' },
  )
  if (loginHistorySentinel.value) {
    loginHistoryObserver.observe(loginHistorySentinel.value)
  }
}

// Filters
const searchQuery = ref('')
const roleFilter = ref<string>('')
const statusFilter = ref<string>('')

const roleItems: DropdownItem[] = [
  { value: '', label: '全部角色' },
  { value: 'admin', label: '管理员' },
  { value: 'user', label: '普通用户' },
]

const statusItems: DropdownItem[] = [
  { value: '', label: '全部状态' },
  { value: 'online', label: '在线' },
  { value: 'offline', label: '离线' },
  { value: 'disabled', label: '已禁用' },
  { value: 'locked', label: '已锁定' },
]

const filteredUsers = computed<UserView[]>(() => {
  return store.users.filter(u => {
    if (searchQuery.value) {
      const q = searchQuery.value.toLowerCase()
      const match = u.username.toLowerCase().includes(q) ||
        u.display_name.toLowerCase().includes(q) ||
        (u.email ?? '').toLowerCase().includes(q)
      if (!match) return false
    }
    if (roleFilter.value && u.role !== roleFilter.value) return false
    if (statusFilter.value && u.status !== statusFilter.value) return false
    return true
  })
})

// Blacklist/whitelist tab
const bwTab = ref<'black' | 'white'>('black')

const accessModeItems: SelectItem[] = [
  { value: 'auto', label: '自适应防护' },
  { value: 'whitelist', label: '白名单模式' },
  { value: 'blacklist', label: '黑名单模式' },
]

// Parse user_agent into human-readable device info
function parseDevice(userAgent?: string): string {
  if (!userAgent) return '--'
  const ua = userAgent
  let browser = 'Unknown'
  let os = 'Unknown'

  // Browser detection
  if (ua.includes('Edg/')) browser = 'Edge'
  else if (ua.includes('Chrome/')) browser = 'Chrome'
  else if (ua.includes('Firefox/')) browser = 'Firefox'
  else if (ua.includes('Safari/') && !ua.includes('Chrome')) browser = 'Safari'

  // OS detection
  if (ua.includes('Windows NT 10')) os = 'Windows 10/11'
  else if (ua.includes('Windows NT')) os = 'Windows'
  else if (ua.includes('Mac OS X')) os = 'macOS'
  else if (ua.includes('Linux')) os = 'Linux'
  else if (ua.includes('Android')) os = 'Android'
  else if (ua.includes('iPhone') || ua.includes('iPad')) os = 'iOS'

  return `${browser} / ${os}`
}

// 后端已将 UTC 转为服务器本地时间字符串（YYYY-MM-DD HH:MM:SS），前端直接显示
function toLocalTime(s: string): string {
  if (!s) return '--'
  return s
}

const newIpInput = ref('')
const newIpReason = ref('')
const addIpPending = ref(false)

async function handleAddIp(): Promise<void> {
  const ip = newIpInput.value.trim()
  if (!ip) return
  const reason = newIpReason.value.trim()
  addIpPending.value = true
  const ok = await store.addIp(bwTab.value, ip, reason)
  addIpPending.value = false
  if (ok) {
    newIpInput.value = ''
    newIpReason.value = ''
    showToast(bwTab.value === 'black' ? '已添加到黑名单' : '已添加到白名单', 'success')
  } else {
    showToast('添加失败，请检查 IP 格式或重试', 'error')
  }
}

// P3 任务6：删除用户按真实结果反馈——失败保留用户并提示
async function handleRemoveUser(id: number): Promise<void> {
  const ok = await store.removeUser(id)
  if (!ok) showToast(store.error ?? '删除用户失败', 'error')
}

async function handleRemoveIp(list: 'black' | 'white', id: number): Promise<void> {
  const ok = await store.removeIp(list, id)
  if (ok) {
    showToast('已删除', 'success')
  } else {
    showToast('删除失败，请重试', 'error')
  }
}

// Add user modal
const addUserModalOpen = ref(false)
const newUser = ref({
  username: '',
  display_name: '',
  email: '',
  password: '88888888',
  role: 'user',
  accountPermissions: [...ACCOUNT_PERMISSIONS] as string[],
  strategyPermissions: [...STRATEGY_PERMISSIONS] as string[],
})

function openAddUserModal(): void {
  // 默认全选权限、密码默认 88888888、邮箱选填
  newUser.value = {
    username: '', display_name: '', email: '', password: '88888888',
    role: 'user',
    accountPermissions: [...ACCOUNT_PERMISSIONS],
    strategyPermissions: [...STRATEGY_PERMISSIONS],
  }
  addUserModalOpen.value = true
}

function closeAddUserModal(): void {
  addUserModalOpen.value = false
}

async function confirmAddUser(): Promise<void> {
  if (!newUser.value.username.trim() || !newUser.value.display_name.trim() || !newUser.value.password) return
  // P3 任务6：按真实结果反馈——失败不关窗、不改本地假用户
  const ok = await store.createUser({
    username: newUser.value.username.trim(),
    display_name: newUser.value.display_name.trim(),
    email: newUser.value.email.trim(),
    password: newUser.value.password,
    role: newUser.value.role,
    accountPermissions: newUser.value.accountPermissions,
    strategyPermissions: newUser.value.strategyPermissions,
  })
  if (!ok) {
    showToast(store.error ?? '添加用户失败', 'error')
    return
  }
  store.clearError()
  closeAddUserModal()
  showToast('用户已添加', 'success')
}

// Edit user modal
const editUserModalOpen = ref(false)
const editUserData = ref({
  id: 0,
  username: '',
  display_name: '',
  email: '',
  role: 'user',
})

function openEditUserModal(u: UserView): void {
  editUserData.value = {
    id: u.id,
    username: u.username,
    display_name: u.display_name,
    email: u.email ?? '',
    role: u.role,
  }
  editUserModalOpen.value = true
}

function closeEditUserModal(): void {
  editUserModalOpen.value = false
}

async function confirmEditUser(): Promise<void> {
  if (!editUserData.value.display_name.trim()) return
  const ok = await store.editUser(editUserData.value.id, {
    display_name: editUserData.value.display_name.trim(),
    email: editUserData.value.email.trim(),
    role: editUserData.value.role,
  })
  closeEditUserModal()
  showToast(ok ? '已保存' : '保存失败', ok ? 'success' : 'error')
}

// Reset password modal
const resetPasswordModalOpen = ref(false)
const resetPasswordData = ref({
  id: 0,
  username: '',
  newPassword: '',
})

function openResetPasswordModal(u: UserView): void {
  resetPasswordData.value = {
    id: u.id,
    username: u.username,
    newPassword: '',
  }
  resetPasswordModalOpen.value = true
}

function closeResetPasswordModal(): void {
  resetPasswordModalOpen.value = false
}

async function confirmResetPassword(): Promise<void> {
  if (!resetPasswordData.value.newPassword) return
  const ok = await store.resetPassword(resetPasswordData.value.id, resetPasswordData.value.newPassword)
  closeResetPasswordModal()
  showToast(ok ? '密码已重置' : '重置失败', ok ? 'success' : 'error')
}

function toggleAccountPerm(perm: string): void {
  const idx = newUser.value.accountPermissions.indexOf(perm)
  if (idx === -1) {
    newUser.value.accountPermissions = [...newUser.value.accountPermissions, perm]
  } else {
    newUser.value.accountPermissions = newUser.value.accountPermissions.filter(p => p !== perm)
  }
}

function toggleStrategyPerm(perm: string): void {
  const idx = newUser.value.strategyPermissions.indexOf(perm)
  if (idx === -1) {
    newUser.value.strategyPermissions = [...newUser.value.strategyPermissions, perm]
  } else {
    newUser.value.strategyPermissions = newUser.value.strategyPermissions.filter(p => p !== perm)
  }
}

// 权限全选/取消全选（含 indeterminate 状态：部分选中时 checkbox 显示横线）
const accountPermAll = computed(() => newUser.value.accountPermissions.length === ACCOUNT_PERMISSIONS.length)
const strategyPermAll = computed(() => newUser.value.strategyPermissions.length === STRATEGY_PERMISSIONS.length)
const accountPermIndeterminate = computed(() =>
  newUser.value.accountPermissions.length > 0 && !accountPermAll.value)
const strategyPermIndeterminate = computed(() =>
  newUser.value.strategyPermissions.length > 0 && !strategyPermAll.value)

function toggleAllAccountPerms(): void {
  newUser.value.accountPermissions = accountPermAll.value ? [] : [...ACCOUNT_PERMISSIONS]
}
function toggleAllStrategyPerms(): void {
  newUser.value.strategyPermissions = strategyPermAll.value ? [] : [...STRATEGY_PERMISSIONS]
}

function toggleUserAccountPerm(user: UserView, perm: string): void {
  const idx = user.accountPermissions.indexOf(perm)
  const newPerms = idx === -1
    ? [...user.accountPermissions, perm]
    : user.accountPermissions.filter(p => p !== perm)
  void store.updatePermissions(user.id, newPerms, user.strategyPermissions)
}

function toggleUserStrategyPerm(user: UserView, perm: string): void {
  const idx = user.strategyPermissions.indexOf(perm)
  const newPerms = idx === -1
    ? [...user.strategyPermissions, perm]
    : user.strategyPermissions.filter(p => p !== perm)
  void store.updatePermissions(user.id, user.accountPermissions, newPerms)
}

// 用户列表权限面板的全选状态（按用户计算）
function userAccountPermAll(user: UserView): boolean {
  return user.accountPermissions.length === ACCOUNT_PERMISSIONS.length
}
function userStrategyPermAll(user: UserView): boolean {
  return user.strategyPermissions.length === STRATEGY_PERMISSIONS.length
}
function userAccountPermIndeterminate(user: UserView): boolean {
  return user.accountPermissions.length > 0 && !userAccountPermAll(user)
}
function userStrategyPermIndeterminate(user: UserView): boolean {
  return user.strategyPermissions.length > 0 && !userStrategyPermAll(user)
}
function toggleAllUserAccountPerms(user: UserView): void {
  const newPerms = userAccountPermAll(user) ? [] : [...ACCOUNT_PERMISSIONS]
  void store.updatePermissions(user.id, newPerms, user.strategyPermissions)
}
function toggleAllUserStrategyPerms(user: UserView): void {
  const newPerms = userStrategyPermAll(user) ? [] : [...STRATEGY_PERMISSIONS]
  void store.updatePermissions(user.id, user.accountPermissions, newPerms)
}

// Status helpers
function statusDotClass(status: string): string {
  return `status-dot--${status}`
}

function statusText(status: string): string {
  const map: Record<string, string> = {
    online: '在线', offline: '离线', disabled: '已禁用', locked: '已锁定',
  }
  return map[status] ?? status
}

function avatarChar(name: string): string {
  return name.charAt(0)
}

// ===== Toast — 悬浮顶部居中的瞬时操作反馈（参考 um-toast）=====
interface ToastState {
  visible: boolean
  message: string
  type: 'success' | 'error' | 'info'
}
const toast = reactive<ToastState>({ visible: false, message: '', type: 'info' })
let toastTimer: ReturnType<typeof setTimeout> | null = null

function showToast(message: string, type: 'success' | 'error' | 'info' = 'success'): void {
  toast.message = message
  toast.type = type
  toast.visible = true
  if (toastTimer) clearTimeout(toastTimer)
  toastTimer = setTimeout(() => { toast.visible = false }, 3000)
}

// 监听 store pending 变化（true→false 表示操作结束），触发 toast
watch(() => store.securityConfig.lockoutPending, (pending, oldPending) => {
  if (oldPending && !pending) {
    const c = store.securityConfig
    if (c.lockoutError) {
      showToast(`登录失败锁定：${c.lockoutError}`, 'error')
    } else {
      showToast(c.login_lockout_enabled ? '已启用登录失败锁定' : '已关闭登录失败锁定', 'success')
    }
  }
})
watch(() => store.securityConfig.accessModePending, (pending, oldPending) => {
  if (oldPending && !pending) {
    const c = store.securityConfig
    if (c.accessModeError) {
      showToast(`访问模式：${c.accessModeError}`, 'error')
    } else {
      showToast('访问模式已更新', 'success')
    }
  }
})

function accessModeDesc(): string {
  return getAccessModeDescription(store.securityConfig.access_mode)
}

function historyStatusTag(success: boolean): string {
  return success ? 'ds-tag ds-tag--success' : 'ds-tag ds-tag--danger'
}

function historyStatusText(success: boolean): string {
  return success ? '成功' : '失败'
}

const failReasonMap: Record<string, string> = {
  invalid_credentials: '密码错误',
  account_locked: '账号锁定',
  account_disabled: '账号已禁用',
  ip_banned: 'IP 被封禁',
}

function loginFailReason(reason?: string): string {
  if (!reason) return '未知'
  return failReasonMap[reason] || reason
}

// 哨兵元素随数据加载/WS刷新动态出现（v-if），watch 确保挂载时重新挂载 observer
watch(loginHistorySentinel, (el) => {
  if (el) setupLoginHistoryObserver()
})

onMounted(() => {
  store.loadAll()
})
onUnmounted(() => {
  if (toastTimer) clearTimeout(toastTimer)
  if (loginHistoryObserver) loginHistoryObserver.disconnect()
})
</script>

<template>
  <div :style="{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0, overflow: 'hidden' }">
    <!-- Toast — 悬浮顶部居中的瞬时操作反馈（参考 um-toast）-->
    <transition name="ds-toast-fade">
      <div
        v-if="toast.visible"
        class="ds-toast"
        :class="`ds-toast--${toast.type}`"
      >{{ toast.message }}</div>
    </transition>
    <div :style="{ flex: 1, overflow: 'auto', maxWidth: '1200px', width: '100%', margin: '0 auto', padding: 'var(--spacer-32) var(--spacer-24)' }">

      <!-- ===== 1. Page Header ===== -->
      <div class="page-header">
        <div :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-8)' }">
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-16)' }">
            <h1 :style="{
              margin: 0,
              fontSize: 'var(--heading-xl-font-size)',
              lineHeight: 'var(--heading-xl-line-height)',
              fontWeight: 'var(--heading-xl-font-weight)',
              color: 'var(--text-default)',
            }">访问控制</h1>
          </div>
        </div>
      </div>

      <!-- ===== 2. Login Lockout Switch ===== -->
      <div class="ds-card">
        <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 'var(--spacer-16)', flexWrap: 'wrap', gap: 'var(--spacer-12)' }">
          <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-12)' }">
            <div class="ds-card__title" :style="{ marginBottom: 0, fontSize: 'var(--heading-md-font-size)', lineHeight: 'var(--heading-md-line-height)', fontWeight: 'var(--heading-md-font-weight)' }">用户管理</div>
            <span class="ds-tag" :style="{ fontVariantNumeric: 'tabular-nums' }">共 {{ store.userCount }} 个用户</span>
          </div>
          <button class="ds-btn ds-btn--secondary" type="button" @click="openAddUserModal">
            <Icon name="Plus" :size="16" />
            添加用户
          </button>
        </div>

        <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-12)', marginBottom: 'var(--spacer-16)' }">
          <SyncSwitch
            :model-value="store.securityConfig.login_lockout_enabled"
            :pending="store.securityConfig.lockoutPending"
            @change="(v: boolean) => store.toggleLoginLockout(v)"
          />
          <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-medium)', color: 'var(--text-secondary)' }">登录失败锁定</span>
          <span :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }">连续登录失败 {{ store.securityConfig.max_failed_attempts }} 次后锁定账号 {{ Math.floor(store.securityConfig.lockout_duration_sec / 60) }} 分钟</span>
          <div :style="{ flex: 1 }"></div>
        </div>

      <!-- ===== 3. Filter/Search Bar ===== -->
      <div class="filter-bar">
        <div class="ds-input" :style="{ maxWidth: '280px' }">
          <span class="icon ds-input__icon"><Icon name="Search" :size="16" /></span>
          <input v-model="searchQuery" type="text" placeholder="搜索用户名、显示名称或邮箱...">
        </div>
        <Dropdown
          :items="roleItems"
          v-model="roleFilter"
          :placeholder="'全部角色'"
        />
        <Dropdown
          :items="statusItems"
          v-model="statusFilter"
          :placeholder="'全部状态'"
        />
      </div>

      <!-- ===== 4. User Table ===== -->
        <div class="ds-table-card">
          <table class="ds-table">
            <thead>
              <tr>
                <th>用户名</th>
                <th>显示名称</th>
                <th>角色</th>
                <th>账户权限</th>
                <th>策略权限</th>
                <th>状态</th>
                <th>最后登录</th>
                <th :style="{ textAlign: 'right' }">操作</th>
              </tr>
            </thead>
            <tbody>
              <template v-for="u in filteredUsers" :key="u.id">
                <tr :style="{ opacity: u.status === 'disabled' ? 0.55 : 1 }">
                  <td>
                    <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-10)' }">
                      <span class="ds-avatar ds-avatar--sm">{{ avatarChar(u.display_name) }}</span>
                      <div>
                        <div :style="{ fontWeight: 'var(--font-weight-medium)' }">{{ u.username }}</div>
                        <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }">{{ u.email }}</div>
                      </div>
                    </div>
                  </td>
                  <td>{{ u.display_name }}</td>
                  <td>
                    <span class="ds-tag" :style="u.role === 'admin' ? { background: 'var(--bg-brand)', color: 'var(--text-onbrand)' } : {}">
                      {{ u.role === 'admin' ? '管理员' : '普通用户' }}
                    </span>
                  </td>
                  <td><button class="ds-btn ds-btn--link" type="button" @click="store.toggleExpand(u.id)">{{ u.accountPermissions.length }} 个账户</button></td>
                  <td><button class="ds-btn ds-btn--link" type="button" @click="store.toggleExpand(u.id)">{{ u.strategyPermissions.length }} 个策略</button></td>
                  <td>
                    <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)' }">
                      <span class="status-dot" :class="statusDotClass(u.status)"></span>
                      <span>{{ statusText(u.status) }}</span>
                    </div>
                  </td>
                  <td :style="{ fontVariantNumeric: 'tabular-nums' }">{{ u.last_login_at ?? '--' }}</td>
                  <td class="ds-table__actions">
                    <div :style="{ display: 'flex', gap: 'var(--spacer-4)', justifyContent: 'flex-end' }">
                      <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="编辑" @click="openEditUserModal(u)">
                        <Icon name="edit" :size="14" />
                      </button>
                      <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="重置密码" v-if="u.status !== 'disabled'" @click="openResetPasswordModal(u)">
                        <Icon name="lock-refresh" :size="14" />
                      </button>
                      <button
                        v-if="u.status === 'locked'"
                        class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                        type="button"
                        title="解锁"
                        :style="{ color: 'var(--status-success-default)' }"
                        :disabled="u.actionPending"
                        @click="store.updateUserStatus(u.id, 'unlocked')"
                      >
                        <Icon name="unlock" :size="14" />
                      </button>
                      <button
                        v-if="u.status === 'disabled'"
                        class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                        type="button"
                        title="启用"
                        :disabled="u.actionPending"
                        @click="store.updateUserStatus(u.id, 'enabled')"
                      >
                        <Icon name="play-filled" :size="14" />
                      </button>
                      <button
                        v-else-if="u.role !== 'admin'"
                        class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                        type="button"
                        title="禁用"
                        :disabled="u.actionPending"
                        @click="store.updateUserStatus(u.id, 'disabled')"
                      >
                        <Icon name="Ban" :size="14" />
                      </button>
                      <button
                        v-if="u.role !== 'admin'"
                        class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm"
                        type="button"
                        title="删除"
                        :style="{ color: 'var(--status-error-default)' }"
                        @click="handleRemoveUser(u.id)"
                      >
                        <Icon name="Delete" :size="14" />
                      </button>
                    </div>
                  </td>
                </tr>
                <!-- Expandable permission row -->
                <tr v-if="u.expanded" class="expand-row is-open" :key="`expand-${u.id}`">
                  <td colspan="8" :style="{ padding: 'var(--spacer-16) var(--spacer-12)' }">
                    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 'var(--spacer-24)' }">
                      <div>
                        <label class="ds-check ds-check--all" :style="{ marginBottom: 'var(--spacer-8)' }">
                          <input
                            type="checkbox"
                            :checked="userAccountPermAll(u)"
                            :indeterminate.prop="userAccountPermIndeterminate(u)"
                            @change="toggleAllUserAccountPerms(u)"
                          >
                          <span class="ds-check__box"></span>
                          <span :style="{ fontWeight: 'var(--font-weight-medium)', fontSize: 'var(--body-md-font-size)' }">账户权限</span>
                        </label>
                        <div class="perm-list">
                          <label v-for="perm in ACCOUNT_PERMISSIONS" :key="perm" class="ds-check">
                            <input
                              type="checkbox"
                              :checked="u.accountPermissions.includes(perm)"
                              @change="toggleUserAccountPerm(u, perm)"
                            >
                            <span class="ds-check__box"></span>
                            {{ perm }}
                          </label>
                        </div>
                      </div>
                      <div>
                        <label class="ds-check ds-check--all" :style="{ marginBottom: 'var(--spacer-8)' }">
                          <input
                            type="checkbox"
                            :checked="userStrategyPermAll(u)"
                            :indeterminate.prop="userStrategyPermIndeterminate(u)"
                            @change="toggleAllUserStrategyPerms(u)"
                          >
                          <span class="ds-check__box"></span>
                          <span :style="{ fontWeight: 'var(--font-weight-medium)', fontSize: 'var(--body-md-font-size)' }">策略权限</span>
                        </label>
                        <div class="perm-list">
                          <label v-for="perm in STRATEGY_PERMISSIONS" :key="perm" class="ds-check">
                            <input
                              type="checkbox"
                              :checked="u.strategyPermissions.includes(perm)"
                              @change="toggleUserStrategyPerm(u, perm)"
                            >
                            <span class="ds-check__box"></span>
                            {{ perm }}
                          </label>
                        </div>
                      </div>
                    </div>
                  </td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>
        <!-- Pagination -->
        <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-12) var(--spacer-20)', borderTop: '1px solid var(--border-neutral-l1)' }">
          <span :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }">共 {{ filteredUsers.length }} 条记录，第 1 / 1 页</span>
          <div class="ds-pagination">
            <button class="ds-pagination__item" type="button" disabled>&lt;</button>
            <button class="ds-pagination__item is-active" type="button">1</button>
            <button class="ds-pagination__item" type="button" disabled>&gt;</button>
          </div>
        </div>
      </div>

      <!-- ===== 5. Blacklist/Whitelist Section ===== -->
      <div class="section-gap">
        <div class="ds-card">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 'var(--spacer-16)', flexWrap: 'wrap', gap: 'var(--spacer-12)' }">
            <div class="ds-card__title" :style="{ marginBottom: 0, fontSize: 'var(--heading-md-font-size)', lineHeight: 'var(--heading-md-line-height)', fontWeight: 'var(--heading-md-font-weight)' }">黑白名单管理</div>
          </div>

          <!-- Access mode selector -->
          <div :style="{ marginBottom: 'var(--spacer-16)' }">
            <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-10)', marginBottom: 'var(--spacer-8)' }">
              <div :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-medium)', color: 'var(--text-secondary)', flexShrink: 0 }">访问模式</div>
              <SyncSelect
                :items="accessModeItems"
                :model-value="store.securityConfig.access_mode"
                :pending="store.securityConfig.accessModePending"
                placeholder="自适应防护"
                @change="(v: string | number) => store.setAccessMode(v as 'auto' | 'whitelist' | 'blacklist')"
              />
              <div :style="{ flex: 1 }"></div>
            </div>
            <div class="ds-notif ds-notif--info">
              <span class="ds-notif__icon"><Icon name="info" :size="16" /></span>
              <span class="ds-notif__body">{{ accessModeDesc() }}</span>
            </div>
          </div>

          <!-- Tabs -->
          <div class="ds-tabs" :style="{ marginBottom: 'var(--spacer-16)' }">
            <button class="ds-tab" :class="{ 'is-active': bwTab === 'black' }" type="button" @click="bwTab = 'black'">
              黑名单 <span class="ds-tag" :class="{ 'ds-tag--danger': true }" :style="{ marginLeft: 'var(--spacer-6)' }">{{ store.blacklistCount }}</span>
            </button>
            <button class="ds-tab" :class="{ 'is-active': bwTab === 'white' }" type="button" @click="bwTab = 'white'">
              白名单 <span class="ds-tag" :style="{ marginLeft: 'var(--spacer-6)' }">{{ store.whitelistCount }}</span>
            </button>
          </div>

          <!-- IP input + reason + add button -->
          <div class="filter-bar">
            <div class="ds-input">
              <input v-model="newIpInput" type="text" placeholder="输入 IP 地址" :disabled="addIpPending" @keyup.enter="handleAddIp">
            </div>
            <div class="ds-input">
              <input v-model="newIpReason" type="text" placeholder="备注（可选）" :disabled="addIpPending" @keyup.enter="handleAddIp">
            </div>
            <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" :disabled="addIpPending" @click="handleAddIp" :style="{ flexShrink: 0 }">
              <span v-if="addIpPending" class="ds-btn__spinner"></span>
              <Icon v-else name="Plus" :size="14" />
              {{ bwTab === 'black' ? '添加到黑名单' : '添加到白名单' }}
            </button>
          </div>

          <!-- Blacklist table -->
          <div v-if="bwTab === 'black'" class="ds-table-card">
            <table class="ds-table">
              <thead>
                <tr>
                  <th>IP 地址</th>
                  <th>添加方式</th>
                  <th>备注</th>
                  <th>添加时间</th>
                  <th :style="{ textAlign: 'right' }">操作</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in store.blacklist" :key="entry.id">
                  <td><code :style="{ fontFamily: 'var(--code-editor-font-family)' }">{{ entry.ip }}</code></td>
                  <td><span :class="sourceTypeClass(entry.sourceType)">{{ sourceTypeText(entry.sourceType) }}</span></td>
                  <td :style="{ color: 'var(--text-secondary)' }">{{ entry.reason ?? '--' }}</td>
                  <td :style="{ fontVariantNumeric: 'tabular-nums' }">{{ entry.created_at }}</td>
                  <td class="ds-table__actions">
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="删除" :style="{ color: 'var(--status-error-default)' }" @click="handleRemoveIp('black', entry.id)">
                      <Icon name="Delete" :size="14" />
                    </button>
                  </td>
                </tr>
                <tr v-if="store.blacklist.length === 0">
                  <td colspan="5" :style="{ textAlign: 'center', color: 'var(--text-tertiary)', padding: 'var(--spacer-16)' }">暂无黑名单记录</td>
                </tr>
              </tbody>
            </table>
          </div>

          <!-- Whitelist table -->
          <div v-else class="ds-table-card">
            <table class="ds-table">
              <thead>
                <tr>
                  <th>IP 地址</th>
                  <th>添加方式</th>
                  <th>备注</th>
                  <th>添加时间</th>
                  <th :style="{ textAlign: 'right' }">操作</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in store.whitelist" :key="entry.id">
                  <td><code :style="{ fontFamily: 'var(--code-editor-font-family)' }">{{ entry.ip }}</code></td>
                  <td><span :class="sourceTypeClass(entry.sourceType)">{{ sourceTypeText(entry.sourceType) }}</span></td>
                  <td :style="{ color: 'var(--text-secondary)' }">{{ entry.reason ?? '--' }}</td>
                  <td :style="{ fontVariantNumeric: 'tabular-nums' }">{{ entry.created_at }}</td>
                  <td class="ds-table__actions">
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="删除" :style="{ color: 'var(--status-error-default)' }" @click="handleRemoveIp('white', entry.id)">
                      <Icon name="Delete" :size="14" />
                    </button>
                  </td>
                </tr>
                <tr v-if="store.whitelist.length === 0">
                  <td colspan="5" :style="{ textAlign: 'center', color: 'var(--text-tertiary)', padding: 'var(--spacer-16)' }">暂无白名单记录</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <!-- ===== 6. Login History ===== -->
      <div class="section-gap">
        <div class="ds-card">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 'var(--spacer-16)', flexWrap: 'wrap', gap: 'var(--spacer-12)' }">
            <div class="ds-card__title" :style="{ marginBottom: 0, fontSize: 'var(--heading-md-font-size)', lineHeight: 'var(--heading-md-line-height)', fontWeight: 'var(--heading-md-font-weight)' }">登录历史</div>
          </div>
          <div :style="{ overflowX: 'auto' }">
            <table class="ds-table" :style="{ width: '100%', tableLayout: 'auto' }">
              <thead>
                <tr>
                  <th>用户</th>
                  <th>时间</th>
                  <th>IP 地址</th>
                  <th>设备 / 浏览器</th>
                  <th>状态</th>
                  <th>失败原因</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in store.loginHistory" :key="entry.id">
                  <td>
                    <span class="ds-avatar ds-avatar--xs" :style="{ marginRight: 'var(--spacer-8)', verticalAlign: 'middle' }">{{ avatarChar(entry.username) }}</span>
                    {{ entry.username }}
                  </td>
                  <td :style="{ color: 'var(--text-secondary)', fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }">{{ toLocalTime(entry.created_at) }}</td>
                  <td :style="{ fontVariantNumeric: 'tabular-nums' }">{{ entry.ip }}</td>
                  <td :style="{ color: 'var(--text-secondary)' }">{{ parseDevice(entry.user_agent) }}</td>
                  <td><span :class="historyStatusTag(entry.success)">{{ historyStatusText(entry.success) }}</span></td>
                  <td :style="{ color: 'var(--text-secondary)' }">{{ entry.success ? '' : loginFailReason(entry.reason) }}</td>
                </tr>
                <tr v-if="store.loginHistory.length === 0">
                  <td colspan="6" :style="{ textAlign: 'center', color: 'var(--text-tertiary)', padding: 'var(--spacer-16)' }">暂无登录记录</td>
                </tr>
              </tbody>
            </table>
            <!-- 无限滚动哨兵：进入视口时加载下一页；触摸屏滑动天然支持 -->
            <div
              v-if="store.loginHistory.length > 0"
              ref="loginHistorySentinel"
              :style="{ height: '1px' }"
            ></div>
            <div
              v-if="store.loginHistoryLoadingMore"
              :style="{ textAlign: 'center', padding: 'var(--spacer-12)', color: 'var(--text-tertiary)', fontSize: 'var(--body-xs-font-size)' }"
            >
              <span class="ds-btn__spinner" :style="{ width: '14px', height: '14px', marginRight: 'var(--spacer-8)', verticalAlign: 'middle' }"></span>
              加载中...
            </div>
            <div
              v-else-if="!store.loginHistoryHasMore && store.loginHistory.length > 0"
              :style="{ textAlign: 'center', padding: 'var(--spacer-12)', color: 'var(--text-tertiary)', fontSize: 'var(--body-xs-font-size)' }"
            >
              没有更多记录
            </div>
          </div>
        </div>
      </div>

    </div>

    <!-- ===== Add User Modal ===== -->
    <Modal :open="addUserModalOpen" title="添加用户" @close="closeAddUserModal">
      <div class="dialog-form">
        <div class="dialog-field">
          <label class="dialog-field__label">用户名</label>
          <div class="ds-input">
            <input v-model="newUser.username" type="text" placeholder="英文用户名">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">显示名称</label>
          <div class="ds-input">
            <input v-model="newUser.display_name" type="text" placeholder="中文名称">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">邮箱</label>
          <div class="ds-input">
            <input v-model="newUser.email" type="email" placeholder="选填，用于通知">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">密码</label>
          <div class="ds-input">
            <input v-model="newUser.password" type="text" placeholder="默认 88888888">
          </div>
        </div>
        <div class="dialog-field dialog-field--perms">
          <div class="dialog-field__perms-header">
            <label class="ds-check ds-check--all">
              <input
                type="checkbox"
                :checked="accountPermAll"
                :indeterminate.prop="accountPermIndeterminate"
                @change="toggleAllAccountPerms"
              >
              <span class="ds-check__box"></span>
              <span class="dialog-field__label">账户权限</span>
            </label>
          </div>
          <div class="perm-list">
            <label v-for="perm in ACCOUNT_PERMISSIONS" :key="perm" class="ds-check">
              <input
                type="checkbox"
                :checked="newUser.accountPermissions.includes(perm)"
                @change="toggleAccountPerm(perm)"
              >
              <span class="ds-check__box"></span>
              {{ perm }}
            </label>
          </div>
        </div>
        <div class="dialog-field dialog-field--perms">
          <div class="dialog-field__perms-header">
            <label class="ds-check ds-check--all">
              <input
                type="checkbox"
                :checked="strategyPermAll"
                :indeterminate.prop="strategyPermIndeterminate"
                @change="toggleAllStrategyPerms"
              >
              <span class="ds-check__box"></span>
              <span class="dialog-field__label">策略权限</span>
            </label>
          </div>
          <div class="perm-list">
            <label v-for="perm in STRATEGY_PERMISSIONS" :key="perm" class="ds-check">
              <input
                type="checkbox"
                :checked="newUser.strategyPermissions.includes(perm)"
                @change="toggleStrategyPerm(perm)"
              >
              <span class="ds-check__box"></span>
              {{ perm }}
            </label>
          </div>
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="closeAddUserModal">取消</button>
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          :disabled="!newUser.username.trim() || !newUser.display_name.trim() || !newUser.password"
          @click="confirmAddUser"
        >保存</button>
      </template>
    </Modal>

    <!-- ===== Edit User Modal ===== -->
    <Modal :open="editUserModalOpen" title="编辑用户" @close="closeEditUserModal">
      <div class="dialog-form">
        <div class="dialog-field">
          <label class="dialog-field__label">用户名</label>
          <div class="ds-input">
            <input :value="editUserData.username" type="text" disabled>
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">显示名称</label>
          <div class="ds-input">
            <input v-model="editUserData.display_name" type="text" placeholder="中文名称">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">邮箱</label>
          <div class="ds-input">
            <input v-model="editUserData.email" type="email" placeholder="选填，用于通知">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label">角色</label>
          <Dropdown
            :items="[{ value: 'user', label: '普通用户' }, { value: 'admin', label: '管理员' }]"
            v-model="editUserData.role"
            :placeholder="'普通用户'"
          />
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="closeEditUserModal">取消</button>
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          :disabled="!editUserData.display_name.trim()"
          @click="confirmEditUser"
        >保存</button>
      </template>
    </Modal>

    <!-- ===== Reset Password Modal ===== -->
    <Modal :open="resetPasswordModalOpen" :title="`重置密码 — ${resetPasswordData.username}`" @close="closeResetPasswordModal">
      <div class="dialog-form">
        <div class="dialog-field">
          <label class="dialog-field__label">新密码</label>
          <div class="ds-input">
            <input v-model="resetPasswordData.newPassword" type="text" placeholder="输入新密码" @keyup.enter="confirmResetPassword">
          </div>
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="closeResetPasswordModal">取消</button>
        <button
          class="ds-btn ds-btn--primary"
          type="button"
          :disabled="!resetPasswordData.newPassword"
          @click="confirmResetPassword"
        >重置</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
/* Page-specific styles (replicated from user-management.html) */

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
  flex-wrap: wrap;
}

.filter-bar {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-16);
  flex-wrap: wrap;
}

.filter-bar .ds-input {
  flex: 1;
  min-width: 120px;
  max-width: 280px;
}

.filter-bar .ds-dropdown {
  width: auto;
  flex-shrink: 0;
  max-width: 160px;
}

.section-gap {
  margin-top: var(--spacer-32);
}

/* Expandable permission row */
.expand-row td {
  background: var(--bg-overlay-l1);
}

/* Permission list */
.perm-list {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacer-8);
}

/* Status dot */
.status-dot {
  width: 8px;
  height: 8px;
  border-radius: var(--radius-full);
  display: inline-block;
  flex-shrink: 0;
}

.status-dot--online {
  background: var(--status-success-default);
}

.status-dot--offline {
  background: var(--brand-grey-500);
}

.status-dot--disabled {
  background: var(--status-error-default);
}

.status-dot--locked {
  background: var(--status-alert-default);
}

/* Dialog form */
.dialog-form {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
}

/* 注：.dialog-field 的水平布局（label + edit 同行）由全局 components.css 提供 */
.dialog-field__label {
  font-size: var(--body-base-font-size);
  color: var(--text-default);
  font-weight: var(--font-weight-medium);
}

/* Responsive */
@media (max-width: 900px) {
  .filter-bar .ds-input {
    max-width: none;
  }

  .filter-bar .ds-dropdown {
    max-width: none;
  }

  .page-header {
    flex-direction: column;
    align-items: flex-start;
  }
}
</style>
