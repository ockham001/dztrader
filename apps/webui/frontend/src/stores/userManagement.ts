import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { usersApi } from '@/api/users'
import { securityApi } from '@/api/security'
import type { User, Permission, SecurityConfig, IpEntry, LoginHistoryEntry } from '@/types/api'

// 同步类操作 pending 最小持续时间（ms），避免网络过快时 spinner 一闪而过
const MIN_PENDING_MS = 300
function delay(ms: number): Promise<void> {
  return new Promise(resolve => setTimeout(resolve, ms))
}

// Available permission options (from the prototype)
export const ACCOUNT_PERMISSIONS = [
  'CTP-主力账户',
  'CTP-套利账户',
  'IB-美股账户',
  '模拟盘-测试',
] as const

export const STRATEGY_PERMISSIONS = [
  'MACD_V3',
  'Grid_BTC',
  'RSI_Multi',
  'Arb_Future',
  'Alpha_Quant',
  'Trend_Follow',
] as const

export type AccountPermission = typeof ACCOUNT_PERMISSIONS[number]
export type StrategyPermission = typeof STRATEGY_PERMISSIONS[number]

export interface UserView extends User {
  accountPermissions: string[]
  strategyPermissions: string[]
  expanded: boolean
  actionPending: boolean
  removePending: boolean
}

export interface IpEntryView extends IpEntry {
  sourceType: 'auto' | 'manual'
  removePending: boolean
}

export interface SecurityConfigView extends SecurityConfig {
  lockoutPending: boolean
  accessModePending: boolean
  lockoutError: string | null
  accessModeError: string | null
}

const ACCESS_MODE_DESCRIPTIONS: Record<string, string> = {
  auto: '恶意 IP 自动加入黑名单，白名单始终放行',
  whitelist: '仅允许白名单 IP 访问，其余全部拒绝',
  blacklist: '屏蔽黑名单 IP，其余全部放行',
}

export function getAccessModeDescription(mode: string): string {
  return ACCESS_MODE_DESCRIPTIONS[mode] ?? ''
}

function defaultSecurityConfig(): SecurityConfigView {
  return {
    login_lockout_enabled: false,
    access_mode: 'auto',
    max_failed_attempts: 5,
    lockout_duration_sec: 1800,
    lockoutPending: false,
    accessModePending: false,
    lockoutError: null,
    accessModeError: null,
  }
}

