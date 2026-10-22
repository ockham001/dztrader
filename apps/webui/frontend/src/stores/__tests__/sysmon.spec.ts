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
    // #74 起后端每帧尝试采集内存 / 交换：默认夹具二者可用
    expect(store.availabilityOf('memory')).toEqual({ available: true })
    expect(store.availabilityOf('swap')).toEqual({ available: true })
    // #75 起后端采集卷 / 共享内存目录：默认夹具二者可用
    expect(store.availabilityOf('volumes')).toEqual({ available: true })
    expect(store.availabilityOf('shm_dir')).toEqual({ available: true })
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

describe('useSysmonStore 温度与每核频率 computed（#73，可用性驱动）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('默认快照按 #73 语义将 temperature / cpu.frequency 标记为不可用', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())

    // Linux 无 hwmon 传感器 → no_sensor；无 cpufreq → unsupported（契约 sysmon §2.5）。
    expect(store.availabilityOf('temperature')).toEqual({ available: false, reason: 'no_sensor' })
    expect(store.availabilityOf('cpu.frequency')).toEqual({ available: false, reason: 'unsupported' })
  })

  it('temperatures 仅在 temperature 可用时返回过滤后的读数，否则空数组', () => {
    const store = useSysmonStore()
    // 无快照 / 默认不可用：空数组，不得伪造 0
    expect(store.temperatures).toEqual([])
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.temperatures).toEqual([])

    store.applySnapshot(makeSysmonSnapshot({ sensorData: true }))
    expect(store.temperatures).toEqual([
      { sensor: 'coretemp#hwmon3:Package id 0', celsius: 45.5 },
      { sensor: 'coretemp#hwmon3:Core 0', celsius: 41.5 },
    ])

    // 数据仍在但可用性标记不可用：不得用陈旧数据冒充
    store.applySnapshot(makeSysmonSnapshot({
      sensorData: true,
      metrics: { temperature: { available: false, reason: 'no_sensor' } },
    }))
    expect(store.temperatures).toEqual([])

    // 过滤空 sensor / 非有限 celsius（可用性显式标记可用）
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { temperature: { available: true } },
      temperatures: [
        { sensor: '', celsius: 30 },
        { sensor: '   ', celsius: 31 },
        { sensor: 'coretemp:Core 0', celsius: Number.NaN },
        { sensor: 'coretemp:Core 1', celsius: Number.POSITIVE_INFINITY },
        { sensor: ' coretemp:Core 2 ', celsius: 45.5 },
      ],
    }))
    expect(store.temperatures).toEqual([{ sensor: 'coretemp:Core 2', celsius: 45.5 }])

    // 可用但快照为 null：空数组（视图显示「暂无数据」）
    store.applySnapshot(makeSysmonSnapshot({ sensorData: true, temperatures: null }))
    expect(store.temperatures).toEqual([])
  })

  it('coreFrequencies 仅在 cpu.frequency 可用时返回过滤后的频率，否则空数组', () => {
    const store = useSysmonStore()
    // 无快照 / 默认不可用：空数组，不得伪造 0
    expect(store.coreFrequencies).toEqual([])
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.coreFrequencies).toEqual([])

    store.applySnapshot(makeSysmonSnapshot({ sensorData: true }))
    expect(store.coreFrequencies).toHaveLength(16)
    expect(store.coreFrequencies[0]).toEqual({ index: 0, mhz: 3600 })
    expect(store.coreFrequencies[15]).toEqual({ index: 15, mhz: 5100 })

    // 数据仍在但可用性标记不可用：不得用陈旧数据冒充
    store.applySnapshot(makeSysmonSnapshot({
      sensorData: true,
      metrics: { 'cpu.frequency': { available: false, reason: 'unsupported' } },
    }))
    expect(store.coreFrequencies).toEqual([])

    // 过滤非法 index / 非正整频率（可用性显式标记可用）
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { 'cpu.frequency': { available: true } },
      coreFrequencies: [
        { index: -1, mhz: 3600 },
        { index: 1.5, mhz: 3600 },
        { index: 2, mhz: 0 },
        { index: 3, mhz: -100 },
        { index: 4, mhz: Number.NaN },
        { index: 5, mhz: Number.POSITIVE_INFINITY },
        { index: 6, mhz: 3600 },
      ],
    }))
    expect(store.coreFrequencies).toEqual([{ index: 6, mhz: 3600 }])

    // 可用但快照为 null：空数组（视图显示「暂无数据」）
    store.applySnapshot(makeSysmonSnapshot({ sensorData: true, coreFrequencies: null }))
    expect(store.coreFrequencies).toEqual([])
  })
})

