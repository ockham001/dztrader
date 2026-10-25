import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  SYSMON_CONFIG_TIMEOUT_MS,
  SYSMON_CONFIG_KEY,
  SYSMON_NETWORK_CONFIG_KEY,
  sysmonPending,
  useSysmonStore,
} from '../sysmon'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import { sysmonApi } from '@/api/sysmon'
import { useToastStore } from '@/stores/toast'
import { makeSysmonConfig, makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn() },
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
    // #76 起后端采集磁盘 I/O：默认夹具可用
    expect(store.availabilityOf('disk_io')).toEqual({ available: true })
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

    // #122：按使用率降序、同值按挂载点升序，不依赖发送端顺序（payload 顺序为 /、/data、/home）
    expect(store.volumes.map(volume => volume.mount)).toEqual(['/home', '/', '/data'])
    expect(store.volumes[0]).toEqual({
      mount: '/home',
      watched: false,
      available: true,
      total_bytes: 50 * GIB,
      used_bytes: 31 * GIB,
      used_ratio: 0.62,
    })
    expect(store.volumes[1].used_ratio).toBe(0.4)
    expect(store.volumes[2].used_ratio).toBe(0.25)

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
    // #122 两个分池同样为空：不可用时不得把陈旧卷落到任一侧
    expect(store.readableVolumes).toEqual([])
    expect(store.zeroCapacityVolumes).toEqual([])
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

    // #122：按使用率降序 —— / 钳制后为 100%，排在 10% 的 /data 之前
    expect(store.volumes.map(volume => volume.mount)).toEqual(['/', '/data'])
    const root = store.volumes[0]
    expect(root.used_bytes).toBe(100 * GIB)
    expect(root.used_ratio).toBe(1)
    const data = store.volumes[1]
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

  it('#122 排序：按使用率降序、同值按挂载点升序，不依赖发送端顺序', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/b-tie', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 50 * GIB, used_ratio: 0.5 },
        { mount: '/low', watched: true, available: true, total_bytes: 100 * GIB, used_bytes: 10 * GIB, used_ratio: 0.1 },
        { mount: '/a-tie', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 50 * GIB, used_ratio: 0.5 },
        { mount: '/high', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 90 * GIB, used_ratio: 0.9 },
      ],
    }))

    // 排序只看使用率与挂载点：关注标记不参与（取消关注路径的契约由 #121 定稿，字段本身由 #124 删除）
    expect(store.volumes.map(volume => volume.mount)).toEqual(['/high', '/a-tie', '/b-tie', '/low'])
  })

  it('#122 无容量挂载分池：容量不可读与 total=0 的卷单列，正文列表只剩可读容量的卷', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/run/user/1000/gnome-remote-desktop/cliprdr-abc', watched: false, available: true, total_bytes: 0, used_bytes: 0, used_ratio: null },
        { mount: '/backup', watched: false, available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))

    // 采集语义不变：三条都在 volumes 里（available=true + total=0 是合法事实）
    expect(store.volumes).toHaveLength(3)
    expect(store.readableVolumes.map(volume => volume.mount)).toEqual(['/'])
    expect(store.zeroCapacityVolumes.map(volume => volume.mount)).toEqual([
      '/backup',
      '/run/user/1000/gnome-remote-desktop/cliprdr-abc',
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

describe('useSysmonStore 磁盘 I/O computed（#76，可用性驱动）', () => {
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无快照时 diskIo 为空数组（不得伪造 0）', () => {
    const store = useSysmonStore()

    expect(store.diskIo).toEqual([])
  })

  it('默认快照（#76 形状）返回净化后的磁盘数组，并按 device 升序排序', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())

    expect(store.availabilityOf('disk_io')).toEqual({ available: true })
    // payload 顺序为 sda、nvme0n1；store 按 device localeCompare 升序重排
    expect(store.diskIo.map(device => device.device)).toEqual(['nvme0n1', 'sda'])
    expect(store.diskIo[0]).toEqual({
      device: 'nvme0n1',
      read_bytes_per_sec: 64 * MIB,
      write_bytes_per_sec: 32 * MIB,
      read_iops: 2048,
      write_iops: 1024,
      latency_ms: 0.45,
      queue_depth: 1.25,
      utilization: 0.72,
    })
    expect(store.diskIo[1].latency_ms).toBe(1.25)
    expect(store.diskIo[1].utilization).toBe(0.35)
  })

  it('disk_io 不可用时为空数组（不渲染陈旧数据）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { disk_io: { available: false, reason: 'no_permission' } },
    }))

    expect(store.diskIo).toEqual([])
  })

  it('可用但本帧 null / 空数组：空数组（视图显示「暂无数据」）', () => {
    const store = useSysmonStore()

    store.applySnapshot(makeSysmonSnapshot({ diskIo: null }))
    expect(store.availabilityOf('disk_io')).toEqual({ available: true })
    expect(store.diskIo).toEqual([])

    store.applySnapshot(makeSysmonSnapshot({ diskIo: [] }))
    expect(store.diskIo).toEqual([])
  })

  it('净化：空 device 剔除；负 / NaN / Infinity 数值 → null；合法 0 保持 0', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      diskIo: [
        {
          device: '',
          read_bytes_per_sec: 1,
          write_bytes_per_sec: 1,
          read_iops: 1,
          write_iops: 1,
          latency_ms: 1,
          queue_depth: 1,
          utilization: 1,
        },
        {
          device: '   ',
          read_bytes_per_sec: 1,
          write_bytes_per_sec: 1,
          read_iops: 1,
          write_iops: 1,
          latency_ms: 1,
          queue_depth: 1,
          utilization: 1,
        },
        {
          device: ' sda ',
          read_bytes_per_sec: -1,
          write_bytes_per_sec: Number.NaN,
          read_iops: Number.POSITIVE_INFINITY,
          write_iops: -0.5,
          latency_ms: Number.NEGATIVE_INFINITY,
          queue_depth: -2,
          utilization: 0.5,
        },
        {
          device: 'nvme0n1',
          read_bytes_per_sec: 0,
          write_bytes_per_sec: 0,
          read_iops: 0,
          write_iops: 0,
          latency_ms: 0,
          queue_depth: 0,
          utilization: 0,
        },
      ],
    }))

    expect(store.diskIo.map(device => device.device)).toEqual(['nvme0n1', 'sda'])
    expect(store.diskIo[1]).toEqual({
      device: 'sda',
      read_bytes_per_sec: null,
      write_bytes_per_sec: null,
      read_iops: null,
      write_iops: null,
      latency_ms: null,
      queue_depth: null,
      utilization: 0.5,
    })
    // 0 是合法真实值（计数器有效但当前无 I/O），不得被净化成 null
    expect(store.diskIo[0]).toEqual({
      device: 'nvme0n1',
      read_bytes_per_sec: 0,
      write_bytes_per_sec: 0,
      read_iops: 0,
      write_iops: 0,
      latency_ms: 0,
      queue_depth: 0,
      utilization: 0,
    })
  })

  it('utilization 必须是 [0,1] 比率：越界 / 非有限 → null，null 保持 null 不变成 0', () => {
    const store = useSysmonStore()
    const base = {
      read_bytes_per_sec: null,
      write_bytes_per_sec: null,
      read_iops: null,
      write_iops: null,
      latency_ms: null,
      queue_depth: null,
    }
    store.applySnapshot(makeSysmonSnapshot({
      diskIo: [
        { ...base, device: 'a', utilization: 1.5 },
        { ...base, device: 'b', utilization: -0.1 },
        { ...base, device: 'c', utilization: Number.NaN },
        { ...base, device: 'd', utilization: Number.POSITIVE_INFINITY },
        { ...base, device: 'e', utilization: null },
        { ...base, device: 'f', utilization: 0 },
        { ...base, device: 'g', utilization: 1 },
      ],
    }))

    expect(store.diskIo.map(device => [device.device, device.utilization])).toEqual([
      ['a', null],
      ['b', null],
      ['c', null],
      ['d', null],
      ['e', null],
      ['f', 0],
      ['g', 1],
    ])
    // null 保持 null（视图显示「不可用」），不得变成 0
    expect(store.diskIo[4].read_bytes_per_sec).toBeNull()
  })
})

