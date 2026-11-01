import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import type { Ref } from 'vue'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import LogViewerTab from '../LogViewerTab.vue'
import { useLogsStore } from '@/stores/logs'
import { SELF_LOG_TAIL_DISABLED_TEXT } from '@/composables/logTailNotice'
import type { WsConnectionState } from '@/composables/wsClient'

// --- Mocks ---

// 可控的 WS 客户端：subscribeLog/unsubscribeLog 为 spy（经 vi.hoisted 创建，用例可直接断言）。
// connectionState 的 vue ref 也在 factory 内动态 import('vue') 构造后挂到 wsMocks.state.ref，
// 供用例读写——vi.mock factory 被提升到文件顶部，不能引用任何模块级 let/const，
// 因此状态必须经 hoisted 对象承接，避免 TDZ。
const wsMocks = vi.hoisted(() => ({
  subscribeLog: vi.fn(),
  unsubscribeLog: vi.fn(),
  state: {} as { ref?: Ref<WsConnectionState> },
}))

vi.mock('@/composables/wsClient', async () => {
  const { ref, readonly } = await import('vue')
  wsMocks.state.ref = ref<WsConnectionState>('connected')
  return {
    useWebSocket: () => ({
      connectionState: readonly(wsMocks.state.ref!),
      lastError: readonly(ref(null)),
      connect: vi.fn(),
      disconnect: vi.fn(),
      mdConnect: vi.fn(),
      mdDisconnect: vi.fn(),
      subscribeLog: wsMocks.subscribeLog,
      unsubscribeLog: wsMocks.unsubscribeLog,
    }),
    registerHandler: vi.fn(),
    unregisterHandler: vi.fn(),
  }
})

// 虚拟滚动容器：真实 RecycleScroller 依赖 DOM 尺寸/虚拟化逻辑，测试以空容器 stub 替代
vi.mock('vue-virtual-scroller', () => ({
  RecycleScroller: {
    name: 'RecycleScroller',
    props: ['items'],
    template: '<div><slot /></div>',
  },
}))

// 子组件 stub（只留必要的模板定位结构）
const stubs = {
  // 点击后模拟选择 error 级别，驱动 handleLevelChange
  Dropdown: {
    props: ['modelValue'],
    emits: ['update:modelValue'],
    template: '<button type="button" class="stub-dropdown" @click="$emit(\'update:modelValue\', \'error\')" />',
  },
}

/** 让断连→重连：connectionState 从 connected 走到 reconnecting 再回 connected，并等待 watch 触发 */
async function reconnect(): Promise<void> {
  wsMocks.state.ref!.value = 'reconnecting'
  await nextTick()
  wsMocks.state.ref!.value = 'connected'
  await nextTick()
  await flushPromises()
}

describe('LogViewerTab WS 重连恢复与防抖清理', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    wsMocks.state.ref!.value = 'connected'
    wsMocks.subscribeLog.mockClear()
    wsMocks.unsubscribeLog.mockClear()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('断线→重连且 tailEnabled 时按当前文件重建日志订阅（I1）', async () => {
    const store = useLogsStore()
    store.selectedFile = '/logs/trade.log'
    store.tailEnabled = true

    const w = mount(LogViewerTab, { global: { stubs } })
    // 挂载时 onMounted 已按 tailEnabled 订阅一次，先清空以聚焦"重连重建"的处理
    wsMocks.subscribeLog.mockClear()

    await reconnect()

    expect(wsMocks.subscribeLog).toHaveBeenCalledTimes(1)
    expect(wsMocks.subscribeLog).toHaveBeenCalledWith('/logs/trade.log')

    w.unmount()
  })

  it('tailEnabled=false 时重连不重建订阅', async () => {
    const store = useLogsStore()
    store.selectedFile = '/logs/trade.log'
    store.tailEnabled = false

    const w = mount(LogViewerTab, { global: { stubs } })

    await reconnect()

    expect(wsMocks.subscribeLog).not.toHaveBeenCalled()

    w.unmount()
  })

  it('tail 开关：开启订阅当前文件，关闭退订', async () => {
    const store = useLogsStore()
    store.selectedFile = '/logs/trade.log'
    store.tailEnabled = false

    const w = mount(LogViewerTab, { global: { stubs } })
    const tail = w.find('.log-viewer__toolbar-row--actions input[type="checkbox"]')

    await tail.setValue(true)
    expect(store.tailEnabled).toBe(true)
    expect(wsMocks.subscribeLog).toHaveBeenCalledWith('/logs/trade.log')

    await tail.setValue(false)
    expect(store.tailEnabled).toBe(false)
    expect(wsMocks.unsubscribeLog).toHaveBeenCalledTimes(1)

    w.unmount()
    // 关闭后卸载不应再退订
    expect(wsMocks.unsubscribeLog).toHaveBeenCalledTimes(1)
  })

  it('切换日志级别时关闭 tail 并退订，然后按新级别加载内容', async () => {
    const store = useLogsStore()
    store.selectedFile = '/logs/trade.log'
    store.tailEnabled = true
    const loadSpy = vi.spyOn(store, 'loadContent').mockResolvedValue()

    const w = mount(LogViewerTab, { global: { stubs } })
    wsMocks.subscribeLog.mockClear()
    wsMocks.unsubscribeLog.mockClear()

    await w.find('.stub-dropdown').trigger('click')
    await flushPromises()

    expect(store.levelFilter).toBe('error')
    expect(wsMocks.unsubscribeLog).toHaveBeenCalledTimes(1)
    expect(store.tailEnabled).toBe(false)
    expect(loadSpy).toHaveBeenCalledTimes(1)

    w.unmount()
  })

  it('卸载时 tailEnabled=true 退订日志', () => {
    const store = useLogsStore()
    store.selectedFile = '/logs/trade.log'
    store.tailEnabled = true

    const w = mount(LogViewerTab, { global: { stubs } })
    wsMocks.unsubscribeLog.mockClear()
    w.unmount()

    expect(wsMocks.unsubscribeLog).toHaveBeenCalledTimes(1)
  })

  it('关键字输入 300ms 防抖后才加载内容（fake timers）', async () => {
    const store = useLogsStore()
    store.tailEnabled = false
    const loadContentSpy = vi.spyOn(store, 'loadContent').mockResolvedValue()
    vi.useFakeTimers()

    const w = mount(LogViewerTab, { global: { stubs } })
    await w.find('input[placeholder="关键字搜索..."]').setValue('biz')

    await vi.advanceTimersByTimeAsync(299)
    expect(loadContentSpy).not.toHaveBeenCalled()

    await vi.advanceTimersByTimeAsync(1)
    expect(store.keyword).toBe('biz')
    expect(loadContentSpy).toHaveBeenCalledTimes(1)

    w.unmount()
  })

  it('卸载时清理未触发的关键字防抖定时器，避免卸载后改共享 store 状态（M14）', async () => {
    const store = useLogsStore()
    store.tailEnabled = false
    const loadContentSpy = vi.spyOn(store, 'loadContent').mockResolvedValue()
    vi.useFakeTimers()

    const w = mount(LogViewerTab, { global: { stubs } })

    // 触发 handleKeywordInput（300ms 防抖），随即卸载——若未清理，定时器到期仍会调 loadContent
    await w.find('input[placeholder="关键字搜索..."]').setValue('biz')
    w.unmount()

    await vi.advanceTimersByTimeAsync(350)
    expect(loadContentSpy).not.toHaveBeenCalled()
    expect(store.keyword).toBe('')
  })
})

