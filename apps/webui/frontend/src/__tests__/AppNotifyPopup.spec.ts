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