describe('useSysmonStore 内存与交换 computed（#74，可用性驱动）', () => {
  const GIB = 1024 ** 3
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无快照时 memoryInfo / swapInfo 均为 null（不得伪造 0）', () => {
    const store = useSysmonStore()

    expect(store.memoryInfo).toBeNull()
    expect(store.swapInfo).toBeNull()
  })

  it('默认快照（#74 形状）返回净化后的内存 / 交换对象', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())

    expect(store.memoryInfo).toEqual({
      total_bytes: 32 * GIB,
      available_bytes: 16 * GIB,
      used_bytes: 16 * GIB,
      used_ratio: 0.5,
      cached_bytes: 8 * GIB,
      buffers_bytes: 256 * MIB,
    })
    expect(store.swapInfo).toEqual({
      total_bytes: 8 * GIB,
      used_bytes: 2 * GIB,
      used_ratio: 0.25,
    })
  })

  it('memory / swap 不可用时返回 null（不渲染陈旧数据）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      metrics: {
        memory: { available: false, reason: 'collect_failed' },
        swap: { available: false, reason: 'unsupported' },
      },
    }))

    expect(store.memoryInfo).toBeNull()
    expect(store.swapInfo).toBeNull()
  })

  it('可用但本帧 payload 为 null（能力粘性 / 未采集）时返回 null，视图显示「暂无数据」', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ memory: null, swap: null }))

    expect(store.availabilityOf('memory')).toEqual({ available: true })
    expect(store.memoryInfo).toBeNull()
    expect(store.swapInfo).toBeNull()
  })

  it('内存核心口径防御：NaN/Infinity/负数/总量 0/可用 > 总量 → null，且 used 按唯一口径重算', () => {
    const store = useSysmonStore()

    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: Number.NaN, available_bytes: 0, used_bytes: 0, used_ratio: null, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: 32 * GIB, available_bytes: Number.POSITIVE_INFINITY, used_bytes: 0, used_ratio: null, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: 32 * GIB, available_bytes: -1, used_bytes: 0, used_ratio: null, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toBeNull()

    // 总量 0 视为本次采集失败（契约 sysmon §2.4），不得按「无内存」渲染
    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: 0, available_bytes: 0, used_bytes: 0, used_ratio: null, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: 16 * GIB, available_bytes: 32 * GIB, used_bytes: 0, used_ratio: 0.5, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toBeNull()

    // used_bytes 与唯一口径不一致时以 total − available 为准（不信任发送端的 used）
    store.applySnapshot(makeSysmonSnapshot({
      memory: { total_bytes: 32 * GIB, available_bytes: 24 * GIB, used_bytes: 1, used_ratio: 0.25, cached_bytes: null, buffers_bytes: null },
    }))
    expect(store.memoryInfo).toEqual({
      total_bytes: 32 * GIB,
      available_bytes: 24 * GIB,
      used_bytes: 8 * GIB,
      used_ratio: 0.25,
      cached_bytes: null,
      buffers_bytes: null,
    })
  })

  it('内存信息项防御：used_ratio 由 used/total 派生（不信任 payload），缓存与缓冲非法 → null', () => {
    const store = useSysmonStore()
    const base = {
      total_bytes: 32 * GIB,
      available_bytes: 16 * GIB,
      used_bytes: 16 * GIB,
    }

    // payload 比率与字节口径矛盾时按 used/total 派生（契约 sysmon §2.4），保证两者一致。
    store.applySnapshot(makeSysmonSnapshot({
      memory: { ...base, used_ratio: 1.5, cached_bytes: -1, buffers_bytes: Number.NaN },
    }))
    expect(store.memoryInfo).toEqual({
      ...base,
      used_ratio: 0.5,
      cached_bytes: null,
      buffers_bytes: null,
    })

    store.applySnapshot(makeSysmonSnapshot({
      memory: { ...base, used_ratio: -0.1, cached_bytes: 1.5, buffers_bytes: Number.POSITIVE_INFINITY },
    }))
    expect(store.memoryInfo).toEqual({
      ...base,
      used_ratio: 0.5,
      cached_bytes: null,
      buffers_bytes: null,
    })

    // Windows 式：缓存有值、缓冲恒 null（本平台不提供，不是 0）
    store.applySnapshot(makeSysmonSnapshot({
      memory: { ...base, used_ratio: 0.5, cached_bytes: 4 * GIB, buffers_bytes: null },
    }))
    expect(store.memoryInfo?.cached_bytes).toBe(4 * GIB)
    expect(store.memoryInfo?.buffers_bytes).toBeNull()
  })

  it('交换防御：total=0 仍返回对象（无交换分区），used 钳制到 total，非法数值 → null', () => {
    const store = useSysmonStore()

    // 无交换分区：payload 为 total=0 的对象，比率为 null（契约 sysmon §2.4）
    store.applySnapshot(makeSysmonSnapshot({ swap: { total_bytes: 0, used_bytes: 0, used_ratio: null } }))
    expect(store.swapInfo).toEqual({ total_bytes: 0, used_bytes: 0, used_ratio: null })

    // 发送端比率与 total=0 矛盾时按契约置 null
    store.applySnapshot(makeSysmonSnapshot({ swap: { total_bytes: 0, used_bytes: 0, used_ratio: 0.5 } }))
    expect(store.swapInfo).toEqual({ total_bytes: 0, used_bytes: 0, used_ratio: null })

    // used 超过 total：钳制到 total（不得出现已用 > 总量），比率按钳制后 used/total 派生
    store.applySnapshot(makeSysmonSnapshot({
      swap: { total_bytes: 8 * GIB, used_bytes: 32 * GIB, used_ratio: 1.5 },
    }))
    expect(store.swapInfo).toEqual({ total_bytes: 8 * GIB, used_bytes: 8 * GIB, used_ratio: 1 })

    // payload 比率与字节口径矛盾时按 used/total 派生（不信任 payload）
    store.applySnapshot(makeSysmonSnapshot({
      swap: { total_bytes: 8 * GIB, used_bytes: 2 * GIB, used_ratio: 0.9 },
    }))
    expect(store.swapInfo).toEqual({ total_bytes: 8 * GIB, used_bytes: 2 * GIB, used_ratio: 0.25 })

    for (const swap of [
      { total_bytes: Number.NaN, used_bytes: 0, used_ratio: null },
      { total_bytes: 8 * GIB, used_bytes: -1, used_ratio: 0.25 },
      { total_bytes: 8 * GIB, used_bytes: Number.POSITIVE_INFINITY, used_ratio: 0.25 },
    ]) {
      store.applySnapshot(makeSysmonSnapshot({ swap }))
      expect(store.swapInfo).toBeNull()
    }
  })
})

