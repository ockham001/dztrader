import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useSysmonStore } from '../sysmon'
import { sysmonApi } from '@/api/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn() },
}))

describe('useSysmonStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('init 经 REST 补拉镜像并写入快照（仅首次）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const store = useSysmonStore()

    await store.init()
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
    expect(store.loaded).toBe(true)
    expect(store.snapshot?.system.hostname).toBe('trade-01')
    expect(store.error).toBeNull()

    // 已加载后不再补拉：后续刷新只由 WS 推送承担（发送方 5s 基线，前端不轮询）
    await store.init()
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })

  it('init 遇到 503（dzsysmon 未上报）时保持无快照态，不抛错', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const store = useSysmonStore()

    await store.init()
    expect(store.snapshot).toBeNull()
    expect(store.loaded).toBe(true)
    expect(store.error).toBe('HTTP 503')
  })

  it('applySnapshot 幂等全量覆盖（WS 增量 / snapshot 快照域共用）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.2 }))
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.6 }))

    expect(store.snapshot?.cpu.total.usage_ratio).toBe(0.6)
    expect(store.cpuUsageRatio).toBe(0.6)
  })

  it('applySnapshot 忽略非法 payload（不写不抛）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.2 }))

    for (const invalid of [null, undefined, 'bad', {}, { timestamp: 'x' }]) {
      store.applySnapshot(invalid)
    }
    expect(store.snapshot?.cpu.total.usage_ratio).toBe(0.2)
  })

  it('cpuUsageRatio 仅在可用且数值有效时返回，否则 null（不得用 0 冒充）', () => {
    const store = useSysmonStore()

    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.42 }))
    expect(store.cpuUsageRatio).toBe(0.42)

    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: null }))
    expect(store.cpuUsageRatio).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      cpuUsageRatio: 0.42,
      metrics: { 'cpu.total': { available: false, reason: 'unsupported' } },
    }))
    expect(store.cpuUsageRatio).toBeNull()

    // NaN/Infinity 一律视为不可用，不得用 0 冒充
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: Number.NaN }))
    expect(store.cpuUsageRatio).toBeNull()
  })

  it('availabilityOf 返回指标可用性条目（缺失 = undefined）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())

    expect(store.availabilityOf('system')).toEqual({ available: true })
    expect(store.availabilityOf('memory')).toEqual({ available: false, reason: 'not_implemented' })
    expect(store.availabilityOf('not-a-metric')).toBeUndefined()
  })
})

describe('useSysmonStore CPU 区块 computed（#72）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('cpuCores 在可用时返回每核数组，并逐项防御 NaN/Infinity', () => {
    const store = useSysmonStore()
    // 无快照：空数组，不得伪造 0
    expect(store.cpuCores).toEqual([])
    expect(store.saturatedCores).toEqual([])

    store.applySnapshot(makeSysmonSnapshot())
    expect(store.cpuCores).toHaveLength(16)
    expect(store.cpuCores.map(core => core.index)).toEqual(Array.from({ length: 16 }, (_, index) => index))
    expect(store.cpuCores.every(core => core.usage_ratio !== null && core.saturated === false)).toBe(true)

    store.applySnapshot(makeSysmonSnapshot({
      cpuCores: [
        { index: 3, usage_ratio: Number.NaN, saturated: true },
        { index: 7, usage_ratio: Number.POSITIVE_INFINITY, saturated: false },
      ],
    }))
    expect(store.cpuCores).toEqual([
      { index: 3, usage_ratio: null, saturated: true },
      { index: 7, usage_ratio: null, saturated: false },
    ])
  })

  it('cpu.cores 不可用时 cpuCores / saturatedCores 为空数组（不用陈旧数据冒充）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      saturatedCores: [1, 2],
      metrics: { 'cpu.cores': { available: false, reason: 'not_implemented' } },
    }))

    expect(store.cpuCores).toEqual([])
    expect(store.saturatedCores).toEqual([])
  })

  it('saturatedCores 返回去重升序的饱和核 index，且与 core.saturated 一致', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ saturatedCores: [5, 2, 5] }))

    expect(store.saturatedCores).toEqual([2, 5])
    expect(store.cpuCores.filter(core => core.saturated).map(core => core.index)).toEqual([2, 5])
  })

  it('cpuLoadAverage 仅在可用且存在时返回，NaN/Infinity 视作不可用', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.cpuLoadAverage).toEqual({
      one: 0.42,
      five: 0.51,
      fifteen: 0.63,
      semantic: 'load_average',
    })

    store.applySnapshot(makeSysmonSnapshot({ cpuLoadAverage: null }))
    expect(store.cpuLoadAverage).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      cpuLoadAverage: {
        one: Number.NaN,
        five: Number.POSITIVE_INFINITY,
        fifteen: 0.63,
        semantic: 'load_average',
      },
    }))
    expect(store.cpuLoadAverage).toEqual({ one: null, five: null, fifteen: 0.63, semantic: 'load_average' })

    store.applySnapshot(makeSysmonSnapshot({
      metrics: { 'cpu.load_average': { available: false, reason: 'unsupported' } },
    }))
    expect(store.cpuLoadAverage).toBeNull()
  })

  it('cpuContextSwitchesPerSec / cpuInterruptsPerSec 仅可用且数值有效时返回', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.cpuContextSwitchesPerSec).toBe(1234)
    expect(store.cpuInterruptsPerSec).toBe(567)

    store.applySnapshot(makeSysmonSnapshot({ contextSwitchesPerSec: null, interruptsPerSec: null }))
    expect(store.cpuContextSwitchesPerSec).toBeNull()
    expect(store.cpuInterruptsPerSec).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      contextSwitchesPerSec: Number.NaN,
      interruptsPerSec: Number.NEGATIVE_INFINITY,
    }))
    expect(store.cpuContextSwitchesPerSec).toBeNull()
    expect(store.cpuInterruptsPerSec).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      metrics: { 'cpu.context_switches': { available: false, reason: 'unsupported' } },
    }))
    expect(store.cpuContextSwitchesPerSec).toBeNull()
    expect(store.cpuInterruptsPerSec).toBeNull()
  })

  it('semanticHint 从 availability.semantic_hints 取值，缺失返回空串', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.semanticHint('cpu.load_average')).toBe('')

    store.applySnapshot(makeSysmonSnapshot({
      semanticHints: { 'cpu.load_average': 'linux load average from /proc/loadavg' },
    }))
    expect(store.semanticHint('cpu.load_average')).toBe('linux load average from /proc/loadavg')
    expect(store.semanticHint('other.key')).toBe('')
  })
})
