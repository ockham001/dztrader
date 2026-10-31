import { describe, it, expect, vi, beforeEach, beforeAll } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useUserManagementStore } from '@/stores/userManagement'
import type { UserView } from '@/stores/userManagement'
import { usersApi } from '@/api/users'
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
    actionPending: false, removePending: false,
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

  it('添加用户 Modal 的密码框是 type=password 且不再预填固定弱口令（#202）', async () => {
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    await w.findAll('button').find(b => b.text().includes('添加用户'))!.trigger('click')
    await w.vm.$nextTick()

    const pwdInputs = w.findAll('.modal-stub input[type="password"]')
    expect(pwdInputs.length).toBeGreaterThan(0)
    // 明文框已全部清除：Modal 内不得出现承载口令的 type="text" 输入
    // （用户名/显示名称/邮箱仍是 text，故只针对密码标签后的输入断言类型）
    const pwd = pwdInputs[0]
    expect(pwd.attributes('autocomplete')).toBe('new-password')
    // 旧实现预填 88888888，新实现为空并强制管理员输入
    expect((pwd.element as HTMLInputElement).value).toBe('')
    expect(w.find('.modal-stub').text()).not.toContain('88888888')
    expect(w.find('.modal-stub').text()).toContain('密码长度至少 12 位')
  })

  it('重置密码 Modal 的密码框是 type=password（#202）', async () => {
    const store = useUserManagementStore()
    store.users = [fakeUser(1, 'alice', 'admin')]
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    // 行内「重置密码」按钮是 icon-only（title 承载文案）
    const resetBtn = w.findAll('button[title="重置密码"]')[0]
    expect(resetBtn, '缺少重置密码按钮').toBeTruthy()
    await resetBtn!.trigger('click')
    await w.vm.$nextTick()

    const pwd = w.find('.modal-stub input[type="password"]')
    expect(pwd.exists()).toBe(true)
    expect(pwd.attributes('autocomplete')).toBe('new-password')
    expect(w.find('.modal-stub').text()).toContain('密码长度至少 12 位')
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
    vi.restoreAllMocks()
    setActivePinia(createPinia())
    const store = useUserManagementStore()
    vi.spyOn(store, 'loadAll').mockResolvedValue()
  })

  function mountWithUsers(users: UserView[]): VueWrapper {
    const store = useUserManagementStore()
    store.users = users
    return mount(UserManagementView, { global: { stubs: sharedStubs } })
  }

  async function openRemoveModal(w: VueWrapper, username: string): Promise<void> {
    await w.find(`button[title="删除用户 ${username}"]`).trigger('click')
  }

  it('点击删除用户先弹确认, 取消后不发删除请求且用户保留', async () => {
    const store = useUserManagementStore()
    const spy = vi.spyOn(store, 'removeUser').mockResolvedValue(true)
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await openRemoveModal(w, 'bob')
    expect(spy).not.toHaveBeenCalled()
    // 断言限定在 Modal 内：列表行本身已渲染 bob, 用全量 w.text() 无法发现 Modal 漏用户名
    const modal = w.find('.modal-stub')
    expect(modal.exists()).toBe(true)
    expect(modal.text()).toContain('确认删除用户')
    expect(modal.text()).toContain('bob')
    expect(modal.text()).toContain('不可恢复')
    // #205 权限功能下线：确认文案不再提及权限配置
    expect(modal.text()).toContain('已登录会话会被断开')
    expect(modal.text()).not.toContain('权限配置将一并移除')
    expect(modal.text()).not.toContain('账户/策略权限')

    await w.findAll('button').find(b => b.text() === '取消')!.trigger('click')
    expect(spy).not.toHaveBeenCalled()
    expect(w.find('.modal-stub').exists()).toBe(false)
    expect(store.users.map(u => u.username)).toContain('bob')  // 取消不影响镜像
  })

  it('确认删除后走 store.removeUser, 成功后用户从列表消失且有成功提示', async () => {
    const store = useUserManagementStore()
    // mock API 边界而非 store 本身：用户从镜像消失由真实 store.removeUser 完成
    const removeSpy = vi.spyOn(usersApi, 'remove').mockResolvedValue(undefined)
    const w = mountWithUsers([fakeUser(1, 'alice', 'admin'), fakeUser(2, 'bob')])
    await flushPromises()

    await openRemoveModal(w, 'bob')
    const confirm = w.findAll('button').find(b => b.text() === '删除')!
    expect(confirm.classes()).toContain('ds-btn--danger')
    await confirm.trigger('click')
    await flushPromises()

    expect(removeSpy).toHaveBeenCalledWith(2)
    expect(store.users.map(u => u.username)).not.toContain('bob')
    expect(w.text()).not.toContain('bob')
    expect(w.find('.modal-stub').exists()).toBe(false)
    expect(w.find('.ds-toast').text()).toContain('用户已删除')
  })

  it('删除失败保持 Modal 打开并提示真实错误', async () => {
    const store = useUserManagementStore()
    vi.spyOn(usersApi, 'remove').mockRejectedValue(new Error('boom'))
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await openRemoveModal(w, 'bob')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await flushPromises()

    expect(w.find('.modal-stub').exists()).toBe(true)  // 失败保留 Modal
    expect(store.users.map(u => u.username)).toContain('bob')  // 镜像保留
    // 与兜底文案区分：真实 store.error 含「未删除，请重试」
    expect(w.find('.ds-toast').text()).toContain('未删除，请重试')
  })

  it('删除请求进行中确认/取消按钮禁用并显示删除中', async () => {
    const store = useUserManagementStore()
    let resolveRemove!: (ok: boolean) => void
    vi.spyOn(store, 'removeUser').mockImplementation(
      () => new Promise<boolean>((resolve) => { resolveRemove = resolve }),
    )
    const w = mountWithUsers([fakeUser(2, 'bob')])
    await flushPromises()

    await openRemoveModal(w, 'bob')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await w.vm.$nextTick()

    const pending = w.findAll('button').find(b => b.text() === '删除中…')!
    expect(pending.attributes('disabled')).toBeDefined()
    const cancel = w.findAll('button').find(b => b.text() === '取消')!
    expect(cancel.attributes('disabled')).toBeDefined()

    resolveRemove(false)  // 结束挂起（失败保留 Modal 即可）
    await flushPromises()
    expect(w.find('.modal-stub').exists()).toBe(true)
  })
})

