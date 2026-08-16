import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useNotifyStore } from '../notify'
import { useMarketSourcesStore } from '../marketSources'

// mock toast store：断言 push 的 toast 分发调用（error/warning/info）
const toastMocks = vi.hoisted(() => ({
  error: vi.fn(),
  warning: vi.fn(),
  info: vi.fn(),
  show: vi.fn(),
  success: vi.fn(),
  dismiss: vi.fn(),
  clear: vi.fn(),
  items: [] as unknown[],
}))
vi.mock('../toast', () => ({
  useToastStore: () => toastMocks,
}))

describe('useNotifyStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    toastMocks.error.mockClear()
    toastMocks.warning.mockClear()
    toastMocks.info.mockClear()
  })

  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('push 缓存条目（含 source/level/timestamp）', () => {
    const store = useNotifyStore()
    store.push('订阅失败', 'dzmd_ctp', 'error')
    expect(store.items).toHaveLength(1)
    expect(store.items[0]).toMatchObject({ source: 'dzmd_ctp', message: '订阅失败', level: 'error' })
    expect(typeof store.items[0].timestamp).toBe('number')
  })

  it('push 按 level 分发到对应 toast 方法', () => {
    const store = useNotifyStore()
    store.push('错误', undefined, 'error')
    expect(toastMocks.error).toHaveBeenCalledWith('错误')
    expect(toastMocks.warning).not.toHaveBeenCalled()
    expect(toastMocks.info).not.toHaveBeenCalled()

    store.push('警告', undefined, 'warning')
    expect(toastMocks.warning).toHaveBeenCalledWith('警告')

    store.push('信息', undefined, 'info')
    expect(toastMocks.info).toHaveBeenCalledWith('信息')
  })

  it('level 缺失时默认 error', () => {
    const store = useNotifyStore()
    store.push('未分级消息')
    expect(toastMocks.error).toHaveBeenCalledWith('未分级消息')
    expect(store.items[0].level).toBe('error')
  })

  it('空消息不弹 toast 不缓存', () => {
    const store = useNotifyStore()
    store.push('', 'dzmd_ctp', 'error')
    expect(store.items).toHaveLength(0)
    expect(toastMocks.error).not.toHaveBeenCalled()
  })

  it('缓存只保留最近 100 条', () => {
    const store = useNotifyStore()
    for (let i = 0; i < 105; i++) {
      store.push(`消息 ${i}`, undefined, 'info')
    }
    expect(store.items).toHaveLength(100)
    // 最旧 5 条被淘汰，最近 100 条保留（含第 104 条）
    expect(store.items[0].message).toBe('消息 5')
    expect(store.items[99].message).toBe('消息 104')
  })

  it('clear 清空缓存', () => {
    const store = useNotifyStore()
    store.push('a')
    store.push('b')
    store.clear()
    expect(store.items).toHaveLength(0)
  })

  // 契约 notify-ui 前端义务：level=error 且 popup=true 必须打断用户展示（入 popup 队列）
  it('popup=true 且 error 级别进入弹窗队列，ackPopup 逐条出队', () => {
    const store = useNotifyStore()
    expect(store.popupCurrent).toBeNull()
    store.push('订阅失败', 'dzmd_ctp', 'error', true)
    expect(store.popupCurrent?.message).toBe('订阅失败')
    store.push('登录失败', 'dzmd_ctp', 'error', true)
    expect(store.popupCurrent?.message).toBe('订阅失败')
    store.ackPopup()
    expect(store.popupCurrent?.message).toBe('登录失败')
    store.ackPopup()
    expect(store.popupCurrent).toBeNull()
  })

  it('popup=true 但非 error 级别不入队（仅 toast）', () => {
    const store = useNotifyStore()
    store.push('警告', undefined, 'warning', true)
    store.push('信息', undefined, 'info', true)
    expect(store.popupCurrent).toBeNull()
    expect(toastMocks.warning).toHaveBeenCalledWith('警告')
    expect(toastMocks.info).toHaveBeenCalledWith('信息')
  })

  it('popup 缺省不入队', () => {
    const store = useNotifyStore()
    store.push('普通错误', undefined, 'error')
    expect(store.popupCurrent).toBeNull()
  })

  // 双 toast 修复核心断言：push 绝不写任何其他 store 的 error.value
  it('push 不触碰 marketSources.error.value（双 toast 修复）', () => {
    const ms = useMarketSourcesStore()
    const notify = useNotifyStore()
    expect(ms.error).toBeNull()

    notify.push('错误消息', 'dzmd_ctp', 'error')
    notify.push('警告消息', 'dzmd_ctp', 'warning')
    notify.push('信息消息', 'dzmd_ctp', 'info')

    // error.value 保持 null：View watch 不会因此二次弹 toast
    expect(ms.error).toBeNull()
    // toast 只弹一次（mock 各被调用一次）
    expect(toastMocks.error).toHaveBeenCalledTimes(1)
    expect(toastMocks.warning).toHaveBeenCalledTimes(1)
    expect(toastMocks.info).toHaveBeenCalledTimes(1)
  })
})
