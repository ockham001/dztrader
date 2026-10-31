// body 滚动锁引用计数（#206 A）
// 回归点：两个模态同时打开时，先关闭的那个**不能**解掉后关闭者的锁。
// 原实现在每个实例里直接写 `body.style.overflow = ''`，后关闭者会把仍在打开的
// 那个的锁一起解掉（背景恢复滚动，且无任何报错）。
import { describe, it, expect, afterEach } from 'vitest'
import { defineComponent, h, ref } from 'vue'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import Modal from '../Modal.vue'
import { activeScrollLockCount, resetScrollLockForTest } from '@/composables/useScrollLock'

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
  // 计数器是模块级单例：先记下是否泄漏（把泄漏变成一条明确失败），
  // 再**强制复位**——否则本用例中途失败留下的脏计数会让后续用例级联误判。
  const leaked = activeScrollLockCount()
  resetScrollLockForTest()
  expect(leaked, '用例结束时仍有未释放的滚动锁').toBe(0)
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

// 嵌套/并存对话框的 Esc 作用域（#206）：一次 Esc 只关最上层。
// 若每层各自无条件响应，按一次会同时关掉多层，键盘用户无法预期。
// 复用上面的 Host 宿主（两个开关控制两个 Modal），避免在一个文件里定义第二个组件。
describe('多个对话框并存时的 Esc 作用域（#206）', () => {
  it('一次 Esc 只关闭最上层对话框', async () => {
    // Host 的 onClose 只改自己的开关，故直接观察「哪一层还在渲染」
    const w = mount(Host, { global: { stubs }, attachTo: document.body })
    wrapper = w
    const vm = w.vm as unknown as { a: boolean; b: boolean }

    vm.a = true
    await nextTick()
    vm.b = true
    await nextTick()
    expect(w.findAll('[role="dialog"]')).toHaveLength(2)

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }))
    await nextTick()

    // 只关最后打开的 B：A 的对话框仍在
    expect(vm.a).toBe(true)
    expect(vm.b).toBe(false)
    const remaining = w.findAll('[role="dialog"]')
    expect(remaining).toHaveLength(1)
    // 剩下的是 A（标题已与面板关联，读得到名字）
    expect(w.find('[role="dialog"]').attributes('aria-labelledby')).toBeTruthy()
    expect(w.text()).toContain('A')

    // 再按一次 Esc 才关 A
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }))
    await nextTick()
    expect(vm.a).toBe(false)
    expect(w.findAll('[role="dialog"]')).toHaveLength(0)
  })
})
