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

// stub 子组件（Modal 模拟真实 v-if="open"：未打开时 slot 不渲染，避免"即使 open 逻辑坏掉也通过"）
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], emits: ['close'], template: '<div v-if="open" class="modal-stub"><slot /></div><div v-if="open" class="modal-footer-stub"><slot name="footer" /></div>' },
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

  it('登录历史失败原因与登录页共用中文映射（未知 reason 原样回退）', async () => {
    const store = useUserManagementStore()
    store.loginHistory = [
      { id: 1, username: 'alice', ip: '1.2.3.4', success: false, reason: 'invalid_credentials', created_at: '2026-08-21 10:00:00' },
      { id: 2, username: 'bob', ip: '1.2.3.5', success: false, reason: 'custom_reason', created_at: '2026-08-21 10:01:00' },
    ]
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    // 契约 rest §2.1: 机器码 → 中文（与登录页同源），不展示 snake_case
    expect(w.text()).toContain('用户名或密码错误')
    expect(w.text()).not.toContain('invalid_credentials')
    // 未知 reason 保持原样回退（登录历史可能含后端新增/历史遗留 code）
    expect(w.text()).toContain('custom_reason')
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
    // Modal 未打开时 slot 不应渲染
    expect(w.findAll('button').find(b => b.text() === '保存')).toBeUndefined()

    const addBtn = w.findAll('button').find(b => b.text().includes('添加用户'))
    await addBtn!.trigger('click')
    await w.vm.$nextTick()
    const saveBtn = w.findAll('button').find(b => b.text() === '保存')
    expect(saveBtn?.attributes('disabled')).toBeDefined()
  })

  it('登录失败锁定开关切换调用 store.toggleLoginLockout，并以当前镜像值下发', async () => {
    const store = useUserManagementStore()
    const spy = vi.spyOn(store, 'toggleLoginLockout').mockResolvedValue()
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const sw = w.find('.sync-switch')
    expect(sw.exists()).toBe(true)

    await sw.trigger('click')
    expect(spy).toHaveBeenCalledWith(true)

    // 模拟异步下发成功后镜像回推 true，再次点击应下发 false
    store.securityConfig.login_lockout_enabled = true
    await w.vm.$nextTick()
    await sw.trigger('click')
    expect(spy).toHaveBeenLastCalledWith(false)
    expect(spy).toHaveBeenCalledTimes(2)
  })
})

describe('UserManagementView 删除用户二次确认（#204）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    const store = useUserManagementStore()
    vi.spyOn(store, 'loadAll').mockResolvedValue()
  })

  function mountWithUsers(users: UserView[]) {
    const store = useUserManagementStore()
    store.users = users
    return mount(UserManagementView, { global: { stubs: sharedStubs } })
  }

  it('点击删除用户先弹确认, 取消后不发删除请求且用户保留', async () => {
    const store = useUserManagementStore()
    const spy = vi.spyOn(store, 'removeUser').mockResolvedValue(true)
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await w.find('button[title="删除用户 bob"]').trigger('click')
    expect(spy).not.toHaveBeenCalled()
    expect(w.text()).toContain('确认删除用户')
    expect(w.text()).toContain('bob')
    expect(w.text()).toContain('不可恢复')

    await w.findAll('button').find(b => b.text() === '取消')!.trigger('click')
    expect(spy).not.toHaveBeenCalled()
    expect(w.text()).not.toContain('确认删除用户')
    expect(w.text()).toContain('bob')
  })

  it('确认删除后调用 removeUser, 成功后用户消失且有成功提示', async () => {
    const store = useUserManagementStore()
    const spy = vi.spyOn(store, 'removeUser').mockImplementation(async (id: number) => {
      store.users = store.users.filter(u => u.id !== id)
      return true
    })
    const w = mountWithUsers([fakeUser(1, 'alice', 'admin'), fakeUser(2, 'bob')])
    await flushPromises()

    await w.find('button[title="删除用户 bob"]').trigger('click')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await flushPromises()

    expect(spy).toHaveBeenCalledWith(2)
    expect(w.text()).not.toContain('bob')
    expect(w.text()).not.toContain('确认删除用户')
    expect(w.find('.ds-toast').text()).toContain('用户已删除')
  })

  it('删除失败保持 Modal 打开并提示错误', async () => {
    const store = useUserManagementStore()
    vi.spyOn(store, 'removeUser').mockImplementation(async () => {
      store.error = '删除用户失败（未删除，请重试）'
      return false
    })
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await w.find('button[title="删除用户 bob"]').trigger('click')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await flushPromises()

    expect(w.text()).toContain('确认删除用户')  // 失败保留 Modal
    expect(w.text()).toContain('bob')
    expect(w.find('.ds-toast').text()).toContain('删除用户失败')
  })

  it('删除请求进行中确认按钮禁用并显示删除中', async () => {
    const store = useUserManagementStore()
    let resolveRemove!: (ok: boolean) => void
    vi.spyOn(store, 'removeUser').mockImplementation(
      () => new Promise<boolean>((resolve) => { resolveRemove = resolve }),
    )
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await w.find('button[title="删除用户 bob"]').trigger('click')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await w.vm.$nextTick()

    const pending = w.findAll('button').find(b => b.text() === '删除中…')!
    expect(pending.attributes('disabled')).toBeDefined()

    resolveRemove(false)  // 结束挂起（失败保留 Modal 即可）
    await flushPromises()
    expect(w.text()).toContain('确认删除用户')
  })
})