describe('useSysmonStore network computed（#77，可用性驱动）', () => {
  const GIB = 1024 ** 3
  const MIB = 1024 ** 2

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无快照时 network 为空数组（不得伪造 0）', () => {
    const store = useSysmonStore()

    expect(store.network).toEqual([])
    expect(store.networkFilteredAll).toBe(false)
  })

  it('默认快照（network 不可用）为空数组；resourceData 快照返回净化后的网卡数组', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot())
    // 夹具默认 network 不可用（#77 之前的历史形状）：store 只认 available，不推断 reason
    expect(store.availabilityOf('network')?.available).toBe(false)
    expect(store.network).toEqual([])

    store.applySnapshot(makeSysmonSnapshot({ resourceData: true }))
    expect(store.availabilityOf('network')).toEqual({ available: true })
    expect(store.network).toEqual([{
      interface: 'eth0',
      rx_bytes_total: 8 * GIB,
      tx_bytes_total: 4 * GIB,
      rx_packets_total: 12_000_000,
      tx_packets_total: 9_000_000,
      rx_errors_total: 3,
      tx_errors_total: 1,
      rx_drops_total: 7,
      tx_drops_total: 2,
      rx_bytes_per_sec: 1 * MIB,
      tx_bytes_per_sec: 2 * MIB,
      rx_errors_per_sec: 0,
      tx_errors_per_sec: 0,
      rx_drops_per_sec: 0,
      tx_drops_per_sec: 0,
      is_virtual: false,
    }])
  })

  it('network 不可用时为空数组（不渲染陈旧数据）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      metrics: { network: { available: false, reason: 'disabled_by_config' } },
    }))

    expect(store.network).toEqual([])
    expect(store.networkFilteredAll).toBe(false)
  })

  it('按 interface 升序排序，不依赖发送端顺序（前端不做 hide_virtual 过滤）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ virtualNetwork: true }))

    expect(store.network.map(nic => nic.interface)).toEqual(['docker0', 'eth0', 'lo', 'veth1234'])
    // 虚拟网卡原样保留：隐藏是发送方义务（契约 §2.4），前端只展示
    expect(store.network.filter(nic => nic.is_virtual).map(nic => nic.interface))
      .toEqual(['docker0', 'lo', 'veth1234'])
  })

  it('空数组 + available=true：networkFilteredAll=true（"全部被滤掉"不是"采集失败"，契约 §2.5）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ network: [] }))

    expect(store.availabilityOf('network')).toEqual({ available: true })
    expect(store.network).toEqual([])
    expect(store.networkFilteredAll).toBe(true)
  })

  it('nullable：network=null 时 collected=false、filteredAll=false（"本帧未采集"区别于"空数组"）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ network: null }))

    expect(store.availabilityOf('network')).toEqual({ available: true })
    expect(store.network).toEqual([])
    expect(store.networkFilteredAll).toBe(false)
  })

  it('净化：interface 空白剔除；六个速率 0 合法保留、负数/NaN/Infinity → null', () => {
    const store = useSysmonStore()
    const base = {
      rx_bytes_total: 1,
      tx_bytes_total: 2,
      rx_packets_total: 3,
      tx_packets_total: 4,
      rx_errors_total: 5,
      tx_errors_total: 6,
      rx_drops_total: 7,
      tx_drops_total: 8,
      is_virtual: false,
    }
    store.applySnapshot(makeSysmonSnapshot({
      network: [
        { ...base, interface: '', rx_bytes_per_sec: 1, tx_bytes_per_sec: 1, rx_errors_per_sec: 1, tx_errors_per_sec: 1, rx_drops_per_sec: 1, tx_drops_per_sec: 1 },
        { ...base, interface: '   ', rx_bytes_per_sec: 1, tx_bytes_per_sec: 1, rx_errors_per_sec: 1, tx_errors_per_sec: 1, rx_drops_per_sec: 1, tx_drops_per_sec: 1 },
        {
          ...base,
          interface: 'bad',
          rx_bytes_per_sec: -1,
          tx_bytes_per_sec: Number.NaN,
          rx_errors_per_sec: Number.POSITIVE_INFINITY,
          tx_errors_per_sec: Number.NEGATIVE_INFINITY,
          rx_drops_per_sec: -0.5,
          tx_drops_per_sec: null,
        },
        {
          ...base,
          interface: 'zero',
          rx_bytes_per_sec: 0,
          tx_bytes_per_sec: 0,
          rx_errors_per_sec: 0,
          tx_errors_per_sec: 0,
          rx_drops_per_sec: 0,
          tx_drops_per_sec: 0,
        },
      ],
    }))

    expect(store.network.map(nic => nic.interface)).toEqual(['bad', 'zero'])
    expect(store.network[0]).toMatchObject({
      rx_bytes_per_sec: null,
      tx_bytes_per_sec: null,
      rx_errors_per_sec: null,
      tx_errors_per_sec: null,
      rx_drops_per_sec: null,
      tx_drops_per_sec: null,
    })
    // 0 是合法真实值（本窗口确实没有增量），不得被净化成 null
    expect(store.network[1]).toMatchObject({
      rx_bytes_per_sec: 0,
      tx_bytes_per_sec: 0,
      rx_errors_per_sec: 0,
      tx_errors_per_sec: 0,
      rx_drops_per_sec: 0,
      tx_drops_per_sec: 0,
    })
  })

  it('八个累计计数器非法（负/小数/NaN/Infinity/缺失）：整条剔除（累计量没有"不可用"语义）', () => {
    const store = useSysmonStore()
    const ok = {
      interface: 'ok',
      rx_bytes_total: 1,
      tx_bytes_total: 1,
      rx_packets_total: 1,
      tx_packets_total: 1,
      rx_errors_total: 1,
      tx_errors_total: 1,
      rx_drops_total: 1,
      tx_drops_total: 1,
      rx_bytes_per_sec: null,
      tx_bytes_per_sec: null,
      rx_errors_per_sec: null,
      tx_errors_per_sec: null,
      rx_drops_per_sec: null,
      tx_drops_per_sec: null,
      is_virtual: false,
    }
    const badTotals: Array<Record<string, unknown>> = [
      { rx_bytes_total: -1 },
      { tx_bytes_total: 1.5 },
      { rx_packets_total: Number.NaN },
      { tx_packets_total: Number.POSITIVE_INFINITY },
      { rx_errors_total: null },
      { tx_drops_total: -0.5 },
      { rx_drops_total: undefined },
    ]
    store.applySnapshot(makeSysmonSnapshot({
      network: [
        ok,
        ...badTotals.map(bad => ({ ...ok, ...bad, interface: `bad-${String(Object.keys(bad)[0])}` })),
      ] as never,
    }))

    expect(store.network.map(nic => nic.interface)).toEqual(['ok'])
  })

  it('is_virtual 严格取 === true：非布尔值按 false 处理', () => {
    const store = useSysmonStore()
    const base = {
      interface: 'nic',
      rx_bytes_total: 0,
      tx_bytes_total: 0,
      rx_packets_total: 0,
      tx_packets_total: 0,
      rx_errors_total: 0,
      tx_errors_total: 0,
      rx_drops_total: 0,
      tx_drops_total: 0,
      rx_bytes_per_sec: null,
      tx_bytes_per_sec: null,
      rx_errors_per_sec: null,
      tx_errors_per_sec: null,
      rx_drops_per_sec: null,
      tx_drops_per_sec: null,
    }
    store.applySnapshot(makeSysmonSnapshot({
      network: [
        { ...base, interface: 'a', is_virtual: true },
        { ...base, interface: 'b', is_virtual: 'yes' },
        { ...base, interface: 'c', is_virtual: 1 },
        { ...base, interface: 'd' },
      ] as never,
    }))

    expect(store.network.map(nic => [nic.interface, nic.is_virtual])).toEqual([
      ['a', true],
      ['b', false],
      ['c', false],
      ['d', false],
    ])
  })
})

