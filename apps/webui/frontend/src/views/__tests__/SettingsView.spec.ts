import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useSettingsStore } from '@/stores/settings'
import SettingsView from '../SettingsView.vue'

// stub 子组件：避免 Modal/TimePicker/Icon 渲染细节
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], emits: ['close'], template: '<div class="modal-stub"><slot /></div><div class="modal-footer-stub"><slot name="footer" /></div>' },
  TimePicker: { props: ['modelValue'], emits: ['update:modelValue'], template: '<input class="timepicker-stub" />' },
}

describe('SettingsView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    // 预置事件通道配置镜像，避免 loadMaster/loadWebui 真实请求
    const store = useSettingsStore()
    vi.spyOn(store, 'loadMaster').mockResolvedValue()
    vi.spyOn(store, 'loadWebui').mockResolvedValue()
  })

  it('渲染三个页签', () => {
    const w = mount(SettingsView, { global: { stubs: sharedStubs } })
    expect(w.text()).toContain('事件通道')
    expect(w.text()).toContain('WebUI')
    expect(w.text()).toContain('主进程')
  })

  it('check 字段失焦且值改变时提交 setEventShmConfig', async () => {
    const store = useSettingsStore()
    // 预置镜像，使 check_interval_min 旧值已知
    store.eventShmConfig = {
      page_size_mb: 64, check_interval_min: 5, check_pages: 10, check_bytes: 1024, preload_points: {},
    }
    const setSpy = vi.spyOn(store, 'setEventShmConfig').mockResolvedValue(true)
    const w = mount(SettingsView, { global: { stubs: sharedStubs } })
    // 触发 check_interval_min 输入失焦（值 5→10 改变）
    const input = w.find('input') // 事件通道 tab 第一个数字输入
    await input.setValue('10')
    await input.trigger('change')
    await input.trigger('blur')
    expect(setSpy).toHaveBeenCalledWith({ check_interval_min: 10 })
  })

  it('check 字段失焦值未改变时不提交', async () => {
    const store = useSettingsStore()
    store.eventShmConfig = {
      page_size_mb: 64, check_interval_min: 5, check_pages: 10, check_bytes: 1024, preload_points: {},
    }
    const setSpy = vi.spyOn(store, 'setEventShmConfig').mockResolvedValue(true)
    const w = mount(SettingsView, { global: { stubs: sharedStubs } })
    // check_interval_min 仍是 5（旧值），失焦不提交
    const input = w.find('input')
    await input.setValue('5')
    await input.trigger('change')
    await input.trigger('blur')
    expect(setSpy).not.toHaveBeenCalled()
  })

  it('切换到 WebUI 页签后字段由 store 渲染，保存提交表单值', async () => {
    const store = useSettingsStore()
    // WebUI tab 内容依赖 store.webui 存在（缺省显示"加载中..."），先赋镜像值
    store.webui = {
      server_listen: '0.0.0.0', server_port: 8080, token_ttl_sec: 3600,
      jwt_secret_set: true, notify_cache_size: 100,
    }
    const saveSpy = vi.spyOn(store, 'setWebui').mockResolvedValue(true)
    const w = mount(SettingsView, { global: { stubs: sharedStubs } })
    const webuiTab = w.findAll('button').find(b => b.text() === 'WebUI')
    await webuiTab!.trigger('click')
    await w.vm.$nextTick()

    const inputs = w.findAll('input[type="number"]')
    expect((inputs[0].element as HTMLInputElement).value).toBe('3600')
    expect((inputs[1].element as HTMLInputElement).value).toBe('100')

    await inputs[0].setValue('7200')
    await inputs[1].setValue('200')
    const saveBtn = w.findAll('button').find(b => b.text().includes('保存配置'))
    await saveBtn!.trigger('click')
    await flushPromises()

    expect(saveSpy).toHaveBeenCalledWith({ token_ttl_sec: 7200, notify_cache_size: 200 })
  })
})