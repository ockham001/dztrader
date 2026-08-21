import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { usePending, PENDING_TIMEOUT } from '../usePending'
import { createOperationRunner, type OperationResult } from '../useOperation'

// P3 任务 2: useOperation 统一「防重入 + name→id 映射 + run + 结果映射」机械样板。
// 覆盖四件套: opKey / busy(防重入) / assign(name→id) / execute(run+boolean|PENDING_TIMEOUT 映射)。
// execute 超时路径经 useToastStore().error，需 active pinia；全用例 fake timers。

describe('createOperationRunner', () => {
  let inst: ReturnType<typeof usePending>
  let nameToId: { value: Record<string, number> }
  let runner: ReturnType<typeof createOperationRunner>

  beforeEach(() => {
    setActivePinia(createPinia())
    inst = usePending()
    inst.__resetForTests()
    nameToId = { value: {} }
    runner = createOperationRunner({
      pending: inst,
      timeout: 1000,
      opKey: (id, op) => `source:${id}:${op}`,
      nameToId,
    })
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('opKey 生成 source:{id}:{op}', () => {
    expect(runner.opKey(12, 'broker_add')).toBe('source:12:broker_add')
  })

  it('busy 反映 pending 防重入状态', async () => {
    const key = runner.opKey(12, 'login')
    expect(runner.busy(key)).toBe(false)
    const p = inst.run(key, () => Promise.resolve())
    expect(runner.busy(key)).toBe(true)
    inst.resolve(key)
    await vi.advanceTimersByTimeAsync(0)
    expect(runner.busy(key)).toBe(false)
    await p
  })

  it('assign 记录 sourceName→id 映射', () => {
    runner.assign(12, 'dzmd_ctp')
    expect(nameToId.value['dzmd_ctp']).toBe(12)
  })

  it('execute 成功返回 true 且保持 pending（等 RTN 清）', async () => {
    const key = runner.opKey(12, 'login')
    const r = await runner.execute(key, () => Promise.resolve({ ok: 1 }), '登录')
    expect(r).toBe(true)
    expect(inst.pending[key]).toBe(true)  // keepPendingOnSuccess 默认，pending 保持
    inst.resolve(key)
    await vi.advanceTimersByTimeAsync(0)
  })

  it('execute fn 拒绝返回 false 并清 pending', async () => {
    const key = runner.opKey(12, 'login')
    const r = await runner.execute(key, () => Promise.reject(new Error('boom')), '登录')
    await vi.advanceTimersByTimeAsync(0)
    expect(r).toBe(false)
    expect(inst.pending[key]).toBe(false)
  })

  it('execute 超时返回 PENDING_TIMEOUT 并清 pending', async () => {
    const key = runner.opKey(12, 'login')
    const r = runner.execute(key, () => new Promise(() => {}), '登录')
    await vi.advanceTimersByTimeAsync(1001)
    expect(await r).toBe(PENDING_TIMEOUT)
    expect(inst.pending[key]).toBe(false)  // 超时 usePending 置 false（非 delete）
  })

  it('process-style 映射：result!==false 使 PENDING_TIMEOUT→true、false→false', async () => {
    // 超时 → PENDING_TIMEOUT → !==false → true（跳过 error）
    const k1 = runner.opKey(1, 'start')
    const p1 = runner.execute(k1, () => new Promise(() => {}), '启动')
    await vi.advanceTimersByTimeAsync(1001)
    expect((await p1) !== false).toBe(true)
    // 失败 → false → !==false → false
    const k2 = runner.opKey(2, 'start')
    const p2 = runner.execute(k2, () => Promise.resolve(undefined as never), '启动')
    await vi.advanceTimersByTimeAsync(0)
    expect((await p2) !== false).toBe(false)
  })

  it('execute 返回类型的判别辅助（类型护栏）', () => {
    const ok: OperationResult = true
    const fail: OperationResult = false
    const timeout: OperationResult = PENDING_TIMEOUT
    expect(ok).toBe(true)
    expect(fail).toBe(false)
    expect(timeout).toBe(PENDING_TIMEOUT)
  })
})