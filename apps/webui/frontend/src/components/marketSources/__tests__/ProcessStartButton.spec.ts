import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import ProcessStartButton from '../ProcessStartButton.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { makeSource } from '@/composables/marketSourceView'

// 停止源重启入口（N1 回归修复）：进程非 Running 时显示"启动进程"按钮。
// start 无 running 前置（REST /start 仅需 admin + 源存在），Stopped/Crashed/未知态均可重启
describe('ProcessStartButton', () => {
  beforeEach(() => { setActivePinia(createPinia()) })

  it('Running 态不渲染', () => {
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP', process_state: 'Running' })
    const w = mount(ProcessStartButton, { props: { source } })
    expect(w.find('button').exists()).toBe(false)
  })

  it('Stopped 态渲染并点击调用 store.start', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'start').mockResolvedValue()
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP', process_state: 'Stopped' })
    const w = mount(ProcessStartButton, { props: { source } })
    await w.find('button').trigger('click')
    expect(spy).toHaveBeenCalledWith(1)
  })

  it('Crashed 态也渲染（崩溃重启入口）', () => {
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP', process_state: 'Crashed' })
    const w = mount(ProcessStartButton, { props: { source } })
    expect(w.find('button').exists()).toBe(true)
  })

  it('startPending 时禁用并显示启动中', () => {
    const source = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP', process_state: 'Stopped', startPending: true })
    const w = mount(ProcessStartButton, { props: { source } })
    const btn = w.find('button')
    expect((btn.element as HTMLButtonElement).disabled).toBe(true)
    expect(w.text()).toContain('启动中')
  })
})