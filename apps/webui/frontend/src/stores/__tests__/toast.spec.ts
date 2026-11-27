import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useToastStore, TOAST_MAX_VISIBLE, TOAST_DURATION } from '../toast'

describe('useToastStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    // performance.now 在 fake timers 下不自动前进，单独代理到 Date.now 以便断言剩余时长
    vi.spyOn(performance, 'now').mockImplementation(() => Date.now())
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.restoreAllMocks()
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

  // ADR 0060：分级停留时长 info 4s / warning 6s / error 10s
  it('error 默认 10s（分级时长，不再是旧的 5s）', () => {
    const store = useToastStore()
    store.error('错误消息')
    expect(store.items[0].level).toBe('error')
    expect(store.items[0].duration).toBe(TOAST_DURATION.error)
    expect(TOAST_DURATION.error).toBe(10000)
  })

  it('info 4s / warning 6s / success 3s', () => {
    const store = useToastStore()
    store.info('信息')
    expect(store.items[0].duration).toBe(4000)
    store.warning('警告')
    expect(store.items[1].duration).toBe(6000)
    store.success('成功')
    expect(store.items[2].duration).toBe(3000)
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

  // === ADR 0060 决策 6：同时最多 3 条，超出排队（不丢弃） ===
  describe('可见条数上限与排队', () => {
    it(`同时最多 ${TOAST_MAX_VISIBLE} 条，超出进队列`, () => {
      const store = useToastStore()
      store.success('a')
      store.success('b')
      store.success('c')
      store.success('d')
      expect(store.items).toHaveLength(3)
      expect(store.queue).toHaveLength(1)
      expect(store.queue[0].message).toBe('d')
    })

    it('关闭一条后队列补位', () => {
      const store = useToastStore()
      store.success('a')
      store.success('b')
      store.success('c')
      store.success('d')
      const firstId = store.items[0].id
      store.dismiss(firstId)
      expect(store.items).toHaveLength(3)
      expect(store.items[2].message).toBe('d')
      expect(store.queue).toHaveLength(0)
    })

    it('排队中的条目超时也不显示（只在补位后开始计时）', () => {
      const store = useToastStore()
      store.success('a', 1000)
      store.success('b', 1000)
      store.success('c', 1000)
      store.success('d', 1000)
      expect(store.queue).toHaveLength(1)
      // 队列条目的 timer 为 null（尚未开始计时）
      expect(store.queue[0].timer).toBeNull()
    })
  })

  // === ADR 0060 决策 3：常驻（popup=true） ===
  describe('常驻提示', () => {
    it('persistent 的 duration=0，不排定时器、永不自动消失', () => {
      const store = useToastStore()
      store.persistent('交易登录失败', 'error')
      expect(store.items).toHaveLength(1)
      expect(store.items[0].duration).toBe(0)
      expect(store.items[0].timer).toBeNull()
      vi.advanceTimersByTime(60000)
      expect(store.items).toHaveLength(1) // 仍在
    })

    it('常驻条目仍可手动关闭', () => {
      const store = useToastStore()
      const id = store.persistent('交易登录失败', 'error')
      store.dismiss(id)
      expect(store.items).toHaveLength(0)
    })
  })

  // === hover 暂停倒计时 ===
  describe('hover 暂停', () => {
    it('pause 记录剩余时长并清定时器，resume 按剩余重排', () => {
      const store = useToastStore()
      store.success('a', 4000)
      const item = store.items[0]

      vi.advanceTimersByTime(1000) // 已过 1s
      store.pause(item.id)
      expect(item.paused).toBe(true)
      expect(item.timer).toBeNull()
      expect(item.remaining).toBeGreaterThan(2900)
      expect(item.remaining).toBeLessThanOrEqual(3000)

      // 暂停期间时间流逝，不应消失
      vi.advanceTimersByTime(10000)
      expect(store.items).toHaveLength(1)

      store.resume(item.id)
      expect(item.paused).toBe(false)
      expect(item.timer).not.toBeNull()
      // 恢复后按剩余时长（约 3s）消失
      vi.advanceTimersByTime(3000)
      expect(store.items).toHaveLength(0)
    })

    it('常驻条目 pause/resume 是 no-op（没有倒计时可暂停）', () => {
      const store = useToastStore()
      store.persistent('常驻', 'error')
      const item = store.items[0]
      store.pause(item.id)
      expect(item.paused).toBe(false)
      store.resume(item.id)
      expect(item.timer).toBeNull()
    })
  })
})
