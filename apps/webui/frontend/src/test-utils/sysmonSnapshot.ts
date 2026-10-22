// 测试夹具：契约 sysmon 快照（SysmonSnapshot）。
// 默认形状 = #74 落地后的后端：system / cpu.total / cpu.cores / cpu.load_average /
// cpu.context_switches / memory / swap 可用（每核 16 个、Linux 真实负载、速率有值、
// 内存 32 GiB + 缓存/缓冲、交换 8 GiB），其余指标不可用（温度按 Linux 无传感器理由
// no_sensor、每核频率按无 cpufreq 理由 unsupported，其余 not_implemented）；
// resourceData=true 时额外填充卷 / 网卡数据并标记可用，用于「全部可用」渲染用例；
// sensorData=true 时额外填充温度 / 每核频率并标记可用。
import type {
  SysmonAvailabilityEntry,
  SysmonCoreFrequency,
  SysmonCpuCore,
  SysmonLoadAverage,
  SysmonMemoryInfo,
  SysmonSnapshotPayload,
  SysmonSwapInfo,
  SysmonTemperatureInfo,
} from '@/types/generated'

export const SYSMON_METRIC_KEYS = [
  'system',
  'cpu.total',
  'cpu.cores',
  'cpu.load_average',
  'cpu.context_switches',
  'cpu.frequency',
  'memory',
  'swap',
  'volumes',
  'disk_io',
  'network',
  'shm_dir',
  'temperature',
] as const

export interface SysmonSnapshotOptions {
  /** 指标可用性覆盖（合并到默认映射；默认 system / cpu.* / memory / swap 可用） */
  metrics?: Record<string, SysmonAvailabilityEntry>
  /** CPU 总使用率；null = 首次采样/不可用（不得用 0 冒充） */
  cpuUsageRatio?: number | null
  /** 填充卷 / 网卡数据，并把对应指标标记为可用（模拟后续指标票落地后的快照） */
  resourceData?: boolean
  /** 内存对象；默认 #74 形状（32 GiB 总量 / 16 GiB 可用 / 0.5 比率 / 8 GiB 缓存 / 256 MiB 缓冲），null = 本帧未采集 */
  memory?: SysmonMemoryInfo | null
  /** 交换对象；默认 {total: 8 GiB, used: 2 GiB, ratio: 0.25}，null = 本帧未采集，total_bytes=0 = 无交换分区 */
  swap?: SysmonSwapInfo | null
  /** 填充温度 / 每核频率数据，并把 temperature / cpu.frequency 标记为可用（模拟 #73 落地后的快照） */
  sensorData?: boolean
  /** 温度传感器读数；默认 null；sensorData=true 时默认两个 Linux hwmon 读数 */
  temperatures?: SysmonTemperatureInfo[] | null
  /** 每核当前频率；默认 null；sensorData=true 时默认按核数填充 3600 + index*100 MHz */
  coreFrequencies?: SysmonCoreFrequency[] | null
  /** 每核使用率；默认 16 个核（index 0..15，usage_ratio 各不相同且非 null，saturated=false），system.logical_cores 同步为数组长度 */
  cpuCores?: SysmonCpuCore[]
  /** 持续饱和的核 index 集合；默认 []。会把对应 core.saturated 置 true 并写入 cpu.saturated_cores */
  saturatedCores?: number[]
  /** 系统负载；默认 Linux 真实负载 {one:0.42,five:0.51,fifteen:0.63,semantic:'load_average'}；null = 快照不提供 */
  cpuLoadAverage?: SysmonLoadAverage | null
  /** 每秒上下文切换；默认 1234，null = 首次采样/不可用 */
  contextSwitchesPerSec?: number | null
  /** 每秒中断；默认 567，null = 首次采样/不可用 */
  interruptsPerSec?: number | null
  /** 跨平台语义提示（availability.semantic_hints）；默认空对象 */
  semanticHints?: Record<string, string>
}

const GIB = 1024 ** 3
const MIB = 1024 ** 2

/** 默认温度读数（模拟 Linux hwmon：<name>#<hwmonN>:<label>）；sensorData=true 时使用 */
const DEFAULT_TEMPERATURES: SysmonTemperatureInfo[] = [
  { sensor: 'coretemp#hwmon3:Package id 0', celsius: 45.5 },
  { sensor: 'coretemp#hwmon3:Core 0', celsius: 41.5 },
]

/** 默认内存（#74 后端形状）：32 GiB 总量 / 16 GiB 可用（used 按唯一口径 = total − available）/ 8 GiB 缓存 / 256 MiB 缓冲 */
const DEFAULT_MEMORY: SysmonMemoryInfo = {
  total_bytes: 32 * GIB,
  available_bytes: 16 * GIB,
  used_bytes: 16 * GIB,
  used_ratio: 0.5,
  cached_bytes: 8 * GIB,
  buffers_bytes: 256 * MIB,
}

