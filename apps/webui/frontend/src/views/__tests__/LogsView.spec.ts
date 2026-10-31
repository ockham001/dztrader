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
// 日志文件列表键盘可达（#206 B3）：列表项原为 div + @click，键盘用户无法查看任何日志
// （本页没有替代路径），故按 listbox/option 语义 + Enter/Space 选择。
describe('LogsView 日志文件列表键盘可达（#206）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.spyOn(useLogsStore(), 'loadFiles').mockResolvedValue()
    vi.spyOn(useLogsStore(), 'refreshFiles').mockResolvedValue()
    vi.spyOn(useSystemStore(), 'init').mockResolvedValue()
  })

  function seedFiles() {
    const store = useLogsStore()
    store.files = [
      { name: 'a.log', logger: 'dzweb', size: 1024, mtime: '2026-10-26 15:00:00', path: '/logs/a.log' },
      { name: 'b.log', logger: 'dzmd_ctp', size: 2048, mtime: '2026-10-26 15:00:00', path: '/logs/b.log' },
    ]
    return store
  }

  it('文件项暴露 listbox/option 语义，选中态经 aria-selected 表达', async () => {
    const store = seedFiles()
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()

    const list = w.find('.log-browser__file-list')
    expect(list.attributes('role')).toBe('listbox')
    // 单一 tab 停靠点在容器上（APG 单选 listbox）：整组在 Tab 序列里只占一格
    expect(list.attributes('tabindex')).toBe('0')
    expect(list.attributes('aria-label')).toBe('日志文件')
    // 当前项经 aria-activedescendant 指到具体的 option id
    const activeId = list.attributes('aria-activedescendant')
    expect(activeId).toBeTruthy()

    const items = w.findAll('.log-browser__file-item')
    expect(items).toHaveLength(2)
    for (const item of items) {
      expect(item.attributes('role')).toBe('option')
      // option 由 activedescendant 指示，自身不进 Tab 序列（-1 仍可被脚本聚焦）
      expect(item.attributes('tabindex')).toBe('-1')
      expect(item.attributes('id')).toBeTruthy()
    }
    expect(items[0].attributes('id')).toBe(activeId)

    // 未选中时两项都是 false（不是缺省属性）
    expect(items[0].attributes('aria-selected')).toBe('false')

    store.selectedFile = '/logs/b.log'
    await flushPromises()
    const after = w.findAll('.log-browser__file-item')
    expect(after[0].attributes('aria-selected')).toBe('false')
    expect(after[1].attributes('aria-selected')).toBe('true')
    // activedescendant 跟随选中项
    expect(w.find('.log-browser__file-list').attributes('aria-activedescendant')).toBe(after[1].attributes('id'))
  })

  it('方向键在文件项之间移动并选中（listbox 键盘语义）', async () => {
    const store = seedFiles()
    const selectSpy = vi.spyOn(store, 'selectFile').mockImplementation(async (path, logger) => {
      store.selectedFile = path
      store.selectedLogger = logger
    })
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()

    const list = w.find('.log-browser__file-list')
    // 初始无选中 → activedescendant 落在首项；ArrowDown 选第二项
    await list.trigger('keydown', { key: 'ArrowDown' })
    await flushPromises()
    expect(selectSpy).toHaveBeenLastCalledWith('/logs/b.log', 'dzmd_ctp')

    // 已在末项，再 ArrowDown 不越界（仍停在末项）
    await list.trigger('keydown', { key: 'ArrowDown' })
    await flushPromises()
    expect(selectSpy).toHaveBeenCalledTimes(2)

    // Home 回到首项
    await list.trigger('keydown', { key: 'Home' })
    await flushPromises()
    expect(selectSpy).toHaveBeenLastCalledWith('/logs/a.log', 'dzweb')
    expect(store.selectedFile).toBe('/logs/a.log')

    // End 到末项
    await list.trigger('keydown', { key: 'End' })
    await flushPromises()
    expect(store.selectedFile).toBe('/logs/b.log')
  })

  it('Enter 与 Space 在容器上选中当前项（键盘用户的真实路径）', async () => {
    const store = seedFiles()
    const selectSpy = vi.spyOn(store, 'selectFile').mockImplementation(async (path, logger) => {
      store.selectedFile = path
      store.selectedLogger = logger
    })
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()

    const list = w.find('.log-browser__file-list')
    // 无选中时 activedescendant 在首项，Enter 选它
    await list.trigger('keydown', { key: 'Enter' })
    await flushPromises()
    expect(selectSpy).toHaveBeenCalledWith('/logs/a.log', 'dzweb')
    expect(store.selectedFile).toBe('/logs/a.log')

    // Space 选中「当前」项——先移到第二项再按 Space
    await list.trigger('keydown', { key: 'ArrowDown' })
    await flushPromises()
    expect(store.selectedFile).toBe('/logs/b.log')
    await list.trigger('keydown', { key: ' ' })
    await flushPromises()
    expect(store.selectedFile).toBe('/logs/b.log')

    // option 自身不再绑 keydown（它已退出 Tab 序列，挂了也是不可达死路径）
    const option = w.findAll('.log-browser__file-item')[0]
    await option.trigger('keydown', { key: 'Enter' })
    await flushPromises()
    // 仍停在 b.log：对 option 直接派发 Enter 不产生任何选中
    expect(store.selectedFile).toBe('/logs/b.log')
  })

  it('鼠标点击仍能选中（原有路径未被键盘改动破坏）', async () => {
    const store = seedFiles()
    vi.spyOn(store, 'selectFile').mockResolvedValue()
    const w = mount(LogsView, { global: { stubs: sharedStubs } })
    await flushPromises()

    await w.findAll('.log-browser__file-item')[1].trigger('click')
    await flushPromises()
    expect(store.selectFile).toHaveBeenCalledWith('/logs/b.log', 'dzmd_ctp')
  })
})
