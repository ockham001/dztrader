// body 滚动锁引用计数（#206 A）
// 回归点：两个模态同时打开时，先关闭的那个**不能**解掉后关闭者的锁。
// 原实现在每个实例里直接写 `body.style.overflow = ''`，后关闭者会把仍在打开的
// 那个的锁一起解掉（背景恢复滚动，且无任何报错）。
import { describe, it, expect, afterEach } from 'vitest'
import { defineComponent, h, ref } from 'vue'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import Modal from '../Modal.vue'

const stubs = { Icon: { props: ['name', 'size'], template: '<span class="icon" />' } }

/** 宿主组件：用两个独立开关挂两个 Modal，模拟嵌套/并存的对话框 */
const Host = defineComponent({
  setup() {
    const a = ref(false)
    const b = ref(false)
    return { a, b }
  },
  render() {
    return h('div', [
      h(Modal, { open: this.a, title: 'A', onClose: () => { this.a = false } }),
      h(Modal, { open: this.b, title: 'B', onClose: () => { this.b = false } }),
    ])
  },
})

let wrapper: VueWrapper | null = null

afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  document.body.style.overflow = ''
})

describe('body 滚动锁引用计数（#206）', () => {
  it('单实例：打开锁、关闭还原', async () => {
    wrapper = mount(Host, { global: { stubs }, attachTo: document.body })
    expect(document.body.style.overflow).toBe('')

    ;(wrapper.vm as unknown as { a: boolean }).a = true
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')

    ;(wrapper.vm as unknown as { a: boolean }).a = false
    await nextTick()
    expect(document.body.style.overflow).toBe('')
  })

  it('两实例并存：先关一个不解后关者的锁，最后一个关闭才还原', async () => {
    wrapper = mount(Host, { global: { stubs }, attachTo: document.body })
    const vm = wrapper.vm as unknown as { a: boolean; b: boolean }

    vm.a = true
    await nextTick()
    vm.b = true
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')

    // 先关 A：B 仍打开，锁必须保持
    vm.a = false
    await nextTick()
    expect(document.body.style.overflow, '先关闭的模态解掉了后关闭者的锁').toBe('hidden')

    // 再关 B：这才真正还原
    vm.b = false
    await nextTick()
    expect(document.body.style.overflow).toBe('')
  })

  it('打开状态下卸载实例：释放自己的锁，不泄漏到整站', async () => {
    wrapper = mount(Host, { global: { stubs }, attachTo: document.body })
    const vm = wrapper.vm as unknown as { a: boolean; b: boolean }
    vm.a = true
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')

    wrapper.unmount()
    wrapper = null
    expect(document.body.style.overflow).toBe('')
  })

  it('还原到进入锁定前的内联值（而不是无条件清空）', async () => {
    document.body.style.overflow = 'scroll'
    wrapper = mount(Host, { global: { stubs }, attachTo: document.body })
    const vm = wrapper.vm as unknown as { a: boolean }

    vm.a = true
    await nextTick()
    expect(document.body.style.overflow).toBe('hidden')

    vm.a = false
    await nextTick()
    expect(document.body.style.overflow).toBe('scroll')
  })
})
