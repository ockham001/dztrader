// 共享 Modal 的对话框语义与焦点管理（#206 A）
// 覆盖：role/aria-modal/aria-labelledby、关闭按钮可访问名、打开聚焦、Tab 循环、
//       Esc 关闭、关闭后焦点还原到触发元素、body 滚动锁定与卸载恢复。
import { describe, it, expect, afterEach } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import Modal from '../Modal.vue'

const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" />' },
}

function mountModal(open = true, title = '测试弹窗') {
  return mount(Modal, {
    props: { open, title },
    slots: {
      default: '<button type="button" class="body-action">正文动作</button>',
      footer: '<button type="button" class="foot-action">确定</button>',
    },
    global: { stubs },
    attachTo: document.body,
  })
}

let wrapper: VueWrapper | null = null

afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  document.body.style.overflow = ''
})

describe('Modal 对话框语义（#206 A）', () => {
  it('面板带 role=dialog / aria-modal，且 aria-labelledby 指向标题元素', async () => {
    wrapper = mountModal(true, '确认删除')
    await nextTick()
    const dialog = wrapper.find('[role="dialog"]')
    expect(dialog.exists()).toBe(true)
    expect(dialog.attributes('aria-modal')).toBe('true')

    const labelId = dialog.attributes('aria-labelledby')
    expect(labelId, 'aria-labelledby 缺失').toBeTruthy()
    // 关联必须真的落到标题文本上，空指针同样算失败
    const titleEl = wrapper.find(`#${labelId}`)
    expect(titleEl.exists()).toBe(true)
    expect(titleEl.text()).toBe('确认删除')
  })

  it('关闭按钮有可访问名（不是空标签按钮）', async () => {
    wrapper = mountModal()
    await nextTick()
    const close = wrapper.find('.ds-dialog__close')
    expect(close.attributes('aria-label')).toBe('关闭')
  })

  it('打开时把焦点移入对话框（首个可聚焦元素）', async () => {
    const trigger = document.createElement('button')
    trigger.textContent = '打开'
    document.body.appendChild(trigger)
    trigger.focus()
    expect(document.activeElement).toBe(trigger)

    wrapper = mountModal(false)
    await wrapper.setProps({ open: true })
    await nextTick()
    await nextTick()

    // 焦点必须离开触发器进入对话框内部
    const dialog = wrapper.find('[role="dialog"]').element
    expect(dialog.contains(document.activeElement)).toBe(true)
    trigger.remove()
  })

  it('关闭后焦点还原到打开它的触发元素', async () => {
    const trigger = document.createElement('button')
    trigger.textContent = '打开'
    document.body.appendChild(trigger)
    trigger.focus()

    wrapper = mountModal(false)
    await wrapper.setProps({ open: true })
    await nextTick()
    await nextTick()
    expect(document.activeElement).not.toBe(trigger)

    await wrapper.setProps({ open: false })
    await nextTick()
    await nextTick()
    expect(document.activeElement).toBe(trigger)
    trigger.remove()
  })

  it('Tab 在对话框内循环：最后一个元素 Tab 回到第一个，第一个 Shift+Tab 到最后一个', async () => {
    wrapper = mountModal()
    await nextTick()
    await nextTick()
    const dialog = wrapper.find('[role="dialog"]').element
    const focusables = Array.from(
      dialog.querySelectorAll<HTMLElement>('button:not([disabled])'),
    )
    expect(focusables.length).toBeGreaterThanOrEqual(2)
    const first = focusables[0]
    const last = focusables[focusables.length - 1]

    last.focus()
    await triggerKey(document, 'Tab', false)
    expect(document.activeElement).toBe(first)

    first.focus()
    await triggerKey(document, 'Tab', true)
    expect(document.activeElement).toBe(last)

    // 中间元素不被打断：事件不得被 preventDefault，否则浏览器默认的 Tab 前进会被吞掉
    // （jsdom 不实现 Tab 移动焦点，只看焦点是否变化是恒真的，必须断言 defaultPrevented）
    const mid = focusables[1]
    mid.focus()
    const midEvent = new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true })
    document.dispatchEvent(midEvent)
    await nextTick()
    expect(midEvent.defaultPrevented).toBe(false)
  })

  it('Esc 关闭对话框（键盘等效路径）', async () => {
    wrapper = mountModal()
    await nextTick()
    const event = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })
    document.dispatchEvent(event)
    await nextTick()
    expect(wrapper.emitted('close')).toHaveLength(1)
  })

  it('打开锁定 body 滚动，关闭与卸载都恢复', async () => {
    wrapper = mountModal(true)
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')

    await wrapper.setProps({ open: false })
    await nextTick()
    expect(document.body.style.overflow).toBe('')

    await wrapper.setProps({ open: true })
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')
    // 打开状态下卸载（如路由切换）不得把整站滚死
    wrapper.unmount()
    wrapper = null
    expect(document.body.style.overflow).toBe('')
  })

  it('关闭时不渲染面板（Esc / 遮罩逻辑不受影响）', async () => {
    wrapper = mountModal(false)
    await nextTick()
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    // 遮罩点击关闭仍然有效
    await wrapper.setProps({ open: true })
    await nextTick()
    await wrapper.find('.modal-overlay').trigger('click')
    expect(wrapper.emitted('close')).toHaveLength(1)
  })
})

/** 派发一个带默认行为的 keydown（jsdom 不会真的移动焦点，由组件自己拦截循环） */
async function triggerKey(target: EventTarget, key: string, shiftKey: boolean): Promise<void> {
  target.dispatchEvent(
    new KeyboardEvent('keydown', { key, shiftKey, bubbles: true, cancelable: true }),
  )
  await nextTick()
}
