import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import { ApiError } from '@/api/client'
import LoginView from '../LoginView.vue'

// mock vue-router：LoginView 登录成功 push('/')，失败不发 push
const pushMock = vi.fn()
vi.mock('vue-router', () => ({
  useRouter: () => ({ push: pushMock }),
}))

// mock ThemeDropdown / Icon 子组件（避免引入布局与图标渲染细节）
vi.mock('@/components/layout/ThemeDropdown.vue', () => ({
  default: { name: 'ThemeDropdown', template: '<div class="theme-dropdown" />' },
}))
vi.mock('@/components/shared/Icon.vue', () => ({
  default: { name: 'Icon', props: { name: String, size: [Number, String] }, template: '<span class="icon" />' },
}))

describe('LoginView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    pushMock.mockClear()
    sessionStorage.clear()
    localStorage.clear()
  })

  it('渲染用户名/密码输入与登录按钮', () => {
    const w = mount(LoginView)
    expect(w.find('input#username').exists()).toBe(true)
    expect(w.find('input#password').exists()).toBe(true)
    expect(w.text()).toContain('登录')
  })

  it('用户名或密码为空时不调用 auth.login 并提示', async () => {
    const auth = useAuthStore()
    const spy = vi.spyOn(auth, 'login')
    const w = mount(LoginView)
    await w.find('form').trigger('submit')
    expect(spy).not.toHaveBeenCalled()
    expect(w.text()).toContain('请输入用户名和密码')
  })

  it('登录成功时跳转首页并清 last_username', async () => {
    sessionStorage.setItem('last_username', 'alice')
    const auth = useAuthStore()
    const loginSpy = vi.spyOn(auth, 'login')
      .mockResolvedValue({ token: 't', user: { username: 'alice' }, expires_in: 3600 } as never)
    const w = mount(LoginView)
    await w.find('input#username').setValue('alice')
    await w.find('input#password').setValue('pw')
    await w.find('form').trigger('submit')
    await flushPromises()
    expect(loginSpy).toHaveBeenCalledWith({ username: 'alice', password: 'pw' })
    expect(pushMock).toHaveBeenCalledWith('/')
    expect(sessionStorage.getItem('last_username')).toBeNull()
  })

  it('登录失败（错误凭证）显示错误且保留 last_username', async () => {
    const auth = useAuthStore()
    vi.spyOn(auth, 'login').mockRejectedValue(
      new ApiError(401, { error: '用户名或密码错误' }),
    )
    const w = mount(LoginView)
    await w.find('input#username').setValue('alice')
    await w.find('input#password').setValue('wrong')
    await w.find('form').trigger('submit')
    await flushPromises()
    expect(pushMock).not.toHaveBeenCalled()
    expect(w.text()).toContain('用户名或密码错误')
    expect(sessionStorage.getItem('last_username')).toBe('alice')
  })

  it('邮箱被锁定（account_locked）时显示剩余分钟警告', async () => {
    const auth = useAuthStore()
    vi.spyOn(auth, 'login').mockRejectedValue(
      new ApiError(401, { error: '账号已锁定', code: 'account_locked', locked_until: Math.floor(Date.now() / 1000) + 300 }),
    )
    const w = mount(LoginView)
    await w.find('input#username').setValue('bob')
    await w.find('input#password').setValue('pw')
    await w.find('form').trigger('submit')
    await flushPromises()
    expect(w.text()).toContain('剩余')
    expect(w.text()).toContain('分钟')
  })

  it('网络错误显示网络错误提示', async () => {
    const auth = useAuthStore()
    vi.spyOn(auth, 'login').mockRejectedValue(new TypeError('Failed to fetch'))
    const w = mount(LoginView)
    await w.find('input#username').setValue('alice')
    await w.find('input#password').setValue('pw')
    await w.find('form').trigger('submit')
    await flushPromises()
    expect(w.text()).toContain('网络错误')
  })

  it('点击眼睛图标切换密码可见性', async () => {
    const w = mount(LoginView)
    const pwd = w.find('input#password')
    expect((pwd.element as HTMLInputElement).type).toBe('password')
    const toggle = w.find('button[aria-label="显示密码"]')
    await toggle.trigger('click')
    await w.vm.$nextTick()
    expect((w.find('input#password').element as HTMLInputElement).type).toBe('text')
  })
})
// #207 C5：原生 required + 首屏聚焦用户名（SPA 下 autofocus 不可靠，用 onMounted focus）
describe('LoginView 表单可用性（#207 C5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    pushMock.mockClear()
    sessionStorage.clear()
    localStorage.clear()
  })

  it('用户名/密码输入带 required（空提交有原生提示，JS 文案仍保留）', () => {
    const w = mount(LoginView, { attachTo: document.body })
    expect(w.find('input#username').attributes('required')).toBeDefined()
    expect(w.find('input#password').attributes('required')).toBeDefined()
    w.unmount()
  })

  it('首屏焦点在用户名输入框', async () => {
    const w = mount(LoginView, { attachTo: document.body })
    await flushPromises()
    expect(document.activeElement).toBe(w.find('input#username').element)
    w.unmount()
  })

  it('已有回填用户名时同样聚焦用户名（不是密码框）', async () => {
    sessionStorage.setItem('last_username', 'alice')
    const w = mount(LoginView, { attachTo: document.body })
    await flushPromises()
    expect((w.find('input#username').element as HTMLInputElement).value).toBe('alice')
    expect(document.activeElement).toBe(w.find('input#username').element)
    w.unmount()
  })
})
