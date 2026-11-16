<script setup lang="ts">
import { ref, computed, reactive, watch, onMounted, onUnmounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import Dropdown from '@/components/shared/Dropdown.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import SyncSelect from '@/components/shared/SyncSelect.vue'
import { sourceTypeText, sourceTypeClass } from '@/composables/useSourceType'
import { useFieldId } from '@/composables/useFieldId'
import { LOGIN_ERROR_TEXT, type LoginErrorCode } from '@/composables/loginErrors'
import {
  useUserManagementStore,
  getAccessModeDescription,
} from '@/stores/userManagement'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import type { SelectItem } from '@/components/shared/SyncSelect.vue'
import type { UserView } from '@/stores/userManagement'
import {
  isPasswordAcceptable,
  MIN_PASSWORD_LENGTH,
  PASSWORD_LENGTH_TEXT,
} from '@/composables/passwordPolicy'

const store = useUserManagementStore()

// label↔控件配对（#233）：id 按组件实例前缀生成，避免同页多实例撞 id
const fieldId = useFieldId()

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

// ===== 删除用户二次确认（#204）=====
// 后端是硬 DELETE 并踢断 WS，不可撤销，必须二次确认。
// 失败保留 Modal 让用户重试，错误经 toast 反馈（与添加/编辑用户一致）。
const removeUserModalOpen = ref(false)
const removeUserTarget = ref<UserView | null>(null)
const removeUserSubmitting = ref(false)

function openRemoveUserModal(u: UserView): void {
  if (u.removePending) return  // 删除进行中不重入
  removeUserTarget.value = u
  removeUserSubmitting.value = false
  removeUserModalOpen.value = true
}

function closeRemoveUserModal(): void {
  if (removeUserSubmitting.value) return  // 请求进行中不允许关闭: 失败时必须保留 Modal
  removeUserModalOpen.value = false
  removeUserTarget.value = null
}

async function confirmRemoveUser(): Promise<void> {
  const target = removeUserTarget.value
  if (!target || removeUserSubmitting.value) return  // 防重入
  removeUserSubmitting.value = true
  // P3 任务6：按真实结果反馈——失败保留用户与 Modal，不产生与服务端不一致的乐观删除
  const ok = await store.removeUser(target.id)
  removeUserSubmitting.value = false
  if (!ok) {
    showToast(store.error ?? '删除用户失败', 'error')
    return
  }
  store.clearError()
  closeRemoveUserModal()
  showToast('用户已删除', 'success')
}

async function handleRemoveIp(list: 'black' | 'white', id: number): Promise<void> {
  const entry = (list === 'black' ? store.blacklist : store.whitelist).find(e => e.id === id)
  if (!entry || entry.removePending) return  // 无操作（陈旧行 / 删除进行中）: 不反馈
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
  password: '',
  role: 'user',
})

function openAddUserModal(): void {
  // #202：不再预填固定弱口令（旧实现预填 88888888）；口令由管理员当场输入，
  // 且必须满足最小长度（服务端强制，前端只提示）
  newUser.value = {
    username: '', display_name: '', email: '', password: '',
    role: 'user',
  }
  addUserModalOpen.value = true
}

function closeAddUserModal(): void {
  addUserModalOpen.value = false
}

