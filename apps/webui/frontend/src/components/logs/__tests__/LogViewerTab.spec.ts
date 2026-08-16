import { describe, it, expect, vi, beforeEach } from 'vitest'
import { nextTick } from 'vue'
import type { Ref } from 'vue'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import LogViewerTab from '../LogViewerTab.vue'
import { useLogsStore } from '@/stores/logs'
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
  Dropdown: { template: '<div class="stub-dropdown" />' },
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

  it('卸载时清理未触发的关键字防抖定时器，避免卸载后改共享 store 状态（M14）', async () => {
    const store = useLogsStore()
    store.tailEnabled = false
    const loadContentSpy = vi.spyOn(store, 'loadContent')

    const w = mount(LogViewerTab, { global: { stubs } })

    // 触发 handleKeywordInput（300ms 防抖），随即卸载——若未清理，定时器到期仍会调 loadContent
    await w.find('input[placeholder="关键字搜索..."]').setValue('biz')
    w.unmount()

    await new Promise(resolve => setTimeout(resolve, 350))

    expect(loadContentSpy).not.toHaveBeenCalled()
  })
})