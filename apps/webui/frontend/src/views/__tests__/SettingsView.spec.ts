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

  it('主进程配置的元数据文件大小走共享格式化模块（IEC 单位档）', async () => {
    const store = useSettingsStore()
    store.master = {
      single_stop_timeout_sec: 5,
      cleanup_max_page_count: 100,
      cleanup_max_page_age_hours: 12,
      meta_file_size: 2 * 1024 * 1024,
    }
    const w = mount(SettingsView, { global: { stubs: sharedStubs } })
    const masterTab = w.findAll('button').find(b => b.text().trim() === '主进程')
    await masterTab!.trigger('click')
    // #126：与监控页 / 配置页 / 日志页同一实现——IEC 档位（旧实现是十进制 `MB` 标签）
    expect(w.text()).toContain('2.0 MiB')
    expect(w.text()).not.toContain(' MB')
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

  // #207 C1：无改动时保存按钮禁用且不发请求（旧实现只按 webuiSaving 禁用，能白点一次 PUT）
  describe('WebUI 保存的 dirty 判据（#207 C1）', () => {
    function seedWebui(overrides: Partial<{ token_ttl_sec: number; notify_cache_size: number }> = {}) {
      const store = useSettingsStore()
      store.webui = {
        server_listen: '0.0.0.0', server_port: 8080, token_ttl_sec: 3600,
        jwt_secret_set: true, notify_cache_size: 100, ...overrides,
      }
      return store
    }

    async function mountWebuiTab() {
      const w = mount(SettingsView, { global: { stubs: sharedStubs } })
      const webuiTab = w.findAll('button').find(b => b.text() === 'WebUI')
      await webuiTab!.trigger('click')
      await w.vm.$nextTick()
      return w
    }

    it('初值与镜像一致：保存按钮禁用 + 显示「无改动」，点击不发请求', async () => {
      const store = seedWebui()
      const saveSpy = vi.spyOn(store, 'setWebui').mockResolvedValue(true)
      const w = await mountWebuiTab()

      const saveBtn = w.find('[data-test="webui-save"]')
      expect(saveBtn.attributes('disabled')).toBeDefined()
      expect(w.find('[data-test="webui-save-clean"]').exists()).toBe(true)

      await saveBtn.trigger('click')
      await flushPromises()
      expect(saveSpy).not.toHaveBeenCalled()
    })

    it('改动后可保存；改回原值又回到禁用（按生效镜像比较，不是按「是否碰过输入框」）', async () => {
      const store = seedWebui()
      const saveSpy = vi.spyOn(store, 'setWebui').mockResolvedValue(true)
      const w = await mountWebuiTab()

      const input = w.findAll('input[type="number"]')[0]
      await input.setValue('7200')
      expect(w.find('[data-test="webui-save"]').attributes('disabled')).toBeUndefined()
      expect(w.find('[data-test="webui-save-clean"]').exists()).toBe(false)

      // 按**数值**比较输入与镜像：表单是字符串/数字混合态，文本形态差异（前导零、number 回写）
      // 不算改动，只有值真的不同才是 dirty
      await input.setValue('03600')
      expect(w.find('[data-test="webui-save"]').attributes('disabled')).toBeDefined()
      expect(w.find('[data-test="webui-save-clean"]').exists()).toBe(true)
      expect(saveSpy).not.toHaveBeenCalled()
    })

    it('镜像尚未到达（store.webui 为 null）时保存禁用，不凭空发请求', async () => {
      const store = useSettingsStore()
      store.webui = null
      const saveSpy = vi.spyOn(store, 'setWebui').mockResolvedValue(true)
      const w = await mountWebuiTab()

      // 加载中态没有保存按钮
      expect(w.find('[data-test="webui-save"]').exists()).toBe(false)
      expect(saveSpy).not.toHaveBeenCalled()
    })
  })
})