describe('useSysmonStore sysmon_config 镜像与「显示全部网卡」pending（#77）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('applyConfig 写生效配置镜像并读 hide_virtual（默认 true）', () => {
    const store = useSysmonStore()
    // 未收到配置帧：按契约默认值 true（不是 false/未知）
    expect(store.config).toBeNull()
    expect(store.hideVirtual).toBe(true)

    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    expect(store.config?.network.hide_virtual).toBe(false)
    expect(store.hideVirtual).toBe(false)

    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))
    expect(store.hideVirtual).toBe(true)
  })

  it('applyConfig 忽略非法 payload（不写镜像、不清 pending）', () => {
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY] = true

    const invalid: unknown[] = [
      null,
      undefined,
      'bad',
      {},
      // network 段缺失 / hide_virtual 非布尔
      { ...makeSysmonConfig(), network: undefined },
      { ...makeSysmonConfig(), network: { hide_virtual: 'yes' } },
      { ...makeSysmonConfig(), network: {} },
      // 其余段缺失（半截配置不写镜像，避免后续消费方读到假值）
      { ...makeSysmonConfig(), sampling: undefined },
      { ...makeSysmonConfig(), disk: undefined },
      { ...makeSysmonConfig(), history: undefined },
      { ...makeSysmonConfig(), realtime: undefined },
      { ...makeSysmonConfig(), alerts: undefined },
    ]
    for (const payload of invalid) store.applyConfig(payload)

    expect(store.config?.network.hide_virtual).toBe(false)
    expect(sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY]).toBe(true)
  })

  it('setHideVirtual 下发 merge patch，成功保持 pending 直到 WS sysmon_config 到达', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true, message: 'dispatched' })
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: true } }))

    const result = await store.setHideVirtual(false)

    expect(result).toBe(true)
    // 只发 network 段：RFC 7386 merge patch（契约 rest §2.8）
    expect(sysmonApi.setConfig).toHaveBeenCalledWith({ network: { hide_virtual: false } })
    // REST 成功只表示"已下发"：pending 仍挂起、镜像值未变（生效信号是 WS sysmon_config）
    expect(store.hideVirtualPending).toBe(true)
    expect(store.hideVirtual).toBe(true)

    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))
    expect(store.hideVirtualPending).toBe(false)
    expect(store.hideVirtual).toBe(false)
  })

  it('setHideVirtual 防重入：pending 中再次调用直接 false 且不重复 HTTP', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true })
    const store = useSysmonStore()

    const first = store.setHideVirtual(false)
    const second = await store.setHideVirtual(true)

    expect(second).toBe(false)
    await expect(first).resolves.toBe(true)
    expect(sysmonApi.setConfig).toHaveBeenCalledTimes(1)
  })

  it('setHideVirtual HTTP 失败：返回 false 且立即清 pending（不等 WS）', async () => {
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 403'))
    const store = useSysmonStore()

    const result = await store.setHideVirtual(true)

    expect(result).toBe(false)
    expect(store.hideVirtualPending).toBe(false)
  })

  it('setHideVirtual 超时：返回 PENDING_TIMEOUT，超时路径已清 pending', async () => {
    vi.useFakeTimers()
    try {
      vi.mocked(sysmonApi.setConfig).mockImplementation(() => new Promise(() => {}))
      const store = useSysmonStore()

      const pendingResult = store.setHideVirtual(false)
      expect(store.hideVirtualPending).toBe(true)
      await vi.advanceTimersByTimeAsync(SYSMON_CONFIG_TIMEOUT_MS + 1)
      const result = await pendingResult

      expect(result).toBe(PENDING_TIMEOUT)
      expect(store.hideVirtualPending).toBe(false)
    } finally {
      vi.useRealTimers()
    }
  })
})

