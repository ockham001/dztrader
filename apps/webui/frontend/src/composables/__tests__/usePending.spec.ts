import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { usePending } from '../usePending'
import { useToastStore } from '@/stores/toast'

// 注意: usePending 现在是工厂（每调用独立 pending/timers 实例）——每个用例
// 从实例取 __resetForTests 重置；超时路径会调用 useToastStore，需要 active pinia

describe('usePending', () => {
  let inst: ReturnType<typeof usePending>
  beforeEach(() => {
    setActivePinia(createPinia())
    inst = usePending()
    inst.__resetForTests()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('run 设置 pending，resolve 清除', async () => {
    const { pending, run, resolve } = inst
    const p = run('a:op', () => Promise.resolve())
    expect(pending['a:op']).toBe(true)
    resolve('a:op')
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['a:op']).toBe(false)
    await p
  })

  it('keepPendingOnSuccess=false 时成功自动清除', async () => {
    const { pending, run } = inst
    await run('a:op2', () => Promise.resolve(), { keepPendingOnSuccess: false })
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['a:op2']).toBe(false)
  })

  it('超时清 pending 并提示', async () => {
    const { pending, run } = inst
    const p = run('a:slow', () => new Promise(() => {}), { timeout: 1000 })
    await vi.advanceTimersByTimeAsync(1001)
    expect(pending['a:slow']).toBe(false)
    await p
  })

  it('fail 清 pending', async () => {
    const { pending, run, fail } = inst
    const p = run('a:fail', () => Promise.resolve())
    expect(pending['a:fail']).toBe(true)
    fail('a:fail', 'boom')
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['a:fail']).toBe(false)
    await p
  })

  it('minPendingMs 防闪烁：resolve 后 pending 至少保留 minPendingMs', async () => {
    const { pending, run, resolve } = inst
    const p = run('a:flash', () => Promise.resolve(), { minPendingMs: 300 })
    resolve('a:flash')
    await vi.advanceTimersByTimeAsync(100)
    expect(pending['a:flash']).toBe(true)   // 未到最短时间，仍显示
    await vi.advanceTimersByTimeAsync(200)
    expect(pending['a:flash']).toBe(false)
    await p
  })

  it('timer 触发后不残留（重复 key 可再次 run）', async () => {
    const { pending, run, resolve } = inst
    const p1 = run('a:again', () => Promise.resolve(), { timeout: 1000 })
    resolve('a:again')
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['a:again']).toBe(false)
    const p2 = run('a:again', () => Promise.resolve(), { timeout: 1000 })  // 再次 run 不冲突
    expect(pending['a:again']).toBe(true)
    resolve('a:again')
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['a:again']).toBe(false)
    await Promise.all([p1, p2])
  })

  it('clearByPrefix 批量清前缀匹配的多个 key 的 pending', async () => {
    const { pending, run, clearByPrefix } = inst
    await run('source:12:op1', () => Promise.resolve())
    await run('source:12:op2', () => Promise.resolve())
    await run('source:13:op1', () => Promise.resolve())
    expect(pending['source:12:op1']).toBe(true)
    clearByPrefix('source:12')
    expect(pending['source:12:op1']).toBeUndefined()
    expect(pending['source:12:op2']).toBeUndefined()
    expect(pending['source:13:op1']).toBe(true)   // 前缀不匹配的不受影响
  })

  it('clearByPrefix 同时清除 timer：超时不再触发', async () => {
    const toast = useToastStore()
    const errorSpy = vi.spyOn(toast, 'error')
    const { pending, run, clearByPrefix } = inst
    run('source:12:slow', () => new Promise(() => {}), { timeout: 1000 })
    clearByPrefix('source:12')
    await vi.advanceTimersByTimeAsync(2000)
    expect(errorSpy).not.toHaveBeenCalled()
    expect(pending['source:12:slow']).toBeUndefined()
  })

  it('未 run 过的 key resolve/fail 不产生垃圾条目', () => {
    const { pending, resolve, fail } = inst
    resolve('never:ran')
    fail('never:ran2', 'boom')
    expect('never:ran' in pending).toBe(false)
    expect('never:ran2' in pending).toBe(false)
    expect(Object.keys(pending).length).toBe(0)
  })

  it('fn 同步 throw 走 fail 语义：pending 清除且 promise 不 reject', async () => {
    const { pending, run } = inst
    const p = run('sync:throw', () => {
      throw new Error('sync boom')
    })
    expect(pending['sync:throw']).toBe(true)
    const result = await p
    await vi.advanceTimersByTimeAsync(0)
    expect(result).toBeUndefined()
    expect(pending['sync:throw']).toBe(false)
  })

  it('实例相互隔离：一个实例的 pending 不影响另一实例', async () => {
    const a = usePending()
    const b = usePending()
    const pa = a.run('iso:op', () => Promise.resolve())
    expect(a.pending['iso:op']).toBe(true)
    expect(b.pending['iso:op']).toBeUndefined()  // b 独立空间
    await pa
  })
})
