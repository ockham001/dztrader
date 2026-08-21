import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useLogsStore } from '@/stores/logs'
import { useSystemStore } from '@/stores/system'
import LogsView from '../LogsView.vue'

// mock wsClient（LogsView 引用 useWebSocket，其内部触碰真实 WebSocket，须隔离）
vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({
    connectionState: { value: 'disconnected' },
    lastError: { value: null },
    connect: vi.fn(),
    disconnect: vi.fn(),
    mdConnect: vi.fn(),
    mdDisconnect: vi.fn(),
    subscribeLog: vi.fn(),
    unsubscribeLog: vi.fn(),
  }),
}))

// stub 重子组件与共享组件（各自已有独立测试）
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Dropdown: { props: ['modelValue'], template: '<span class="dropdown-stub">{{ String(modelValue) }}</span>' },
  DatePicker: { props: ['modelValue'], template: '<input class="datepicker-stub" />' },
  LogViewerTab: { template: '<div class="tab-viewer-stub">查看面板</div>' },
  LogAnalysisTab: { template: '<div class="tab-analysis-stub">分析面板</div>' },
  LogLevelControlTab: { template: '<div class="tab-control-stub">级别面板</div>' },
}

describe('LogsView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.spyOn(useLogsStore(), 'loadFiles').mockResolvedValue()
    vi.spyOn(useLogsStore(), 'refreshFiles').mockResolvedValue()
    vi.spyOn(useSystemStore(), 'init').mockResolvedValue()
  })

  it('渲染日志页的查看/分析/级别面板', async () => {
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()
    expect(w.text()).toContain('查看面板')
    expect(w.text()).toContain('分析面板')
    expect(w.text()).toContain('级别面板')
  })

  it('点击页签切换 browserTab', async () => {
    const store = useLogsStore()
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const analysisTab = w.findAll('button').find(b => b.text().trim() === '分析')
    await analysisTab!.trigger('click')
    expect(store.browserTab).toBe('analysis')
  })
})