export const useUserManagementStore = defineStore('userManagement', () => {
  const users = ref<UserView[]>([])
  const blacklist = ref<IpEntryView[]>([])
  const whitelist = ref<IpEntryView[]>([])
  const loginHistory = ref<LoginHistoryEntry[]>([])
  // 登录历史分页状态：初始 10 条，滚动到底加载更多 10 条
  const LOGIN_HISTORY_PAGE_SIZE = 10
  const loginHistoryPage = ref(0) // 已加载的最后一页（0 表示未加载）
  const loginHistoryHasMore = ref(true)
  const loginHistoryLoadingMore = ref(false)
  const securityConfig = ref<SecurityConfigView>(defaultSecurityConfig())
  const loading = ref(false)
  const error = ref<string | null>(null)

  const userCount = computed(() => users.value.length)
  const blacklistCount = computed(() => blacklist.value.length)
  const whitelistCount = computed(() => whitelist.value.length)

  function findUser(id: number): UserView | undefined {
    return users.value.find(u => u.id === id)
  }

  function updateUser(id: number, patch: Partial<UserView>): void {
    const idx = users.value.findIndex(u => u.id === id)
    if (idx === -1) return
    users.value[idx] = { ...users.value[idx], ...patch }
  }

  async function loadAll(): Promise<void> {
    loading.value = true
    error.value = null
    try {
      const [userResp, config, black, white, history] = await Promise.all([
        usersApi.list({ page: 1, page_size: 50 }),
        securityApi.getConfig(),
        securityApi.listBlacklist(),
        securityApi.listWhitelist(),
        securityApi.loginHistory(1, LOGIN_HISTORY_PAGE_SIZE, 1),
      ])
      users.value = userResp.users.map(u => {
        return {
          ...u,
          accountPermissions: [],
          strategyPermissions: [],
          expanded: false,
          actionPending: false,
          removePending: false,
        } as UserView
      })
      securityConfig.value = { ...config, lockoutPending: false, accessModePending: false, lockoutError: null, accessModeError: null }
      blacklist.value = black.map(b => ({ ...b, sourceType: (b.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: false }))
      whitelist.value = white.map(w => ({ ...w, sourceType: (w.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: false }))
      loginHistory.value = history.entries
      loginHistoryPage.value = 1
      loginHistoryHasMore.value = history.entries.length >= LOGIN_HISTORY_PAGE_SIZE
    } catch {
      // 后台不可用：清空数据，让 UI 显示空状态
      users.value = []
      blacklist.value = []
      whitelist.value = []
      loginHistory.value = []
      loginHistoryPage.value = 0
      loginHistoryHasMore.value = false
    } finally {
      loading.value = false
    }
  }

  function toggleExpand(id: number): void {
    const u = findUser(id)
    if (u) updateUser(id, { expanded: !u.expanded })
  }

  // 多设备同步：收到 WS data_changed 推送时按 scope 刷新对应数据
  // 保留 pending/error/expanded 等 view state，避免打断正在进行的本地操作
  async function refreshByScope(scope: string): Promise<void> {
    try {
      if (scope === 'security_config') {
        const config = await securityApi.getConfig()
        securityConfig.value = { ...securityConfig.value, ...config }
      } else if (scope === 'blacklist') {
        const black = await securityApi.listBlacklist()
        const prevBlack = new Map(blacklist.value.map(e => [e.id, e.removePending]))
        blacklist.value = black.map(b => ({ ...b, sourceType: (b.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: prevBlack.get(b.id) ?? false }))
      } else if (scope === 'whitelist') {
        const white = await securityApi.listWhitelist()
        const prevWhite = new Map(whitelist.value.map(e => [e.id, e.removePending]))
        whitelist.value = white.map(w => ({ ...w, sourceType: (w.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: prevWhite.get(w.id) ?? false }))
      } else if (scope === 'users') {
        const userResp = await usersApi.list({ page: 1, page_size: 50 })
        // 保留现有用户的 expanded/permissions 等 view state；被删除的用户自然消失
        const existingMap = new Map(users.value.map(u => [u.id, u]))
        users.value = userResp.users.map(u => {
          const existing = existingMap.get(u.id)
          return {
            ...u,
            accountPermissions: existing?.accountPermissions ?? [],
            strategyPermissions: existing?.strategyPermissions ?? [],
            expanded: existing?.expanded ?? false,
            actionPending: false,
            // 保留 in-flight 删除标记（WS 刷新不打断正在进行的删除防重入）
            removePending: existing?.removePending ?? false,
          } as UserView
        })
      } else if (scope === 'login_history') {
        // 新登录记录到达：重新加载第一页（重置分页），让用户看到最新记录
        const history = await securityApi.loginHistory(1, LOGIN_HISTORY_PAGE_SIZE, 1)
        loginHistory.value = history.entries
        loginHistoryPage.value = 1
        loginHistoryHasMore.value = history.entries.length >= LOGIN_HISTORY_PAGE_SIZE
      }
    } catch {
      // 静默失败：WS 推送触发的刷新不阻塞用户操作
    }
  }

  // 无限滚动：加载下一页登录历史，追加到列表末尾
  async function loadMoreLoginHistory(): Promise<void> {
    if (loginHistoryLoadingMore.value || !loginHistoryHasMore.value) return
    loginHistoryLoadingMore.value = true
    try {
      const nextPage = loginHistoryPage.value + 1
      const history = await securityApi.loginHistory(nextPage, LOGIN_HISTORY_PAGE_SIZE, 1)
      if (history.entries.length > 0) {
        loginHistory.value = [...loginHistory.value, ...history.entries]
      }
      loginHistoryPage.value = nextPage
      loginHistoryHasMore.value = history.entries.length >= LOGIN_HISTORY_PAGE_SIZE
    } catch {
      // 加载失败保持现有数据，允许用户重试
    } finally {
      loginHistoryLoadingMore.value = false
    }
  }

  async function createUser(data: {
    username: string
    display_name: string
    email: string
    password: string
    role: string
    accountPermissions: string[]
    strategyPermissions: string[]
  }): Promise<boolean> {
    // P3 任务6：去掉本地插假用户兜底（Date.now() 当 id 的幽灵用户会污染镜像）。
    // 失败只置 error 返回 false，由视图按真实结果反馈。
    try {
      const created = await usersApi.create({
        username: data.username,
        display_name: data.display_name,
        email: data.email,
        password: data.password,
        role: data.role,
      })
      const newUser: UserView = {
        ...created,
        accountPermissions: data.accountPermissions,
        strategyPermissions: data.strategyPermissions,
        expanded: false,
        actionPending: false,
        removePending: false,
      }
      users.value = [...users.value, newUser]
      return true
    } catch {
      error.value = '创建用户失败'
      return false
    }
  }

  async function updateUserStatus(id: number, status: 'enabled' | 'disabled' | 'locked' | 'unlocked'): Promise<void> {
    const u = findUser(id)
    if (!u || u.actionPending) return
    updateUser(id, { actionPending: true })
    try {
      const updated = await usersApi.updateStatus(id, status)
      const newStatus = updated.status
      updateUser(id, { actionPending: false, status: newStatus })
    } catch {
      // Fallback: update locally
      const localStatus: User['status'] = status === 'enabled' ? 'offline' : status === 'disabled' ? 'disabled' : status === 'locked' ? 'locked' : 'offline'
      updateUser(id, { actionPending: false, status: localStatus })
    }
  }

  async function removeUser(id: number): Promise<boolean> {
    // P3 任务6：改 API 优先——先等服务端删除成功再本地移除；失败保留用户（不产生与
    // 服务端不一致的乐观删除，后续刷新也不会"复活"）。
    const u = findUser(id)
    if (!u || u.removePending) return true  // 幂等: 目标不存在或已在删除, 无操作
    updateUser(id, { removePending: true })
    try {
      await usersApi.remove(id)
      users.value = users.value.filter(x => x.id !== id)
      return true
    } catch {
      updateUser(id, { removePending: false })
      error.value = '删除用户失败（未删除，请重试）'
      return false
    }
  }

  async function editUser(id: number, data: {
    display_name: string
    email: string
    role: string
  }): Promise<boolean> {
    try {
      const updated = await usersApi.update(id, data)
      updateUser(id, { display_name: updated.display_name, email: updated.email, role: updated.role })
      return true
    } catch {
      return false
    }
  }

  async function resetPassword(id: number, newPassword: string): Promise<boolean> {
    try {
      await usersApi.resetPassword(id, newPassword)
      return true
    } catch {
      return false
    }
  }

  async function toggleLoginLockout(enabled: boolean): Promise<void> {
    if (securityConfig.value.lockoutPending) return
    securityConfig.value.lockoutPending = true
    securityConfig.value.lockoutError = null
    const minDelay = delay(MIN_PENDING_MS)
    try {
      const updated = await securityApi.setConfig({
        login_lockout_enabled: enabled,
        access_mode: securityConfig.value.access_mode,
        max_failed_attempts: securityConfig.value.max_failed_attempts,
        lockout_duration_sec: securityConfig.value.lockout_duration_sec,
      })
      await minDelay
      securityConfig.value = { ...securityConfig.value, ...updated, lockoutPending: false, accessModePending: false, lockoutError: null, accessModeError: null }
    } catch {
      await minDelay
      securityConfig.value.lockoutPending = false
      securityConfig.value.lockoutError = '同步失败'
    }
  }

  async function setAccessMode(mode: 'auto' | 'whitelist' | 'blacklist'): Promise<void> {
    if (securityConfig.value.accessModePending) return
    securityConfig.value.accessModePending = true
    securityConfig.value.accessModeError = null
    const prevMode = securityConfig.value.access_mode
    securityConfig.value.access_mode = mode
    const minDelay = delay(MIN_PENDING_MS)
    try {
      const updated = await securityApi.setConfig({
        login_lockout_enabled: securityConfig.value.login_lockout_enabled,
        access_mode: mode,
        max_failed_attempts: securityConfig.value.max_failed_attempts,
        lockout_duration_sec: securityConfig.value.lockout_duration_sec,
      })
      await minDelay
      securityConfig.value = { ...securityConfig.value, ...updated, lockoutPending: false, accessModePending: false, lockoutError: null, accessModeError: null }
    } catch {
      await minDelay
      securityConfig.value.accessModePending = false
      securityConfig.value.access_mode = prevMode
      securityConfig.value.accessModeError = '同步失败'
    }
  }

  async function addIp(list: 'black' | 'white', ip: string, reason?: string): Promise<boolean> {
    try {
      if (list === 'black') {
        const entry = await securityApi.addBlacklist({ ip, reason })
        blacklist.value = [...blacklist.value, { ...entry, sourceType: (entry.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: false }]
      } else {
        const entry = await securityApi.addWhitelist({ ip, reason })
        whitelist.value = [...whitelist.value, { ...entry, sourceType: (entry.source === 'auto' ? 'auto' : 'manual') as 'auto' | 'manual', removePending: false }]
      }
      return true
    } catch {
      return false
    }
  }

  async function removeIp(list: 'black' | 'white', id: number): Promise<boolean> {
    const arr = list === 'black' ? blacklist.value : whitelist.value
    const target = arr.find(e => e.id === id)
    if (!target || target.removePending) return true  // 幂等: 无操作
    if (list === 'black') {
      blacklist.value = blacklist.value.map(e => e.id === id ? { ...e, removePending: true } : e)
    } else {
      whitelist.value = whitelist.value.map(e => e.id === id ? { ...e, removePending: true } : e)
    }
    try {
      if (list === 'black') {
        await securityApi.removeBlacklist(id)
        blacklist.value = blacklist.value.filter(e => e.id !== id)
      } else {
        await securityApi.removeWhitelist(id)
        whitelist.value = whitelist.value.filter(e => e.id !== id)
      }
      return true
    } catch {
      if (list === 'black') {
        blacklist.value = blacklist.value.map(e => e.id === id ? { ...e, removePending: false } : e)
      } else {
        whitelist.value = whitelist.value.map(e => e.id === id ? { ...e, removePending: false } : e)
      }
      return false
    }
  }

  async function updatePermissions(id: number, accountPermissions: string[], strategyPermissions: string[]): Promise<void> {
    updateUser(id, { accountPermissions, strategyPermissions })
    try {
      const perms: Permission[] = [
        ...accountPermissions.map(p => ({ type: 'account' as const, id: p, granted: true })),
        ...strategyPermissions.map(p => ({ type: 'strategy' as const, id: p, granted: true })),
      ]
      await usersApi.updatePermissions(id, perms)
    } catch {
      // Local-only update
    }
  }

  function clearError(): void {
    error.value = null
  }

  return {
    users,
    blacklist,
    whitelist,
    loginHistory,
    loginHistoryHasMore,
    loginHistoryLoadingMore,
    securityConfig,
    loading,
    error,
    userCount,
    blacklistCount,
    whitelistCount,
    loadAll,
    toggleExpand,
    refreshByScope,
    loadMoreLoginHistory,
    createUser,
    editUser,
    resetPassword,
    updateUserStatus,
    removeUser,
    toggleLoginLockout,
    setAccessMode,
    addIp,
    removeIp,
    updatePermissions,
    clearError,
  }
})
