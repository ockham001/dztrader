import { describe, it, expect } from 'vitest'
import { nextTick } from 'vue'
import { mount } from '@vue/test-utils'
import SyncSelect from '../SyncSelect.vue'

describe('SyncSelect', () => {
  const items = [
    { value: 'a', label: '选项A' },
    { value: 'b', label: '选项B' },
    { value: 'c', label: '选项C' },
  ]

  it('renders trigger with selected label', () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'b' },
    })
    expect(wrapper.find('.ds-sync-select__label').text()).toBe('选项B')
  })

  it('renders placeholder when no value', () => {
    const wrapper = mount(SyncSelect, {
      props: { items, placeholder: '请选择' },
    })
    expect(wrapper.find('.ds-sync-select__label').text()).toBe('请选择')
  })

  it('opens menu on trigger click', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a' },
    })
    expect(wrapper.find('.ds-sync-select__menu').exists()).toBe(false)
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    expect(wrapper.find('.ds-sync-select__menu').exists()).toBe(true)
  })

  it('仅在打开时 emit open（关闭不重复触发）', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a' },
    })
    const trigger = wrapper.find('.ds-sync-select__trigger')
    await trigger.trigger('click')
    expect(wrapper.emitted('open')).toHaveLength(1)

    await trigger.trigger('click')  // 关闭
    expect(wrapper.emitted('open')).toHaveLength(1)

    await trigger.trigger('click')  // 再次打开
    expect(wrapper.emitted('open')).toHaveLength(2)
  })

  it('点击组件外部关闭菜单', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a' },
    })
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    expect(wrapper.find('.ds-sync-select__menu').exists()).toBe(true)

    document.body.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    await nextTick()
    expect(wrapper.find('.ds-sync-select__menu').exists()).toBe(false)
    wrapper.unmount()
  })

  it('emits change with value on item select', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a' },
    })
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    const menuItems = wrapper.findAll('.ds-sync-select__item')
    await menuItems[1].trigger('click') // 选项B

    const changeEvents = wrapper.emitted('change')
    expect(changeEvents).toBeTruthy()
    expect(changeEvents![0]).toEqual(['b'])
  })

  it('does not open when pending', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a', pending: true },
    })
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    expect(wrapper.find('.ds-sync-select__menu').exists()).toBe(false)
  })

  it('does not open when disabled', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a', disabled: true },
    })
    expect((wrapper.find('.ds-sync-select__trigger').element as HTMLButtonElement).disabled).toBe(true)
  })

  it('shows spinner when pending', () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a', pending: true },
    })
    expect(wrapper.find('.ds-sync-select__spinner').exists()).toBe(true)
  })

  it('marks selected item with is-selected class', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'b' },
    })
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    const menuItems = wrapper.findAll('.ds-sync-select__item')
    expect(menuItems[1].classes()).toContain('is-selected')
  })

  it('does not emit change when selecting same value', async () => {
    const wrapper = mount(SyncSelect, {
      props: { items, modelValue: 'a' },
    })
    await wrapper.find('.ds-sync-select__trigger').trigger('click')
    const menuItems = wrapper.findAll('.ds-sync-select__item')
    await menuItems[0].trigger('click') // 选项A (same as current)
    expect(wrapper.emitted('change')).toBeFalsy()
  })
})