async function confirmAddUser(): Promise<void> {
  if (!newUser.value.username.trim() || !newUser.value.display_name.trim() || !newUser.value.password) return
  // 契约 rest §1「密码策略」：最小长度（服务端强制，此处只避免无谓往返）
  if (!isPasswordAcceptable(newUser.value.password)) {
    showToast(PASSWORD_LENGTH_TEXT, 'error')
    return
  }
  // P3 任务6：按真实结果反馈——失败不关窗、不改本地假用户
  const ok = await store.createUser({
    username: newUser.value.username.trim(),
    display_name: newUser.value.display_name.trim(),
    email: newUser.value.email.trim(),
    password: newUser.value.password,
    role: newUser.value.role,
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
  if (!isPasswordAcceptable(resetPasswordData.value.newPassword)) {
    showToast(PASSWORD_LENGTH_TEXT, 'error')
    return
  }
  const ok = await store.resetPassword(resetPasswordData.value.id, resetPasswordData.value.newPassword)
  closeResetPasswordModal()
  showToast(ok ? '密码已重置' : '重置失败', ok ? 'success' : 'error')
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

// 登录历史失败原因：与登录页共用 LOGIN_ERROR_TEXT（契约 rest §2.1 机器码单一真相源）。
// 未知 reason 保持原样回退（登录历史可能含后端新增/历史遗留 code，保持现状不吞掉）。
// 用 hasOwnProperty 而非直接索引：reason 是 DB 自由文本，直接索引会命中原型链键。
function loginFailReason(reason?: string): string {
  if (!reason) return '未知'
  return Object.prototype.hasOwnProperty.call(LOGIN_ERROR_TEXT, reason)
    ? LOGIN_ERROR_TEXT[reason as LoginErrorCode]
    : reason
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
  <div class="um-page">
    <!-- Toast — 悬浮顶部居中的瞬时操作反馈（参考 um-toast）-->
    <transition name="ds-toast-fade">
      <div
        v-if="toast.visible"
        class="ds-toast"
        :class="`ds-toast--${toast.type}`"
      >{{ toast.message }}</div>
    </transition>
    <div class="um-content">

      <!-- ===== 1. Page Header ===== -->
      <div class="page-header">
        <div class="um-header-stack">
          <div class="um-header-row">
            <h1 class="um-page-title">访问控制</h1>
          </div>
        </div>
      </div>

      <!-- ===== 2. Login Lockout Switch ===== -->
      <div class="ds-card">
        <div class="um-card-header">
          <div class="um-card-header__left">
            <div class="ds-card__title um-card-title">用户管理</div>
            <span class="ds-tag um-tabular-nums">共 {{ store.userCount }} 个用户</span>
          </div>
          <button class="ds-btn ds-btn--secondary" type="button" @click="openAddUserModal">
            <Icon name="Plus" :size="16" />
            添加用户
          </button>
        </div>

        <div class="um-switch-row">
          <SyncSwitch
            :model-value="store.securityConfig.login_lockout_enabled"
            :pending="store.securityConfig.lockoutPending"
            label="登录失败锁定"
            @change="(v: boolean) => store.toggleLoginLockout(v)"
          />
          <span class="um-switch-label">登录失败锁定</span>
          <span class="um-switch-hint">连续登录失败 {{ store.securityConfig.max_failed_attempts }} 次后锁定账号 {{ Math.floor(store.securityConfig.lockout_duration_sec / 60) }} 分钟</span>
          <div class="um-flex-fill"></div>
        </div>

      <!-- ===== 3. Filter/Search Bar ===== -->
      <div class="filter-bar">
        <div class="ds-input um-search-input">
          <span class="icon ds-input__icon"><Icon name="Search" :size="16" /></span>
          <input v-model="searchQuery" type="text" placeholder="搜索用户名、显示名称或邮箱..." aria-label="搜索用户">
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
                <th>状态</th>
                <th>最后登录</th>
                <th class="um-align-right">操作</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="u in filteredUsers" :key="u.id" :class="{ 'um-user-row--disabled': u.status === 'disabled' }">
                <td>
                  <div class="um-user-cell">
                    <span class="ds-avatar ds-avatar--sm">{{ avatarChar(u.display_name) }}</span>
                    <div>
                      <div class="um-user-cell__name">{{ u.username }}</div>
                      <div class="um-user-cell__email">{{ u.email }}</div>
                    </div>
                  </div>
                </td>
                <td>{{ u.display_name }}</td>
                <td>
                  <span class="ds-tag" :class="{ 'um-tag--admin': u.role === 'admin' }">
                    {{ u.role === 'admin' ? '管理员' : '普通用户' }}
                  </span>
                </td>
                <td>
                  <div class="um-status-cell">
                    <span class="status-dot" :class="statusDotClass(u.status)"></span>
                    <span>{{ statusText(u.status) }}</span>
                  </div>
                </td>
                <td class="um-tabular-nums">{{ u.last_login_at ?? '--' }}</td>
                <td class="ds-table__actions">
                  <div class="um-actions">
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="编辑" @click="openEditUserModal(u)">
                      <Icon name="edit" :size="14" />
                    </button>
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm" type="button" title="重置密码" v-if="u.status !== 'disabled'" @click="openResetPasswordModal(u)">
                      <Icon name="lock-refresh" :size="14" />
                    </button>
                    <button
                      v-if="u.status === 'locked'"
                      class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm um-btn-unlock"
                      type="button"
                      title="解锁"
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
                      class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon"
                      type="button"
                      :title="`删除用户 ${u.display_name}`"
                      :aria-label="`删除用户 ${u.display_name}`"
                      :disabled="u.removePending"
                      @click="openRemoveUserModal(u)"
                    >
                      <span v-if="u.removePending" class="ds-btn__spinner"></span>
                      <Icon v-else name="Delete" :size="14" />
                    </button>
                  </div>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <!-- Pagination -->
        <div class="um-pagination-bar">
          <span class="um-pagination-bar__summary">共 {{ filteredUsers.length }} 条记录，第 1 / 1 页</span>
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
          <div class="um-card-header">
            <div class="ds-card__title um-card-title">黑白名单管理</div>
          </div>

          <!-- Access mode selector -->
          <div class="um-access-mode">
            <div class="um-access-mode__row">
              <div class="um-field-label">访问模式</div>
              <SyncSelect
                :items="accessModeItems"
                :model-value="store.securityConfig.access_mode"
                :pending="store.securityConfig.accessModePending"
                placeholder="自适应防护"
                @change="(v: string | number) => store.setAccessMode(v as 'auto' | 'whitelist' | 'blacklist')"
              />
              <div class="um-flex-fill"></div>
            </div>
            <div class="ds-notif ds-notif--info">
              <span class="ds-notif__icon"><Icon name="info" :size="16" /></span>
              <span class="ds-notif__body">{{ accessModeDesc() }}</span>
            </div>
          </div>

          <!-- Tabs -->
          <div class="ds-tabs um-tabs">
            <button class="ds-tab" :class="{ 'is-active': bwTab === 'black' }" type="button" @click="bwTab = 'black'">
              黑名单 <span class="ds-tag um-tag-count" :class="{ 'ds-tag--danger': true }">{{ store.blacklistCount }}</span>
            </button>
            <button class="ds-tab" :class="{ 'is-active': bwTab === 'white' }" type="button" @click="bwTab = 'white'">
              白名单 <span class="ds-tag um-tag-count">{{ store.whitelistCount }}</span>
            </button>
          </div>

          <!-- IP input + reason + add button -->
          <div class="filter-bar">
            <div class="ds-input">
              <input v-model="newIpInput" type="text" placeholder="输入 IP 地址" aria-label="IP 地址" :disabled="addIpPending" @keyup.enter="handleAddIp">
            </div>
            <div class="ds-input">
              <input v-model="newIpReason" type="text" placeholder="备注（可选）" aria-label="IP 备注" :disabled="addIpPending" @keyup.enter="handleAddIp">
            </div>
            <button class="ds-btn ds-btn--secondary ds-btn--sm um-btn-static" type="button" :disabled="addIpPending" @click="handleAddIp">
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
                  <th class="um-align-right">操作</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in store.blacklist" :key="entry.id">
                  <td><code class="um-ip-code">{{ entry.ip }}</code></td>
                  <td><span :class="sourceTypeClass(entry.sourceType)">{{ sourceTypeText(entry.sourceType) }}</span></td>
                  <td class="um-text-secondary">{{ entry.reason ?? '--' }}</td>
                  <td class="um-tabular-nums">{{ entry.created_at }}</td>
                  <td class="ds-table__actions">
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon" type="button"
                      :title="`删除 ${entry.ip}`" :aria-label="`从黑名单删除 ${entry.ip}`"
                      :disabled="entry.removePending" @click="handleRemoveIp('black', entry.id)">
                      <span v-if="entry.removePending" class="ds-btn__spinner"></span>
                      <Icon v-else name="Delete" :size="14" />
                    </button>
                  </td>
                </tr>
                <tr v-if="store.blacklist.length === 0">
                  <td colspan="5" class="um-empty-cell">暂无黑名单记录</td>
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
                  <th class="um-align-right">操作</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in store.whitelist" :key="entry.id">
                  <td><code class="um-ip-code">{{ entry.ip }}</code></td>
                  <td><span :class="sourceTypeClass(entry.sourceType)">{{ sourceTypeText(entry.sourceType) }}</span></td>
                  <td class="um-text-secondary">{{ entry.reason ?? '--' }}</td>
                  <td class="um-tabular-nums">{{ entry.created_at }}</td>
                  <td class="ds-table__actions">
                    <button class="ds-btn ds-btn--tertiary ds-btn--icon ds-btn--sm ds-btn--danger-icon" type="button"
                      :title="`删除 ${entry.ip}`" :aria-label="`从白名单删除 ${entry.ip}`"
                      :disabled="entry.removePending" @click="handleRemoveIp('white', entry.id)">
                      <span v-if="entry.removePending" class="ds-btn__spinner"></span>
                      <Icon v-else name="Delete" :size="14" />
                    </button>
                  </td>
                </tr>
                <tr v-if="store.whitelist.length === 0">
                  <td colspan="5" class="um-empty-cell">暂无白名单记录</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <!-- ===== 6. Login History ===== -->
      <div class="section-gap">
        <div class="ds-card">
          <div class="um-card-header">
            <div class="ds-card__title um-card-title">登录历史</div>
          </div>
          <div class="um-history-scroll">
            <table class="ds-table um-history-table">
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
                    <span class="ds-avatar ds-avatar--xs um-avatar">{{ avatarChar(entry.username) }}</span>
                    {{ entry.username }}
                  </td>
                  <td class="um-history-time">{{ toLocalTime(entry.created_at) }}</td>
                  <td class="um-tabular-nums">{{ entry.ip }}</td>
                  <td class="um-text-secondary">{{ parseDevice(entry.user_agent) }}</td>
                  <td><span :class="historyStatusTag(entry.success)">{{ historyStatusText(entry.success) }}</span></td>
                  <td class="um-text-secondary">{{ entry.success ? '' : loginFailReason(entry.reason) }}</td>
                </tr>
                <tr v-if="store.loginHistory.length === 0">
                  <td colspan="6" class="um-empty-cell">暂无登录记录</td>
                </tr>
              </tbody>
            </table>
            <!-- 无限滚动哨兵：进入视口时加载下一页；触摸屏滑动天然支持 -->
            <div
              v-if="store.loginHistory.length > 0"
              ref="loginHistorySentinel"
              class="um-history-sentinel"
            ></div>
            <div
              v-if="store.loginHistoryLoadingMore"
              class="um-history-footer"
            >
              <span class="ds-btn__spinner um-loading-spinner"></span>
              加载中...
            </div>
            <div
              v-else-if="!store.loginHistoryHasMore && store.loginHistory.length > 0"
              class="um-history-footer"
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
          <label class="dialog-field__label" :for="fieldId('add-username')">用户名</label>
          <div class="ds-input">
            <input :id="fieldId('add-username')" v-model="newUser.username" type="text" placeholder="英文用户名">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('add-display-name')">显示名称</label>
          <div class="ds-input">
            <input :id="fieldId('add-display-name')" v-model="newUser.display_name" type="text" placeholder="中文名称">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('add-email')">邮箱</label>
          <div class="ds-input">
            <input :id="fieldId('add-email')" v-model="newUser.email" type="email" placeholder="选填，用于通知">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('add-password')">密码</label>
          <div class="ds-input">
            <input
              :id="fieldId('add-password')"
              v-model="newUser.password"
              type="password"
              autocomplete="new-password"
              :placeholder="`至少 ${MIN_PASSWORD_LENGTH} 位`"
            >
          </div>
          <span class="dialog-field__hint">{{ PASSWORD_LENGTH_TEXT }}</span>
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
          <label class="dialog-field__label" :for="fieldId('edit-username')">用户名</label>
          <div class="ds-input">
            <input :id="fieldId('edit-username')" :value="editUserData.username" type="text" disabled>
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('edit-display-name')">显示名称</label>
          <div class="ds-input">
            <input :id="fieldId('edit-display-name')" v-model="editUserData.display_name" type="text" placeholder="中文名称">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('edit-email')">邮箱</label>
          <div class="ds-input">
            <input :id="fieldId('edit-email')" v-model="editUserData.email" type="email" placeholder="选填，用于通知">
          </div>
        </div>
        <div class="dialog-field">
          <label class="dialog-field__label" :for="fieldId('edit-role')">角色</label>
          <Dropdown
            :id="fieldId('edit-role')"
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
          <label class="dialog-field__label" :for="fieldId('reset-new-password')">新密码</label>
          <div class="ds-input">
            <input
              :id="fieldId('reset-new-password')"
              v-model="resetPasswordData.newPassword"
              type="password"
              autocomplete="new-password"
              :placeholder="`至少 ${MIN_PASSWORD_LENGTH} 位`"
              @keyup.enter="confirmResetPassword"
            >
          </div>
          <span class="dialog-field__hint">{{ PASSWORD_LENGTH_TEXT }}；重置后该用户下次登录须先改密</span>
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

    <!-- ===== Delete User Confirmation Modal (#204: 二次确认) ===== -->
    <Modal :open="removeUserModalOpen" title="确认删除用户" @close="closeRemoveUserModal">
      <div class="um-confirm-text">
        确认删除用户 <code class="mono um-confirm-code">{{ removeUserTarget?.username }}</code> 吗？
        <div class="um-confirm-note">
          删除后不可恢复，已登录会话会被断开。
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" :disabled="removeUserSubmitting" @click="closeRemoveUserModal">取消</button>
        <button
          class="ds-btn ds-btn--danger"
          type="button"
          :disabled="removeUserSubmitting"
          @click="confirmRemoveUser"
        >
          <span v-if="removeUserSubmitting" class="ds-btn__spinner"></span>
          {{ removeUserSubmitting ? '删除中…' : '删除' }}
        </button>
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

/* ===== #346：模板内联静态样式收编（值与原内联逐字一致） ===== */

/* 页面骨架（原根节点 + 内容区 wrapper 内联） */
.um-page {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
  overflow: hidden;
}

.um-content {
  flex: 1;
  overflow: auto;
  max-width: 1200px;
  width: 100%;
  margin: 0 auto;
  padding: var(--spacer-32) var(--spacer-24);
}

/* 页头标题组（原 page-header 内的两层 flex 容器与 h1 内联） */
.um-header-stack {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

.um-header-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-16);
}

.um-page-title {
  margin: 0;
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
}

/* 卡片标题行（用户管理 / 黑白名单 / 登录历史三处复用）；
   .um-card-title 覆盖全局 .ds-card__title 的 heading-sm 尺寸与 margin */
.um-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-16);
}

.um-card-header__left {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
}

.um-card-title {
  margin-bottom: 0;
  font-size: var(--heading-md-font-size);
  line-height: var(--heading-md-line-height);
  font-weight: var(--heading-md-font-weight);
}

/* 数字等宽（计数标签 / 表格数字列） */
.um-tabular-nums {
  font-variant-numeric: tabular-nums;
}

/* 登录失败锁定行 */
.um-switch-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-16);
}

