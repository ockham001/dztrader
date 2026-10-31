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
