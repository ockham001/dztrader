import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useToastStore } from '../toast'

describe('useToastStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('shows a toast with success level', () => {
    const store = useToastStore()
    store.success('操作成功')
    expect(store.items).toHaveLength(1)
    expect(store.items[0].level).toBe('success')
    expect(store.items[0].message).toBe('操作成功')
  })

  it('shows a toast with warning level', () => {
    const store = useToastStore()
    store.warning('警告消息')
    expect(store.items[0].level).toBe('warning')
  })

  it('shows a toast with error level (default 5s duration)', () => {
    const store = useToastStore()
    store.error('错误消息')
    expect(store.items[0].level).toBe('error')
    expect(store.items[0].duration).toBe(5000)
  })

  it('auto-dismisses after duration', () => {
    const store = useToastStore()
    store.success('临时消息', 3000)
    expect(store.items).toHaveLength(1)
    vi.advanceTimersByTime(3000)
    expect(store.items).toHaveLength(0)
  })

  it('dismisses by id', () => {
    const store = useToastStore()
    const id = store.success('test')
    store.dismiss(id)
    expect(store.items).toHaveLength(0)
  })

  it('clear removes all toasts', () => {
    const store = useToastStore()
    store.success('a')
    store.success('b')
    store.success('c')
    store.clear()
    expect(store.items).toHaveLength(0)
  })

  it('each toast has a unique id', () => {
    const store = useToastStore()
    store.success('a')
    store.success('b')
    expect(store.items[0].id).not.toBe(store.items[1].id)
  })
})