.um-switch-label {
  font-size: var(--body-sm-font-size);
  font-weight: var(--font-weight-medium);
  color: var(--text-secondary);
}

.um-switch-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

/* 撑满剩余宽度的占位（两处 flex: 1 空 div） */
.um-flex-fill {
  flex: 1;
}

/* 搜索框：原内联 max-width 在窄屏也压住 .filter-bar .ds-input 的 media 放开规则，
   故用三段选择器（scoped 后 0,4,0 > media 的 0,3,0）逐字保留该优先级 */
.filter-bar .ds-input.um-search-input {
  max-width: 280px;
}

/* 操作列表头右对齐 */
.um-align-right {
  text-align: right;
}

/* 用户单元格（头像 + 用户名/邮箱两行） */
.um-user-cell {
  display: flex;
  align-items: center;
  gap: var(--spacer-10);
}

.um-user-cell__name {
  font-weight: var(--font-weight-medium);
}

.um-user-cell__email {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

/* 管理员角色标签（原条件内联对象） */
.um-tag--admin {
  background: var(--bg-brand);
  color: var(--text-onbrand);
}

/* 状态单元格（状态点 + 文案） */
.um-status-cell {
  display: flex;
  align-items: center;
  gap: var(--spacer-6);
}

/* 行内操作按钮组 */
.um-actions {
  display: flex;
  gap: var(--spacer-4);
  justify-content: flex-end;
}

/* 解锁按钮：原内联 color 压住 .ds-btn--tertiary 的 hover/[disabled]（0,2,0），
   双类（scoped 后 0,3,0）逐字保留原优先级 */
.um-btn-unlock.um-btn-unlock {
  color: var(--status-success-default);
}

/* 分页条 */
.um-pagination-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--spacer-12) var(--spacer-20);
  border-top: 1px solid var(--border-neutral-l1);
}

