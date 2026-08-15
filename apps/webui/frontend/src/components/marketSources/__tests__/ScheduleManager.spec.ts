import { describe, it, expect, vi, beforeEach } from 'vitest'
import { nextTick } from 'vue'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import ScheduleManager from '../ScheduleManager.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { makeSource } from '@/composables/marketSourceView'

// 最小 stub: Modal/TimePicker（Modal 模拟真实 v-if="open" 行为; TimePicker 为受控 input）
const stubs = {
  Modal: { props: ['open'], template: '<div v-if="open"><slot /><slot name="footer" /></div>' },
  TimePicker: { props: ['modelValue'], emits: ['update:modelValue'],
    template: '<input class="tp" :value="modelValue" @input="$emit(\'update:modelValue\', $event.target.value)" />' },
}

describe('ScheduleManager', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('login == logout 时不弹非法时段（前端拦截, 不下发）', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'addSchedule').mockResolvedValue()
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP' })
    const w = mount(ScheduleManager, { props: { source }, global: { stubs } })
    await w.find('button.ds-btn--tertiary').trigger('click')  // 打开 Modal
    const tps = w.findAll('input.tp')
    await tps[0].setValue('09:00')
    await tps[1].setValue('09:00')
    await w.findAll('button.ds-btn--primary')[0].trigger('click')  // 确认
    await nextTick()
    expect(spy).not.toHaveBeenCalled()
    expect(w.text()).toContain('不能相同')
  })

  it('login < logout 正常下发（同日时段）', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'addSchedule').mockResolvedValue()
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP' })
    const w = mount(ScheduleManager, { props: { source }, global: { stubs } })
    await w.find('button.ds-btn--tertiary').trigger('click')
    const tps = w.findAll('input.tp')
    await tps[0].setValue('09:00')
    await tps[1].setValue('15:30')
    await w.findAll('button.ds-btn--primary')[0].trigger('click')
    expect(spy).toHaveBeenCalledWith(1, '09:00', '15:30')
  })

  it('跨午夜时段（login > logout）显示标识', () => {
    const source = makeSource({
      id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP',
      schedules: [
        { id: 0, source_id: 1, login_time: '08:45', logout_time: '15:30' },
        { id: 0, source_id: 1, login_time: '20:45', logout_time: '02:30' },
      ],
    })
    const w = mount(ScheduleManager, { props: { source }, global: { stubs } })
    const items = w.findAll('.schedule-item')
    expect(items[0].find('.schedule-item__overnight').exists()).toBe(false)
    expect(items[1].find('.schedule-item__overnight').exists()).toBe(true)
  })
})