/** 默认交换：8 GiB 总量 / 2 GiB 已用（比率 0.25） */
const DEFAULT_SWAP: SysmonSwapInfo = {
  total_bytes: 8 * GIB,
  used_bytes: 2 * GIB,
  used_ratio: 0.25,
}

/**
 * 默认每核数据：usage_ratio 各不相同且非 null（首次采样之外均有真实值），均未饱和。
 * 比例刻意避开整十百分比，避免「0.0%」之类字符串断言歧义。
 */
export function makeSysmonCpuCores(count = 16): SysmonCpuCore[] {
  return Array.from({ length: count }, (_, index) => ({
    index,
    usage_ratio: 0.05 + index * 0.031,
    saturated: false,
  }))
}

export function makeSysmonSnapshot(options: SysmonSnapshotOptions = {}): SysmonSnapshotPayload {
  const resourceAvailable = options.resourceData === true
  const sensorAvailable = options.sensorData === true
  const metrics: Record<string, SysmonAvailabilityEntry> = {}
  for (const key of SYSMON_METRIC_KEYS) {
    const available = key === 'system'
      || key === 'cpu.total'
      || key === 'cpu.cores'
      || key === 'cpu.load_average'
      || key === 'cpu.context_switches'
      || key === 'memory'
      || key === 'swap'
      || (resourceAvailable && (key === 'volumes' || key === 'network'))
      || (sensorAvailable && (key === 'temperature' || key === 'cpu.frequency'))
    const reason = key === 'temperature'
      ? 'no_sensor'
      : (key === 'cpu.frequency' ? 'unsupported' : 'not_implemented')
    metrics[key] = available
      ? { available: true }
      : { available: false, reason }
  }
  for (const [key, entry] of Object.entries(options.metrics ?? {})) {
    metrics[key] = { ...entry }
  }

  const saturatedCores = [...(options.saturatedCores ?? [])]
  const cores = (options.cpuCores ?? makeSysmonCpuCores()).map(core => ({
    ...core,
    saturated: core.saturated || saturatedCores.includes(core.index),
  }))

  return {
    source: 'dzsysmon',
    timestamp: Date.UTC(2026, 9, 21, 15, 0, 0),
    sampling: {
      measure_interval_ms: 1000,
      last_sample_unix_ms: Date.UTC(2026, 9, 21, 15, 0, 0) - 877,
      missed_ticks: 0,
    },
    platform: {
      family: 'linux',
      version: 'Ubuntu 24.04.1 LTS',
      kernel: '6.8.0-45-generic',
      arch: 'x86_64',
      build: 0,
    },
    system: {
      hostname: 'trade-01',
      cpu_model: 'AMD Ryzen 9 7950X 16-Core Processor',
      logical_cores: cores.length,
      boot_time_unix_sec: Date.UTC(2026, 9, 20, 12, 0, 0) / 1000,
      uptime_sec: 3661,
    },
    cpu: {
      total: {
        usage_ratio: options.cpuUsageRatio === undefined ? 0.187 : options.cpuUsageRatio,
      },
      cores,
      core_frequencies: options.coreFrequencies !== undefined
        ? options.coreFrequencies
        : (sensorAvailable
            ? cores.map(core => ({ index: core.index, mhz: 3600 + core.index * 100 }))
            : null),
      saturated_cores: saturatedCores,
      load_average: options.cpuLoadAverage === undefined
        ? { one: 0.42, five: 0.51, fifteen: 0.63, semantic: 'load_average' }
        : options.cpuLoadAverage,
      context_switches_per_sec: options.contextSwitchesPerSec === undefined
        ? 1234
        : options.contextSwitchesPerSec,
      interrupts_per_sec: options.interruptsPerSec === undefined ? 567 : options.interruptsPerSec,
    },
    temperature: options.temperatures !== undefined
      ? options.temperatures
      : (sensorAvailable ? DEFAULT_TEMPERATURES.map(reading => ({ ...reading })) : null),
    memory: options.memory !== undefined
      ? options.memory
      : { ...DEFAULT_MEMORY },
    swap: options.swap !== undefined
      ? options.swap
      : { ...DEFAULT_SWAP },
    volumes: resourceAvailable
      ? [{ mount: '/data', total_bytes: 1024 * GIB, used_bytes: 256 * GIB, used_ratio: 0.25 }]
      : null,
    disk_io: null,
    network: resourceAvailable
      ? [{
          interface: 'eth0',
          rx_bytes_total: 8 * GIB,
          tx_bytes_total: 4 * GIB,
          rx_bytes_per_sec: 1 * MIB,
          tx_bytes_per_sec: 2 * MIB,
          rx_errors_per_sec: 0,
          tx_errors_per_sec: 0,
          rx_drops_per_sec: 0,
          tx_drops_per_sec: 0,
          is_virtual: false,
        }]
      : null,
    shm_dir: null,
    availability: {
      platform_description: 'Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)',
      metrics,
      semantic_hints: { ...(options.semanticHints ?? {}) },
    },
  }
}
