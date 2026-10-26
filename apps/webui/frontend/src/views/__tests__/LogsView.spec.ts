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

  it('日志文件大小走共享格式化模块（IEC 单位档，不再用 KB/MB）', async () => {
    const store = useLogsStore()
    store.files = [
      { name: 'a.log', logger: 'dzweb', size: 2 * 1024 * 1024, mtime: '2026-10-26 15:00:00', path: '/logs/a.log' },
      { name: 'b.log', logger: 'dzweb', size: 1536, mtime: '2026-10-26 15:00:00', path: '/logs/b.log' },
      { name: 'c.log', logger: 'dzweb', size: 512, mtime: '2026-10-26 15:00:00', path: '/logs/c.log' },
    ]
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()
    const meta = w.findAll('.log-browser__file-meta')
    expect(meta).toHaveLength(3)
    // #126：与监控页 / 配置页同一实现——IEC 档位，`B` 档取整、≥KiB 固定 1 位小数
    expect(meta[0].text()).toContain('2.0 MiB')
    expect(meta[1].text()).toContain('1.5 KiB')
    expect(meta[2].text()).toContain('512 B')
    // 旧实现的十进制标签 KB/MB 不再出现
    expect(w.text()).not.toContain('KB')
    expect(w.text()).not.toContain('MB')
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