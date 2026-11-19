import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import SyncSwitch from '../SyncSwitch.vue'

describe('SyncSwitch', () => {
  it('renders unchecked state', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false },
    })
    expect((wrapper.find('input[type="checkbox"]').element as HTMLInputElement).checked).toBe(false)
  })

  it('renders checked state', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: true },
    })
    expect((wrapper.find('input[type="checkbox"]').element as HTMLInputElement).checked).toBe(true)
  })

  it('emits change event with new value on click', async () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false },
    })
    await wrapper.find('input').setValue(true)
    // change event should emit the new value (true)
    const changeEvents = wrapper.emitted('change')
    expect(changeEvents).toBeTruthy()
    expect(changeEvents![0]).toEqual([true])
  })

  it('does not emit change when pending', async () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false, pending: true },
    })
    const input = wrapper.find('input')
    expect(input.element.disabled).toBe(true)
    await input.trigger('change')
    expect(wrapper.emitted('change')).toBeUndefined()
  })

  it('does not emit change when disabled', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false, disabled: true },
    })
    expect(wrapper.find('input').element.disabled).toBe(true)
  })

  it('applies pending class', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false, pending: true },
    })
    expect(wrapper.find('label').classes()).toContain('ds-switch--pending')
  })

  it('checkbox checked state follows modelValue, not user click', async () => {
    // When user clicks, the component prevents native checkbox from changing
    // and emits 'change' instead. The actual state only changes when parent
    // updates modelValue.
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false },
    })
    const input = wrapper.find('input[type="checkbox"]')
    expect((input.element as HTMLInputElement).checked).toBe(false)

    // Simulate user toggle
    await input.setValue(true)
    // The component resets checkbox.checked to modelValue in onToggle
    // So after the event, it should still be false (parent hasn't updated)
    expect((input.element as HTMLInputElement).checked).toBe(false)
    expect(wrapper.emitted('change')).toBeTruthy()
  })

  // #233：纯图形开关在 label 内没有可读文字，视觉名称由调用方的相邻 span 提供，
  // 读屏读不到 → 必须由 label prop 落到内部 input 的 aria-label 上
  it('把 label prop 透出为 checkbox 的 aria-label（读屏可报出名称）', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false, label: '自动登录' },
    })
    expect(wrapper.find('input[type="checkbox"]').attributes('aria-label')).toBe('自动登录')
  })

  it('未传 label 时不生成 aria-label（避免出现空的无障碍名称）', () => {
    const wrapper = mount(SyncSwitch, {
      props: { modelValue: false },
    })
    expect(wrapper.find('input[type="checkbox"]').attributes('aria-label')).toBeUndefined()
  })
})

// #379：开关的 <input> 曾由 `.ds-switch input { display: none }` 隐藏，控件被摘出无障碍树
// （CDP 实测 activeElement 退回 BODY、AXTree 无 checkbox 角色）。CSS 侧的回归由
// assets/css/__tests__/componentsCss.spec.ts 锁；这里从组件挂载侧锁住**可访问契约**：
// input 真的在 DOM 里、带着名称与禁用态——两者合起来才是「键盘与读屏能操作这个开关」。
describe('SyncSwitch 可访问契约（#379）', () => {
  it('checkbox 作为真实元素挂载（不是被 CSS 移除的装饰），且角色/类型正确', () => {
    const wrapper = mount(SyncSwitch, { props: { modelValue: true, label: '显示全部网卡' } })
    const input = wrapper.find('input[type="checkbox"]')
    expect(input.exists()).toBe(true)
    // 语义保留为 checkbox：视觉是开关，但状态与事件都是 checkbox 语义。
    // 加 role="switch" 需要另配 aria-checked，会与 :checked 重复且易不一致（#379 明确不做）
    expect(input.attributes('role')).toBeUndefined()
    expect((input.element as HTMLInputElement).type).toBe('checkbox')
    expect((input.element as HTMLInputElement).checked).toBe(true)
  })

  it('label prop 落到 aria-label；未传时不产生空名称', () => {
    const named = mount(SyncSwitch, { props: { modelValue: false, label: '实时' } })
    expect(named.find('input[type="checkbox"]').attributes('aria-label')).toBe('实时')
    const unnamed = mount(SyncSwitch, { props: { modelValue: false } })
    expect(unnamed.find('input[type="checkbox"]').attributes('aria-label')).toBeUndefined()
  })

  it('disabled 与 pending 都把 disabled 属性落到 input（读屏能报出不可用）', () => {
    for (const props of [{ disabled: true }, { pending: true }]) {
      const wrapper = mount(SyncSwitch, { props: { modelValue: false, ...props } })
      const input = wrapper.find('input[type="checkbox"]')
      expect(input.attributes('disabled'), JSON.stringify(props)).toBeDefined()
      expect((input.element as HTMLInputElement).disabled).toBe(true)
    }
  })

  it('开关本体（焦点环归属的视觉控件）始终渲染', () => {
    // 焦点环画在 .ds-switch 上（input 只有 1×1 且被 clip-path 裁掉）。若视觉块消失，
    // 焦点环会画在一个看不见的元素上——等于没有焦点指示。
    const wrapper = mount(SyncSwitch, { props: { modelValue: false, label: '自动登录' } })
    expect(wrapper.find('.ds-switch').exists()).toBe(true)
    expect(wrapper.find('.ds-switch__track').exists()).toBe(true)
    expect(wrapper.find('.ds-switch__thumb').exists()).toBe(true)
  })
})