describe('实时开关禁用态解释（#207 C2）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    wsMocks.state.ref!.value = 'connected'
    wsMocks.subscribeLog.mockClear()
    wsMocks.unsubscribeLog.mockClear()
  })

  it('选中 WebUI 自身日志：开关禁用并显示原因（不再是"禁用但无说明"）', async () => {
    const { useSystemStore } = await import('@/stores/system')
    const system = useSystemStore()
    system.processName = 'dzweb'
    const store = useLogsStore()
    store.selectedFile = 'logs/dzweb/dzweb.log'
    store.selectedLogger = 'dzweb'

    const w = mount(LogViewerTab, { global: { stubs } })

    const tail = w.find('[data-test="log-tail-switch"]')
    expect((tail.element as HTMLInputElement).disabled).toBe(true)
    const hint = w.find('[data-test="log-tail-hint"]')
    expect(hint.exists()).toBe(true)
    // 文案与后端退订 toast 共用同一常量（composables/logTailNotice）
    expect(hint.text()).toBe(SELF_LOG_TAIL_DISABLED_TEXT)
    // 可见文案与 hover 说明同源，用户不必猜
    expect(tail.element.closest('label')?.getAttribute('title')).toContain('反馈循环')

    w.unmount()
  })

  it('选中其他进程日志：开关可用、不显示禁用原因', async () => {
    const { useSystemStore } = await import('@/stores/system')
    useSystemStore().processName = 'dzweb'
    const store = useLogsStore()
    store.selectedFile = 'logs/dztraderd/dztraderd.log'
    store.selectedLogger = 'dztraderd'

    const w = mount(LogViewerTab, { global: { stubs } })

    const tail = w.find('[data-test="log-tail-switch"]')
    expect((tail.element as HTMLInputElement).disabled).toBe(false)
    expect(w.find('[data-test="log-tail-hint"]').exists()).toBe(false)

    w.unmount()
  })

  // #233 验收第 2 条：图形开关的 label 内只有 span（track/thumb），无任何可读文字；
  // 视觉名称「实时」写在 label 外的 span 里，读屏拿不到。nesting 能让两条 lint 规则通过，
  // 但 lint 不查「label 内有没有文字」——故这里断言运行期真的有可读名称。
  it('实时开关有可访问名称（读屏可报出，而非只有图形和旁边的未关联文字）', () => {
    const store = useLogsStore()
    store.selectedFile = 'logs/dztraderd/dztraderd.log'
    store.selectedLogger = 'dztraderd'

    const w = mount(LogViewerTab, { global: { stubs } })
    const tail = w.find('[data-test="log-tail-switch"]')
    expect(tail.attributes('aria-label')).toBe('实时')
    w.unmount()
  })

  it('未选中文件：开关禁用但没有"自身日志"原因（原因是未选文件，不误导）', async () => {
    const { useSystemStore } = await import('@/stores/system')
    useSystemStore().processName = 'dzweb'
    const store = useLogsStore()
    store.selectedFile = ''
    store.selectedLogger = ''

    const w = mount(LogViewerTab, { global: { stubs } })

    expect((w.find('[data-test="log-tail-switch"]').element as HTMLInputElement).disabled).toBe(true)
    expect(w.find('[data-test="log-tail-hint"]').exists()).toBe(false)

    w.unmount()
  })
})