describe('UserManagementView 权限功能下线（#205）', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    setActivePinia(createPinia())
    const store = useUserManagementStore()
    vi.spyOn(store, 'loadAll').mockResolvedValue()
  })

  it('用户列表表头不含账户/策略权限列', async () => {
    const store = useUserManagementStore()
    store.users = [fakeUser(1, 'alice', 'admin')]
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const userTable = w.findAll('table.ds-table').find(t => t.text().includes('用户名'))!
    const headers = userTable.findAll('th').map(th => th.text())
    expect(headers).toEqual(['用户名', '显示名称', '角色', '状态', '最后登录', '操作'])
  })

  it('用户行不再显示「N 个账户 / N 个策略」入口', async () => {
    const store = useUserManagementStore()
    store.users = [fakeUser(1, 'alice', 'admin'), fakeUser(2, 'bob')]
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const userTable = w.findAll('table.ds-table').find(t => t.text().includes('用户名'))!
    const rows = userTable.findAll('tbody tr')
    expect(rows).toHaveLength(2)
    expect(rows[0].findAll('td')).toHaveLength(6)  // 权限两列已移除
    expect(w.text()).not.toMatch(/\d+ 个账户/)
    expect(w.text()).not.toMatch(/\d+ 个策略/)
    expect(w.findAll('button').some(b => /个账户|个策略/.test(b.text()))).toBe(false)
  })

  it('添加用户 Modal 不再渲染权限勾选区', async () => {
    const w = mount(UserManagementView, { global: { stubs: sharedStubs } })
    await flushPromises()
    await w.findAll('button').find(b => b.text().includes('添加用户'))!.trigger('click')
    await w.vm.$nextTick()
    const modal = w.find('.modal-stub')
    expect(modal.exists()).toBe(true)
    // 正向锚点：证明默认 slot 确实渲染（只有否定断言时 slot 整体消失仍会全绿）
    expect(modal.text()).toContain('用户名')
    // 两块权限勾选区（含全选）及其原型选项均已下线
    expect(modal.text()).not.toContain('账户权限')
    expect(modal.text()).not.toContain('策略权限')
    expect(modal.text()).not.toContain('CTP-主力账户')
    expect(modal.text()).not.toContain('IB-美股账户')
    expect(modal.text()).not.toContain('MACD_V3')
  })
})