describe('useSysmonStore 卷 / 关注路径 / 共享内存目录 computed（#75，可用性驱动）', () => {
  const GIB = 1024 ** 3
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无快照时 volumes / watchPaths / shmDirInfo 为空（不得伪造 0）', () => {
    const store = useSysmonStore()

    expect(store.volumes).toEqual([])
    expect(store.watchPaths).toEqual([])
    expect(store.shmDirInfo).toBeNull()
  })

  it('默认快照（#75 形状）返回净化后的卷 / 关注路径 / 共享内存目录', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())

    // watched 优先排序，其次按 mount 升序：/data 虽是数组第二项，仍排到首位
    expect(store.volumes.map(volume => volume.mount)).toEqual(['/data', '/', '/home'])
    expect(store.volumes[0]).toEqual({
      mount: '/data',
      watched: true,
      available: true,
      total_bytes: 1024 * GIB,
      used_bytes: 256 * GIB,
      used_ratio: 0.25,
    })
    expect(store.volumes[1].used_ratio).toBe(0.4)
    expect(store.volumes[2].used_ratio).toBe(0.62)

    expect(store.watchPaths).toEqual([
      { path: '/srv/dztrader/shm', mount: '/data', available: true },
      { path: '/srv/dztrader/db', mount: '/data', available: true },
    ])
    expect(store.shmDirInfo).toEqual({
      path: '/srv/dztrader/shm',
      total_bytes: 720 * MIB,
      page_files: 12,
      oldest_file_age_sec: 480,
    })
  })

  it('volumes 不可用时为空数组（不渲染陈旧数据），watchPaths 不受 volumes 开关影响', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { volumes: { available: false, reason: 'disabled_by_config' } },
    }))

    expect(store.volumes).toEqual([])
    // 契约 sysmon §2.5：配置关闭 volumes 后 watch_paths 仍输出（告警需要它）
    expect(store.watchPaths).toHaveLength(2)
  })

  it('shm_dir 不可用 / 可用但本帧对象缺失：返回 null（视图区分不可用与暂无数据）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { shm_dir: { available: false, reason: 'collect_failed' } },
    }))
    expect(store.shmDirInfo).toBeNull()

    store.applySnapshot(makeSysmonSnapshot({ shmDir: null }))
    expect(store.availabilityOf('shm_dir')).toEqual({ available: true })
    expect(store.shmDirInfo).toBeNull()
  })

  it('单个卷 available=false：保留卷（能显示不可用）但容量字段净化成 null', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/backup', watched: false, available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))

    expect(store.volumes).toHaveLength(2)
    const backup = store.volumes.find(volume => volume.mount === '/backup')
    expect(backup).toEqual({
      mount: '/backup',
      watched: false,
      available: false,
      total_bytes: null,
      used_bytes: null,
      used_ratio: null,
      reason: 'no_permission',
    })
  })

  it('卷净化：脏 mount 剔除，used 钳制到 total，比率按 used/total 派生（不信任 payload）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '', watched: false, available: true, total_bytes: 1 * GIB, used_bytes: 0, used_ratio: 0 },
        { mount: '   ', watched: false, available: true, total_bytes: 1 * GIB, used_bytes: 0, used_ratio: 0 },
        { mount: '/', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 400 * GIB, used_ratio: 2 },
        { mount: '/data', watched: true, available: true, total_bytes: 100 * GIB, used_bytes: 10 * GIB, used_ratio: 0.9 },
      ],
    }))

    expect(store.volumes.map(volume => volume.mount)).toEqual(['/data', '/'])
    const root = store.volumes[1]
    expect(root.used_bytes).toBe(100 * GIB)
    expect(root.used_ratio).toBe(1)
    const data = store.volumes[0]
    expect(data.used_bytes).toBe(10 * GIB)
    expect(data.used_ratio).toBe(0.1)
  })

  it('标称可用但容量字段非法（NaN/Infinity/负数/非整数）：按不可读处理，不得用 0 冒充', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/a', watched: false, available: true, total_bytes: Number.NaN, used_bytes: 0, used_ratio: 0 },
        { mount: '/b', watched: false, available: true, total_bytes: Number.POSITIVE_INFINITY, used_bytes: 0, used_ratio: 0 },
        { mount: '/c', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: -1, used_ratio: 0 },
        { mount: '/d', watched: false, available: true, total_bytes: 1.5, used_bytes: 0, used_ratio: 0 },
      ],
    }))

    expect(store.volumes).toHaveLength(4)
    for (const volume of store.volumes) {
      expect(volume.available).toBe(false)
      expect(volume.total_bytes).toBeNull()
      expect(volume.used_bytes).toBeNull()
      expect(volume.used_ratio).toBeNull()
      expect(volume.reason).toBe('collect_failed')
    }
  })

  it('total_bytes=0 的特殊卷：保留 available=true，比率 null（不是 0%）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/special', watched: false, available: true, total_bytes: 0, used_bytes: 0, used_ratio: 0 },
      ],
    }))

    expect(store.volumes).toEqual([
      {
        mount: '/special',
        watched: false,
        available: true,
        total_bytes: 0,
        used_bytes: 0,
        used_ratio: null,
      },
    ])
  })

  it('watchPaths 防御：空 path 剔除，available=false 强制 mount=null 且补 reason', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      watchPaths: [
        { path: '', mount: '/', available: true },
        { path: '  ', mount: '/', available: true },
        { path: '/srv/shm', mount: '/data', available: true },
        // 解析失败：发送端不得猜卷，前端再兜一层（mount 置 null）
        { path: '/srv/missing', mount: '/', available: false, reason: 'path_not_found' },
        { path: '/srv/opaque', mount: null, available: true },
      ],
    }))

    expect(store.watchPaths).toEqual([
      { path: '/srv/shm', mount: '/data', available: true },
      { path: '/srv/missing', mount: null, available: false, reason: 'path_not_found' },
      { path: '/srv/opaque', mount: null, available: false, reason: 'collect_failed' },
    ])
  })

  it('watchPaths 可用但本帧数组缺失：空数组（视图不渲染该子区块）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ watchPaths: null }))
    expect(store.watchPaths).toEqual([])
  })

  it('shmDirInfo 净化：path 非法 / 计数字段非法整块 null；最旧页文件年龄非法 → null', () => {
    const store = useSysmonStore()

    for (const shmDir of [
      { path: '', total_bytes: 1 * MIB, page_files: 1, oldest_file_age_sec: 0 },
      { path: '/srv/shm', total_bytes: Number.NaN, page_files: 1, oldest_file_age_sec: 0 },
      { path: '/srv/shm', total_bytes: -1, page_files: 1, oldest_file_age_sec: 0 },
      { path: '/srv/shm', total_bytes: 1 * MIB, page_files: Number.POSITIVE_INFINITY, oldest_file_age_sec: 0 },
      { path: '/srv/shm', total_bytes: 1 * MIB, page_files: 1.5, oldest_file_age_sec: 0 },
    ]) {
      store.applySnapshot(makeSysmonSnapshot({ shmDir }))
      expect(store.shmDirInfo).toBeNull()
    }

    // 无页文件：oldest_file_age_sec=null（视图不渲染该行，不是 0 秒）
    store.applySnapshot(makeSysmonSnapshot({
      shmDir: { path: '/srv/shm', total_bytes: 0, page_files: 0, oldest_file_age_sec: null },
    }))
    expect(store.shmDirInfo).toEqual({
      path: '/srv/shm',
      total_bytes: 0,
      page_files: 0,
      oldest_file_age_sec: null,
    })

    store.applySnapshot(makeSysmonSnapshot({
      shmDir: { path: '/srv/shm', total_bytes: 1 * MIB, page_files: 1, oldest_file_age_sec: -5 },
    }))
    expect(store.shmDirInfo?.oldest_file_age_sec).toBeNull()
  })
})