describe('useSysmonStore activeAlerts（#80，快照 alerts[] 持续状态）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无快照 / 快照 alerts 为空：活动告警为空数组（不是"未采集"，也不伪造）', () => {
    const store = useSysmonStore()
    expect(store.activeAlerts).toEqual([])
    store.applySnapshot(makeSysmonSnapshot())
    expect(store.activeAlerts).toEqual([])
  })

  it('按 kind、卷挂载点稳定排序并保留 since（不依赖发送端顺序）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      alerts: [
        { kind: 'volume_free', subject: '/home', since: 1_790_000_003 },
        { kind: 'swap_used', subject: '', since: 1_790_000_002 },
        { kind: 'volume_free', subject: '/data', since: 1_790_000_004 },
        { kind: 'memory_used', subject: '', since: 1_790_000_001 },
      ],
    }))

    expect(store.activeAlerts.map(alert => `${alert.kind}|${alert.subject}`)).toEqual([
      'memory_used|',
      'swap_used|',
      'volume_free|/data',
      'volume_free|/home',
    ])
    expect(store.activeAlerts[0].since).toBe(1_790_000_001)
  })

  it('净化：未知 kind / volume_free 空挂载点 / 重复项剔除；非 volume_free 的 subject 强制空串；since 非法置 0', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      alerts: [
        { kind: 'memory_used', subject: '/ignored', since: 1 },
        { kind: 'volume_free', subject: '  ', since: 2 },
        { kind: 'volume_free', subject: '/data', since: -1 },
        { kind: 'volume_free', subject: '/data', since: 9 },
        { kind: 'disk_io_hot', subject: '', since: 3 },
        { kind: 'swap_used', subject: '', since: Number.NaN },
      ] as never,
    }))

    expect(store.activeAlerts).toEqual([
      { kind: 'memory_used', subject: '', since: 1 },
      { kind: 'swap_used', subject: '', since: 0 },
      { kind: 'volume_free', subject: '/data', since: 0 },
    ])
  })

  it('后续快照覆盖：恢复后活动告警清空（持续状态随快照更新）', () => {
    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({
      alerts: [{ kind: 'memory_used', subject: '', since: 100 }],
    }))
    expect(store.activeAlerts).toHaveLength(1)

    store.applySnapshot(makeSysmonSnapshot())
    expect(store.activeAlerts).toEqual([])
  })
})

