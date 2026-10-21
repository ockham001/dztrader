// 测试夹具：契约 sysmon 快照（SysmonSnapshot）。
// 默认形状 = #72 落地后的后端：system / cpu.total / cpu.cores / cpu.load_average /
// cpu.context_switches 可用（每核 16 个、Linux 真实负载、速率有值），其余指标不可用；
// resourceData=true 时额外填充内存 / 卷 / 网卡数据并标记可用，用于「全部可用」渲染用例。
import type {
  SysmonAvailabilityEntry,
  SysmonCpuCore,
  SysmonLoadAverage,
  SysmonSnapshotPayload,
} from '@/types/generated'

export const SYSMON_METRIC_KEYS = [
  'system',
  'cpu.total',
  'cpu.cores',
  'cpu.load_average',
  'cpu.context_switches',
  'memory',
  'swap',
  'volumes',
  'disk_io',
  'network',
  'shm_dir',
] as const

export interface SysmonSnapshotOptions {
  /** 指标可用性覆盖（合并到默认映射；默认 system / cpu.total / cpu.cores / cpu.load_average / cpu.context_switches 可用） */
  metrics?: Record<string, SysmonAvailabilityEntry>
  /** CPU 总使用率；null = 首次采样/不可用（不得用 0 冒充） */
  cpuUsageRatio?: number | null
  /** 填充内存 / 卷 / 网卡数据，并把对应指标标记为可用（模拟后续指标票落地后的快照） */
  resourceData?: boolean
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
  const metrics: Record<string, SysmonAvailabilityEntry> = {}
  for (const key of SYSMON_METRIC_KEYS) {
    const available = key === 'system'
      || key === 'cpu.total'
      || key === 'cpu.cores'
      || key === 'cpu.load_average'
      || key === 'cpu.context_switches'
      || (resourceAvailable && (key === 'memory' || key === 'volumes' || key === 'network'))
    metrics[key] = available
      ? { available: true }
      : { available: false, reason: 'not_implemented' }
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
      saturated_cores: saturatedCores,
      load_average: options.cpuLoadAverage === undefined
        ? { one: 0.42, five: 0.51, fifteen: 0.63, semantic: 'load_average' }
        : options.cpuLoadAverage,
      context_switches_per_sec: options.contextSwitchesPerSec === undefined
        ? 1234
        : options.contextSwitchesPerSec,
      interrupts_per_sec: options.interruptsPerSec === undefined ? 567 : options.interruptsPerSec,
    },
    memory: resourceAvailable
      ? {
          total_bytes: 32 * GIB,
          available_bytes: 16 * GIB,
          used_bytes: 16 * GIB,
          used_ratio: 0.5,
        }
      : null,
    swap: null,
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
