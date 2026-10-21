import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { usePending, PENDING_TIMEOUT } from '../usePending'
import { createOperationRunner } from '../useOperation'

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
})