describe('useSysmonStore 完整配置镜像与保存（#81）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('loadConfig 成功：写生效配置镜像（不等 WS 推送）', async () => {
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(
      makeSysmonConfig({ sampling: { measure_interval_ms: 500 } }),
    )
    const store = useSysmonStore()
    expect(store.config).toBeNull()

    await store.loadConfig()

    expect(sysmonApi.getConfig).toHaveBeenCalledTimes(1)
    // 配置镜像按契约全量形状写入（各段都能被配置 tab 消费）
    expect(store.config?.sampling.measure_interval_ms).toBe(500)
    expect(store.config?.sampling.record_interval_sec).toBe(60)
    expect(store.config?.alerts.cooldown_sec).toBe(600)
  })

  it('loadConfig 503：保持空镜像、不抛错、不弹错（页面显示「配置尚未到达」）', async () => {
    vi.mocked(sysmonApi.getConfig).mockRejectedValue(new Error('HTTP 503'))
    const toastSpy = vi.spyOn(useToastStore(), 'error')
    const store = useSysmonStore()

    await expect(store.loadConfig()).resolves.toBeUndefined()

    expect(store.config).toBeNull()
    // 镜像未就绪是正常启动态，不是错误弹窗场景（错误提示由页面空态承担）
    expect(toastSpy).not.toHaveBeenCalled()
  })

  it('loadConfig 忽略形状非法的响应（半截配置不得写进镜像）', async () => {
    vi.mocked(sysmonApi.getConfig).mockResolvedValue({ network: { hide_virtual: true } } as never)
    const store = useSysmonStore()

    await store.loadConfig()

    expect(store.config).toBeNull()
  })

  it('loadConfig 与 WS 并发：WS 先到，较旧的 REST 响应不得覆盖生效镜像', async () => {
    // 生效信号是 WS `sysmon_config`；REST 只是首次补拉。若 GET 在等待期间 WS 已到达，
    // 迟到的 REST 响应（更旧）必须丢弃，否则配置展示会短暂回退到旧生效值。
    let resolveGet: (value: ReturnType<typeof makeSysmonConfig>) => void = () => {}
    vi.mocked(sysmonApi.getConfig).mockReturnValue(
      new Promise(resolve => {
        resolveGet = resolve
      }),
    )
    const store = useSysmonStore()
    const loading = store.loadConfig()

    store.applyConfig(makeSysmonConfig({ realtime: { push_interval_sec: 30 } }))
    resolveGet(makeSysmonConfig({ realtime: { push_interval_sec: 5 } }))
    await loading

    expect(store.config?.realtime.push_interval_sec).toBe(30)
  })

  it('init 在配置镜像缺失时经 REST 补拉一次；已有镜像后不再补拉', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig())
    const store = useSysmonStore()

    await store.init()
    expect(sysmonApi.getConfig).toHaveBeenCalledTimes(1)
    expect(store.config?.realtime.push_interval_sec).toBe(5)

    // 镜像已在：再次挂载（顶栏 / 监控页都会 init）不重复请求
    await store.init()
    expect(sysmonApi.getConfig).toHaveBeenCalledTimes(1)
  })

  it('setConfig 下发 merge patch，成功保持 pending 直到 WS sysmon_config 到达', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true, message: 'dispatched' })
    const store = useSysmonStore()
    store.applyConfig(makeSysmonConfig())
    const patch = { sampling: { measure_interval_ms: 500 }, history: { enabled: false } }

    const result = await store.setConfig(patch)

    expect(result).toBe(true)
    // body 原样透传为 SET_SYSMON_CONFIG payload（契约 rest §2.8），前端不做额外包装
    expect(sysmonApi.setConfig).toHaveBeenCalledWith(patch)
    // REST 成功只表示"已下发"：pending 保持、镜像值未变（生效信号是 WS sysmon_config）
    expect(store.configPending).toBe(true)
    expect(store.config?.sampling.measure_interval_ms).toBe(1000)

    // WS 全量回推：镜像更新 + pending 清除
    store.applyConfig(makeSysmonConfig({ sampling: { measure_interval_ms: 500 }, history: { enabled: false } }))
    expect(store.configPending).toBe(false)
    expect(store.config?.sampling.measure_interval_ms).toBe(500)
    expect(store.config?.history.enabled).toBe(false)
  })

  it('setConfig HTTP 失败：返回 false 且立即清 pending（不等 WS）', async () => {
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 403'))
    const store = useSysmonStore()

    const result = await store.setConfig({ realtime: { push_interval_sec: 10 } })

    expect(result).toBe(false)
    expect(store.configPending).toBe(false)
  })

  it('setConfig 防重入：pending 中再次调用直接 false 且不重复 HTTP', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true })
    const store = useSysmonStore()

    const first = store.setConfig({ realtime: { push_interval_sec: 10 } })
    const second = await store.setConfig({ realtime: { push_interval_sec: 20 } })

    expect(second).toBe(false)
    await expect(first).resolves.toBe(true)
    expect(sysmonApi.setConfig).toHaveBeenCalledTimes(1)
  })

  it('setConfig 超时：返回 PENDING_TIMEOUT，超时路径已清 pending', async () => {
    vi.useFakeTimers()
    try {
      vi.mocked(sysmonApi.setConfig).mockImplementation(() => new Promise(() => {}))
      const store = useSysmonStore()

      const pendingResult = store.setConfig({ alerts: { cooldown_sec: 60 } })
      expect(store.configPending).toBe(true)
      await vi.advanceTimersByTimeAsync(SYSMON_CONFIG_TIMEOUT_MS + 1)
      const result = await pendingResult

      expect(result).toBe(PENDING_TIMEOUT)
      expect(store.configPending).toBe(false)
    } finally {
      vi.useRealTimers()
    }
  })

  it('applyConfig 同时清「完整配置保存」与「显示全部网卡」两个 pending', () => {
    const store = useSysmonStore()
    sysmonPending.pending[SYSMON_CONFIG_KEY] = true
    sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY] = true

    store.applyConfig(makeSysmonConfig({ network: { hide_virtual: false } }))

    expect(sysmonPending.pending[SYSMON_CONFIG_KEY]).toBe(false)
    expect(sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY]).toBe(false)
    expect(store.config?.network.hide_virtual).toBe(false)
  })
})