.um-pagination-bar__summary {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

/* 访问模式选择块 */
.um-access-mode {
  margin-bottom: var(--spacer-16);
}

.um-access-mode__row {
  display: flex;
  align-items: center;
  gap: var(--spacer-10);
  margin-bottom: var(--spacer-8);
}

.um-field-label {
  flex-shrink: 0;
  font-size: var(--body-sm-font-size);
  font-weight: var(--font-weight-medium);
  color: var(--text-secondary);
}

/* Tabs 下间距与 tab 内计数标签 */
.um-tabs {
  margin-bottom: var(--spacer-16);
}

.um-tag-count {
  margin-left: var(--spacer-6);
}

/* 不参与伸缩的按钮（黑白名单添加） */
.um-btn-static {
  flex-shrink: 0;
}

/* 黑白名单 IP 代码列 / 次要文本列 */
.um-ip-code {
  font-family: var(--code-editor-font-family);
}

.um-text-secondary {
  color: var(--text-secondary);
}

/* 空态单元格：scoped 单类（0,2,0）会输给全局 .ds-table td:first-child（0,2,1），
   故用双类抬到（0,3,0），逐字保留原内联的 color/padding */
.um-empty-cell.um-empty-cell {
  text-align: center;
  color: var(--text-tertiary);
  padding: var(--spacer-16);
}

/* 登录历史：横向滚动容器 / 表格 / 头像 / 时间列 */
.um-history-scroll {
  overflow-x: auto;
}

.um-history-table {
  width: 100%;
  table-layout: auto;
}

.um-avatar {
  margin-right: var(--spacer-8);
  vertical-align: middle;
}

.um-history-time {
  color: var(--text-secondary);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

/* 无限滚动哨兵 / 加载与尾行 / 尾行 spinner */
.um-history-sentinel {
  height: 1px;
}

.um-history-footer {
  text-align: center;
  padding: var(--spacer-12);
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
}

.um-loading-spinner {
  width: var(--icon-size-14);
  height: var(--icon-size-14);
  margin-right: var(--spacer-8);
  vertical-align: middle;
}

/* 删除确认弹窗文案 */
.um-confirm-text {
  color: var(--text-default);
}

.um-confirm-code {
  font-weight: var(--font-weight-medium);
}

.um-confirm-note {
  margin-top: var(--spacer-8);
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
}

/* 禁用用户行（原内联三元 opacity） */
.um-user-row--disabled {
  opacity: 0.55;
}

/* Responsive（P5 收编原 900 → lg 档：窄屏放开 filter 控件宽度限制） */
@media (max-width: 1023.98px) {
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
