// 自助改密弹窗（契约 rest §2.1/§2.2）
// 覆盖：旧密码必填、新口令最小长度、两次输入一致、契约错误码映射、成功后必须重新登录。
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { ApiError } from '@/api/client'
import { useAuthStore } from '@/stores/auth'
import ChangePasswordModal from '../ChangePasswordModal.vue'

const push = vi.fn()
vi.mock('vue-router', () => ({
  useRouter: () => ({ push: (...args: unknown[]) => push(...args) }),
}))

const mockedChangePassword = vi.fn()
vi.mock('@/api/auth', () => ({
  authApi: {
    changePassword: (...args: unknown[]) => mockedChangePassword(...args),
    login: vi.fn(),
    ackDefaultPassword: vi.fn(),
  },
}))

const stubs = {
  Modal: {
    props: ['open', 'title'],
    emits: ['close'],
    template: '<div v-if="open" class="modal-stub"><slot /><div class="footer-stub"><slot name="footer" /></div></div>',
  },
}

function mountModal(open = true, forced = false) {
  return mount(ChangePasswordModal, {
    props: { open, forced },
    global: { stubs },
  })
}

async function fill(w: ReturnType<typeof mountModal>, oldPwd: string, newPwd: string, confirm = newPwd) {
  await w.find('[data-test="old-password"]').setValue(oldPwd)
  await w.find('[data-test="new-password"]').setValue(newPwd)
  await w.find('[data-test="confirm-password"]').setValue(confirm)
}

describe('ChangePasswordModal', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    localStorage.clear()
  })

  it('旧密码为空时本地拦截，不发请求', async () => {
    const w = mountModal()
    await fill(w, '', 'NewStrongPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(mockedChangePassword).not.toHaveBeenCalled()
    expect(w.text()).toContain('请输入旧密码')
  })

  it('新口令短于最小长度时本地拦截（契约 rest §1 密码策略）', async () => {
    const w = mountModal()
    await fill(w, 'old-password', 'short11char')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(mockedChangePassword).not.toHaveBeenCalled()
    expect(w.text()).toContain('密码长度至少 12 位')
  })

  it('两次输入不一致时本地拦截', async () => {
    const w = mountModal()
    await fill(w, 'old-password', 'NewStrongPass1', 'DifferentPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(mockedChangePassword).not.toHaveBeenCalled()
    expect(w.text()).toContain('两次输入的密码不一致')
  })

  it('提交旧密码与新口令（契约 rest §2.1）', async () => {
    mockedChangePassword.mockResolvedValue({ ok: true })
    const w = mountModal()
    await fill(w, 'old-password', 'NewStrongPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(mockedChangePassword).toHaveBeenCalledWith('old-password', 'NewStrongPass1')
  })

  it('成功后清会话并回登录页（契约 rest §1：改密吊销当前会话）', async () => {
    mockedChangePassword.mockResolvedValue({ ok: true })
    const auth = useAuthStore()
    auth.token = 'jwt-old'
    auth.mustChangePassword = true
    localStorage.setItem('jwt_token', 'jwt-old')

    const w = mountModal()
    await fill(w, 'old-password', 'NewStrongPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()

    expect(auth.token).toBeNull()
    expect(auth.mustChangePassword).toBe(false)
    expect(localStorage.getItem('jwt_token')).toBeNull()
    expect(push).toHaveBeenCalledWith('/login')
  })

  it('password_too_weak：按 code 映射中文，不回显英文 error', async () => {
    mockedChangePassword.mockRejectedValue(
      new ApiError(400, { error: 'password too weak', code: 'password_too_weak' }),
    )
    const w = mountModal()
    await fill(w, 'old-password', 'NewStrongPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(w.text()).toContain('密码长度至少 12 位')
    expect(w.text()).not.toContain('password too weak')
  })

  it('invalid_old_password：按 code 映射中文（契约 rest §1 自助改密须校验旧密码）', async () => {
    mockedChangePassword.mockRejectedValue(
      new ApiError(400, { error: 'invalid old password', code: 'invalid_old_password' }),
    )
    const w = mountModal()
    await fill(w, 'wrong-old', 'NewStrongPass1')
    await w.find('[data-test="submit-password"]').trigger('click')
    await flushPromises()
    expect(w.text()).toContain('旧密码不正确')
    expect(w.text()).not.toContain('invalid old password')
  })

  it('强制改密：显示提示且不给「取消」按钮', async () => {
    const w = mountModal(true, true)
    expect(w.find('[data-test="force-change-hint"]').exists()).toBe(true)
    const buttons = w.findAll('button').map(b => b.text())
    expect(buttons).not.toContain('取消')
    expect(buttons).toContain('登出')
  })
})
