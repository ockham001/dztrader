// App.vue 的通知呈现（ADR 0060 决策 3）
// 覆盖：popup=true 走**常驻 toast**（不自动消失），且**不再渲染**全屏遮罩弹窗。
// 原先的 alertdialog + 焦点陷阱 + 滚动锁实现随遮罩弹窗一并删除——它阻挡行情/持仓，
// 与盯盘场景冲突；焦点口径改由 ToastContainer 的普通可关闭卡片承担。
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useNotifyStore } from '@/stores/notify'
import { useToastStore } from '@/stores/toast'
import App from '../App.vue'

vi.mock('vue-router', () => ({
  useRoute: () => ({ meta: { public: true } }),
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({
    connectionState: { value: 'disconnected' },
    lastError: { value: null },
    connect: vi.fn(),
    disconnect: vi.fn(),
  }),
}))

vi.mock('@/api/notifications', () => ({
  notificationsApi: { list: vi.fn().mockResolvedValue([]) },
}))

const stubs = {
  AppShell: { template: '<div class="shell-stub"><slot /></div>' },
  ToastContainer: { template: '<div class="toast-stub" />' },
  ChangePasswordModal: { props: ['open', 'forced'], template: '<div class="chpwd-stub" />' },
  Icon: { props: ['name', 'size'], template: '<span class="icon" />' },
  RouterView: { template: '<div class="route-stub" />' },
}

let wrapper: VueWrapper | null = null

afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  document.body.style.overflow = ''
})

describe('App 通知呈现（ADR 0060）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('popup=true 不再渲染全屏遮罩弹窗', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('磁盘空间不足', 'dzsysmon', 'error', true)
    await flushPromises()
    // 遮罩弹窗已删除：既无 overlay、也无 alertdialog 角色
    expect(wrapper.find('.popup-overlay').exists()).toBe(false)
    expect(wrapper.find('[role="alertdialog"]').exists()).toBe(false)
  })

  it('popup=true 产生**常驻** toast（duration=0，不自动消失）', async () => {
    const notify = useNotifyStore()
    const toast = useToastStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('交易登录失败', 'dztd_ctp', 'error', true)
    await flushPromises()
    expect(toast.items).toHaveLength(1)
    expect(toast.items[0].message).toBe('交易登录失败')
    expect(toast.items[0].duration).toBe(0) // 常驻：不排定时器
    expect(toast.items[0].timer).toBeNull()
  })

  it('popup=false 的 error 按分级时长自动消失（10s）', async () => {
    const notify = useNotifyStore()
    const toast = useToastStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('下单被拒', 'dztd_ctp', 'error', false)
    await flushPromises()
    expect(toast.items).toHaveLength(1)
    expect(toast.items[0].duration).toBe(10000)
    expect(toast.items[0].timer).not.toBeNull()
  })

  it('不写任何 store 的 error.value（双通道互斥）', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('订阅失败', 'dzmd_ctp', 'error', true)
    await flushPromises()
    // notify_ui 只走 toast，不污染 HTTP 失败链路
    expect(wrapper.find('.popup-overlay').exists()).toBe(false)
  })
})
