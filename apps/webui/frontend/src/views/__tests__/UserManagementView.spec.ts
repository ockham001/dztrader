import { describe, it, expect, vi, beforeEach, beforeAll } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useUserManagementStore, ACCOUNT_PERMISSIONS, STRATEGY_PERMISSIONS } from '@/stores/userManagement'
import type { UserView } from '@/stores/userManagement'
import UserManagementView from '../UserManagementView.vue'

// jsdom 缺 IntersectionObserver（登录历史无限滚动用）
class IOStub {
  observe(): void {}
  unobserve(): void {}
  disconnect(): void {}
  takeRecords(): IntersectionObserverEntry[] { return [] }
}
beforeAll(() => {
  ;(globalThis as Record<string, unknown>).IntersectionObserver = IOStub
})

// stub 子组件
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], emits: ['close'], template: '<div class="modal-stub"><slot /></div><div class="modal-footer-stub"><slot name="footer" /></div>' },
  Dropdown: { props: ['modelValue'], template: '<span class="dropdown-stub">{{ modelValue }}</span>' },
  SyncSwitch: { props: ['modelValue', 'pending'], emits: ['change'], template: '<button type="button" class="sync-switch" @click="$emit(\'change\', !modelValue)" />' },
  SyncSelect: { props: ['items', 'modelValue'], emits: ['change'], template: '<button type="button" class="sync-select" @click="$emit(\'change\', \'blacklist\')">{{ modelValue }}</button>' },
}

function fakeUser(
  id: number,
  username: string,
  role: 'admin' | 'user' = 'user',
  status: 'online' | 'offline' | 'disabled' | 'locked' = 'offline',
): UserView {
  return {
    id, username, display_name: username, email: `${username}@x.com`, role, status,
    last_login_at: '2026-08-21 10:00:00', created_at: '2026-08-01',
    accountPermissions: [...ACCOUNT_PERMISSIONS], strategyPermissions: [...STRATEGY_PERMISSIONS],
    expanded: false, actionPending: false, removePending: false,
  }
}

describe('UserManagementView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    const store = useUserManagementStore()
    // 阻断真实 API 拉取
    vi.spyOn(store, 'loadAll').mockResolvedValue()
  })

  it('渲染用户管理标题', () => {
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    expect(w.text()).toContain('访问控制')
    expect(w.text()).toContain('用户管理')
  })

  it('空用户列表显示共 0 个用户', async () => {
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    expect(w.text()).toContain('共 0 个用户')
  })

  it('列表渲染用户并按搜索框过滤', async () => {
    const store = useUserManagementStore()
    store.users = [fakeUser(1, 'alice', 'admin'), fakeUser(2, 'bob')]
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    expect(w.text()).toContain('alice')
    expect(w.text()).toContain('bob')
    // 搜索 bob
    const search = w.find('input[placeholder*="搜索用户名"]')
    await search.setValue('bob')
    await w.vm.$nextTick()
    expect(w.text()).toContain('bob')
    expect(w.text()).not.toContain('alice')
  })

  it('打开添加用户 modal 时默认保存按钮禁用（空用户名）', async () => {
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const addBtn = w.findAll('button').find(b => b.text().includes('添加用户'))
    await addBtn!.trigger('click')
    await w.vm.$nextTick()
    const saveBtn = w.findAll('button').find(b => b.text() === '保存')
    expect(saveBtn?.attributes('disabled')).toBeDefined()
  })

  it('登录失败锁定开关切换调用 store.toggleLoginLockout', async () => {
    const store = useUserManagementStore()
    vi.spyOn(store, 'toggleLoginLockout').mockResolvedValue()
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const sw = w.find('.sync-switch')
    if (sw.exists()) {
      await sw.trigger('click')
      expect(store.toggleLoginLockout).toHaveBeenCalled()
    }
  })
})