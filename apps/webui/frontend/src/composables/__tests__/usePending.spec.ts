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

  it('超时清 pending 并提示（opLabel 优先，无 opLabel 回落 key）', async () => {
    const toast = useToastStore()
    const errorSpy = vi.spyOn(toast, 'error')
    const { pending, run } = inst

    const p = run('a:slow', () => new Promise(() => {}), { timeout: 1000, opLabel: '行情源登录' })
    await vi.advanceTimersByTimeAsync(1001)
    expect(pending['a:slow']).toBe(false)
    expect(errorSpy).toHaveBeenCalledTimes(1)
    expect(errorSpy).toHaveBeenLastCalledWith('行情源登录超时')
    await p

    const p2 = run('b:slow', () => new Promise(() => {}), { timeout: 1000 })
    await vi.advanceTimersByTimeAsync(1001)
    expect(errorSpy).toHaveBeenCalledTimes(2)
    expect(errorSpy).toHaveBeenLastCalledWith('b:slow 操作超时')
    await p2
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

  // ===== touch：续命语义（#508）=====
  // 兜底超时要回答「多久没动静」而不是「总共等了多久」——登录链这种慢但活着的过程靠它。

  it('touch 把 deadline 推到此刻 + 原 timeout：不超时且 pending 保持', async () => {
    const { pending, run, touch } = inst
    const p = run('t:alive', () => new Promise(() => {}), { timeout: 1000, opLabel: '登录' })

    await vi.advanceTimersByTimeAsync(800)
    expect(pending['t:alive']).toBe(true)
    touch('t:alive')  // 有进展
    await vi.advanceTimersByTimeAsync(800)  // 距首次武装 1600ms，但距 touch 只有 800ms
    expect(pending['t:alive']).toBe(true)

    await vi.advanceTimersByTimeAsync(300)  // 距 touch 1100ms > 1000ms
    expect(pending['t:alive']).toBe(false)
    await p
  })

  it('touch 可反复续命；停止推进后仍按原 timeout 到期（不累积漂移）', async () => {
    const { pending, run, touch } = inst
    const p = run('t:repeat', () => new Promise(() => {}), { timeout: 1000 })
    for (let i = 0; i < 5; i++) {
      await vi.advanceTimersByTimeAsync(900)
      touch('t:repeat')
    }
    expect(pending['t:repeat']).toBe(true)  // 5 次续命后仍活着
    await vi.advanceTimersByTimeAsync(1001)
    expect(pending['t:repeat']).toBe(false)
    await p
  })

  it('touch 不改 startTime 与 minPendingMs：resolve 的最短展示时长仍按首次 run 计算', async () => {
    const { pending, run, touch, resolve } = inst
    // fn 用可控 promise：手动 resolve 才能让 run 的 promise 正常 settle（否则只能等超时）
    let release: (() => void) | undefined
    const fnPromise = new Promise<void>((r) => { release = r })
    const p = run('t:min', () => fnPromise, { timeout: 5000, minPendingMs: 500 })

    await vi.advanceTimersByTimeAsync(200)
    touch('t:min')
    resolve('t:min')  // elapsed = 200 < 500 → 延迟到满 500ms 再清
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['t:min']).toBe(true)
    await vi.advanceTimersByTimeAsync(400)  // 距 resolve 400ms，总 600ms > 500ms
    expect(pending['t:min']).toBe(false)
    release?.()
    await p
  })

  it('touch 对不存在的 key 无副作用（不创建垃圾条目、不排定时器）', async () => {
    const { pending, touch } = inst
    touch('never:run')
    await vi.advanceTimersByTimeAsync(10_000)
    expect('never:run' in pending).toBe(false)
    expect(Object.keys(pending).length).toBe(0)
  })

  it('结束路径（resolve 后）touch 不复活 pending', async () => {
    const { pending, run, touch, resolve } = inst
    const p = run('t:done', () => Promise.resolve(), { timeout: 1000 })
    await p
    resolve('t:done')
    await vi.advanceTimersByTimeAsync(0)
    expect(pending['t:done']).toBe(false)
    touch('t:done')
    await vi.advanceTimersByTimeAsync(2000)
    expect(pending['t:done']).toBe(false)
  })
})
