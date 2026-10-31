// App.vue 的 notify 弹窗焦点管理（#206 A）
// 覆盖：alertdialog 语义保留、消息与弹窗关联、打开时焦点移入、确认后焦点还原。
// Modal 的通用焦点陷阱由 shared/__tests__/Modal.spec.ts 覆盖，此处只验证 App 把
// 同一口径接到了 notify 弹窗上（原先是「两套标准」：有 alertdialog 但无焦点管理）。
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import { createPinia, setActivePinia } from 'pinia'
import { useNotifyStore } from '@/stores/notify'
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

describe('App 通知弹窗焦点管理（#206 A）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('弹窗保留 alertdialog 语义，消息元素与弹窗关联', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('磁盘空间不足', 'dzsysmon', 'error', true)
    await flushPromises()

    const dialog = wrapper.find('[role="alertdialog"]')
    expect(dialog.exists()).toBe(true)
    expect(dialog.attributes('aria-modal')).toBe('true')
    const labelId = dialog.attributes('aria-labelledby')
    expect(labelId, 'aria-labelledby 缺失').toBeTruthy()
    const msgEl = wrapper.find(`#${labelId}`)
    expect(msgEl.exists()).toBe(true)
    expect(msgEl.text()).toBe('磁盘空间不足')
  })

  it('弹窗打开时焦点移入，确认后焦点还原到打开前的元素', async () => {
    const notify = useNotifyStore()
    const opener = document.createElement('button')
    opener.textContent = '触发按钮'
    document.body.appendChild(opener)
    opener.focus()

    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    await flushPromises()
    expect(document.activeElement).toBe(opener)

    notify.push('磁盘空间不足', 'dzsysmon', 'error', true)
    await flushPromises()
    await nextTick()
    await nextTick()

    const popup = wrapper.find('.popup-modal').element
    expect(popup.contains(document.activeElement)).toBe(true)

    await wrapper.find('.popup-modal__action').trigger('click')
    await flushPromises()
    await nextTick()
    await nextTick()

    // 出队后焦点回到打开它的按钮，而不是丢到 body
    expect(document.activeElement).toBe(opener)
    opener.remove()
  })

  it('无 popup 消息时不渲染弹窗', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('只是一条 info', 'dzweb', 'info', false)
    await flushPromises()
    expect(wrapper.find('[role="alertdialog"]').exists()).toBe(false)
  })
})

// Esc 通道（#206）：notify 弹窗与 Modal 共用同一焦点陷阱栈。
// 若 alertdialog 只压栈、不提供 Esc 关闭自己的通道，那么它打开期间下层 Modal 的
// Esc 会被 isTopmost() 判为「不是最上层」而静默失效——出现「按 Esc 什么都不发生」。
// 本组用例钉住：notify 在上层时 Esc 确认通知（逐条出队），而不是死窗口。
describe('App 通知弹窗的 Esc 通道（#206）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('notify 弹窗打开时按 Esc 确认当前条（等价于点确认），不会静默无响应', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('磁盘空间不足', 'dzsysmon', 'error', true)
    await flushPromises()
    await nextTick()
    expect(wrapper.find('[role="alertdialog"]').exists()).toBe(true)

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }))
    await flushPromises()
    await nextTick()

    // 出队：弹窗消失（ackPopup 的效果），不是「按了没反应」
    expect(wrapper.find('[role="alertdialog"]').exists()).toBe(false)
  })

  it('多条通知时 Esc 逐条出队，一次只确认一条', async () => {
    const notify = useNotifyStore()
    wrapper = mount(App, { global: { stubs }, attachTo: document.body })
    notify.push('第一条', 'dzsysmon', 'error', true)
    notify.push('第二条', 'dzweb', 'error', true)
    await flushPromises()
    await nextTick()
    expect(wrapper.find('.popup-modal__msg').text()).toBe('第一条')

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }))
    await flushPromises()
    await nextTick()

    // 仍在展示第二条：一次 Esc 只确认一条
    expect(wrapper.find('[role="alertdialog"]').exists()).toBe(true)
    expect(wrapper.find('.popup-modal__msg').text()).toBe('第二条')
  })
})
