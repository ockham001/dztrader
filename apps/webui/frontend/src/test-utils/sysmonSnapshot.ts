// 测试夹具：契约 sysmon 快照（SysmonSnapshot）。
// 默认形状 = #77 落地后的后端：system / cpu.total / cpu.cores / cpu.load_average /
// cpu.context_switches / memory / swap / volumes / disk_io / shm_dir 可用（每核 16 个、Linux 真实负载、
// 速率有值、内存 32 GiB + 缓存/缓冲、交换 8 GiB、三个卷、
// 磁盘 I/O 两块盘、共享内存目录占用有值），其余指标不可用（温度按 Linux 无传感器理由 no_sensor、
// 每核频率按无 cpufreq 理由 unsupported，network 默认不可用 —— 网卡由 network 选项显式给）。
// resourceData=true 时额外填充网卡数据并标记 network 可用（volumes / disk_io / shm_dir 默认已可用），
// 用于「全部可用」渲染用例；sensorData=true 时额外填充温度 / 每核频率并标记可用。
// network 选项可显式给网卡数组（含虚拟网卡 / 空数组 = 全部被 hide_virtual 滤掉 / null = 本帧未采集）；
// virtualNetwork=true 是 3 块虚拟网卡 + eth0 的混合集合（验证前端不自行过滤、只做展示）。
// semantic_hints 默认携带 #76 后端在 disk_io 能力可用时发送的口径提示；用例可按需覆盖。
import type {
  SysmonAvailabilityEntry,
  SysmonConfigPayload,
  SysmonCoreFrequency,
  SysmonCpuCore,
  SysmonDiskIoInfo,
  SysmonLoadAverage,
  SysmonMemoryInfo,
  SysmonNetworkInfo,
  SysmonShmDirInfo,
  SysmonSnapshotPayload,
  SysmonSwapInfo,
  SysmonTemperatureInfo,
  SysmonVolumeInfo,
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
  /** 填充网卡数据并标记 network 可用（#75 起 volumes / shm_dir 默认已可用） */
  resourceData?: boolean
  /** 内存对象；默认 #74 形状（32 GiB 总量 / 16 GiB 可用 / 0.5 比率 / 8 GiB 缓存 / 256 MiB 缓冲），null = 本帧未采集 */
  memory?: SysmonMemoryInfo | null
  /** 交换对象；默认 {total: 8 GiB, used: 2 GiB, ratio: 0.25}，null = 本帧未采集，total_bytes=0 = 无交换分区 */
  swap?: SysmonSwapInfo | null
  /** 卷数组；默认 3 个卷（`/` 40% / `/data` 25% / `/home` 62%），null = 本帧未采集（能力粘性） */
  volumes?: SysmonVolumeInfo[] | null
  /** 共享内存目录占用；默认 {path: /srv/dztrader/shm, 720 MiB, 12 页文件, 最旧 480s}，null = 本帧未采集 */
  shmDir?: SysmonShmDirInfo | null
  /** 磁盘 I/O；默认两块盘（#76 后端形状），null = 本帧未采集（能力粘性） */
  diskIo?: SysmonDiskIoInfo[] | null
  /**
   * 网卡数组（契约 sysmon §2.4 network[]）；默认 null（network 不可用）。
   * 显式给值同时把 `network` 标记为可用，便于覆盖：含虚拟网卡的混合集合、
   * 空数组（= hide_virtual 滤掉全部虚拟网卡，契约 §2.5：available 仍为 true）、
   * null（能力可用但本帧未采集）。
   */
  network?: SysmonNetworkInfo[] | null
  /**
   * 额外给一份「物理 + 虚拟」混合网卡集合并把 network 标记为可用（eth0 + docker0 + veth1234 + lo）。
   * 与 `network` 同时给时以 `network` 为准。
   */
  virtualNetwork?: boolean
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
  /** 跨平台语义提示（availability.semantic_hints）；默认携带 disk_io 口径提示 */
  semanticHints?: Record<string, string>
}

const GIB = 1024 ** 3
const MIB = 1024 ** 2

/**
 * 默认温度读数（模拟 Linux hwmon：<name>#<hwmonN>:<label>）；sensorData=true 时使用。
 * #126 起后端除 `sensor` 外还发 `chip` 与 `label`（additive 字段），夹具按真实形状给出。
 */
const DEFAULT_TEMPERATURES: SysmonTemperatureInfo[] = [
  { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
  { sensor: 'coretemp#hwmon3:Core 0', chip: 'coretemp', label: 'Core 0', celsius: 41.5 },
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
 * 默认卷（#75 后端形状）：/ 40%、/data 25%（承载 shm 与 db 目录）、/home 62%。
 * 顺序刻意与使用率不同（payload 顺序 /、/data、/home），用于验证前端按使用率降序重排
 * 而不是依赖发送端顺序（#122）。
 */
const DEFAULT_VOLUMES: SysmonVolumeInfo[] = [
  { mount: '/', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
  { mount: '/data', available: true, total_bytes: 1024 * GIB, used_bytes: 256 * GIB, used_ratio: 0.25 },
  { mount: '/home', available: true, total_bytes: 50 * GIB, used_bytes: 31 * GIB, used_ratio: 0.62 },
]

/** 默认共享内存目录占用（#75）：720 MiB / 12 个页文件 / 最旧页文件 480s */
const DEFAULT_SHM_DIR: SysmonShmDirInfo = {
  path: '/srv/dztrader/shm',
  total_bytes: 720 * MIB,
  page_files: 12,
  oldest_file_age_sec: 480,
}

/** #76 后端在 disk_io 能力可用时发送的口径提示（契约 sysmon §2.5） */
const DEFAULT_DISK_IO_HINT =
  'latency is the average time per completed request, queue wait included; queue depth is the '
  + 'instantaneous in-flight request count at the measurement tick, not an interval average'

/**
 * 默认磁盘 I/O（#76 后端形状）：两块盘，吞吐 / IOPS / 延迟 / 队列深度 / 利用率均有值。
 * 刻意按 sda、nvme0n1 的发送顺序排列，用于验证前端按 device 升序排序而不是依赖发送端顺序。
 */
const DEFAULT_DISK_IO: SysmonDiskIoInfo[] = [
  {
    device: 'sda',
    read_bytes_per_sec: 4 * MIB,
    write_bytes_per_sec: 2 * MIB,
    read_iops: 128,
    write_iops: 64,
    latency_ms: 1.25,
    queue_depth: 2.5,
    utilization: 0.35,
  },
  {
    device: 'nvme0n1',
    read_bytes_per_sec: 64 * MIB,
    write_bytes_per_sec: 32 * MIB,
    read_iops: 2048,
    write_iops: 1024,
    latency_ms: 0.45,
    queue_depth: 1.25,
    utilization: 0.72,
  },
]

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

/**
 * 默认网卡（#77 契约 sysmon §2.4 network[]）：单块物理网卡 eth0，八个累计计数器与六个速率
 * 均有真实值（速率非 null；错误/丢弃速率 0 = 本窗口确实没有增量，是合法真实值）。
 */
const DEFAULT_NETWORK: SysmonNetworkInfo[] = [
  {
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
  },
]

/**
 * 虚拟网卡（is_virtual=true）：docker0 桥接 + veth 容器对端 + lo 回环（契约 §2.4 明文把回环、
 * 桥接、隧道、聚合与 VLAN 逻辑接口都算虚拟）。刻意放在 eth0 之前，用于验证前端按 interface
 * 升序排序而不是依赖发送端顺序。
 */
const VIRTUAL_NETWORK: SysmonNetworkInfo[] = [
  {
    interface: 'veth1234',
    rx_bytes_total: 512 * MIB,
    tx_bytes_total: 256 * MIB,
    rx_packets_total: 400_000,
    tx_packets_total: 300_000,
    rx_errors_total: 0,
    tx_errors_total: 0,
    rx_drops_total: 5,
    tx_drops_total: 1,
    rx_bytes_per_sec: 64 * 1024,
    tx_bytes_per_sec: 32 * 1024,
    rx_errors_per_sec: 0,
    tx_errors_per_sec: 0,
    rx_drops_per_sec: 0,
    tx_drops_per_sec: 0,
    is_virtual: true,
  },
  {
    interface: 'docker0',
    rx_bytes_total: 1024 * MIB,
    tx_bytes_total: 2048 * MIB,
    rx_packets_total: 800_000,
    tx_packets_total: 900_000,
    rx_errors_total: 0,
    tx_errors_total: 0,
    rx_drops_total: 0,
    tx_drops_total: 0,
    rx_bytes_per_sec: 128 * 1024,
    tx_bytes_per_sec: 256 * 1024,
    rx_errors_per_sec: 0,
    tx_errors_per_sec: 0,
    rx_drops_per_sec: 0,
    tx_drops_per_sec: 0,
    is_virtual: true,
  },
  {
    interface: 'lo',
    rx_bytes_total: 64 * MIB,
    tx_bytes_total: 64 * MIB,
    rx_packets_total: 120_000,
    tx_packets_total: 120_000,
    rx_errors_total: 0,
    tx_errors_total: 0,
    rx_drops_total: 0,
    tx_drops_total: 0,
    rx_bytes_per_sec: 0,
    tx_bytes_per_sec: 0,
    rx_errors_per_sec: 0,
    tx_errors_per_sec: 0,
    rx_drops_per_sec: 0,
    tx_drops_per_sec: 0,
    is_virtual: true,
  },
]

/**
 * 默认系统监控配置（契约 sysmon §3.1 全量生效配置；network.hide_virtual 默认 true）。
 * overrides 按段浅合并：只覆盖给定字段，其余保持契约默认值。
 */
export type SysmonConfigOverrides = {
  [K in keyof SysmonConfigPayload]?: Partial<SysmonConfigPayload[K]>
}

export function makeSysmonConfig(overrides: SysmonConfigOverrides = {}): SysmonConfigPayload {
  return {
    sampling: {
      measure_interval_ms: 1000,
      record_interval_sec: 60,
      phase_offset_sec: 3,
      groups: {
        cpu: true,
        memory: true,
        swap: true,
        volumes: true,
        disk_io: true,
        network: true,
        temperature: true,
        cpu_frequency: true,
      },
      ...overrides.sampling,
    },
    disk: { devices: [], ...overrides.disk },
    network: { hide_virtual: true, ...overrides.network },
    history: {
      enabled: true,
      raw_retention_hours: 24,
      max_retention_days: 30,
      max_bytes: 2_147_483_648,
      ...overrides.history,
    },
    realtime: { push_interval_sec: 5, ...overrides.realtime },
    thresholds: {
      cpu_sustain_used_pct: 90,
      cpu_sustain_samples: 10,
      memory_used_pct: 90,
      swap_used_pct: 50,
      volume_free_pct: 10,
      log_repeat_sec: 600,
      ...overrides.thresholds,
    },
  }
}

export function makeSysmonSnapshot(options: SysmonSnapshotOptions = {}): SysmonSnapshotPayload {
  const resourceAvailable = options.resourceData === true
  const sensorAvailable = options.sensorData === true
  // network 能力可用 = 任一网卡选项显式给值（含空数组 / null：能力可用但本帧未采集 / 全被滤掉）
  const networkAvailable = resourceAvailable
    || options.network !== undefined
    || options.virtualNetwork === true
  const metrics: Record<string, SysmonAvailabilityEntry> = {}
  for (const key of SYSMON_METRIC_KEYS) {
    const available = key === 'system'
      || key === 'cpu.total'
      || key === 'cpu.cores'
      || key === 'cpu.load_average'
      || key === 'cpu.context_switches'
      || key === 'memory'
      || key === 'swap'
      || key === 'volumes'
      || key === 'disk_io'
      || key === 'shm_dir'
      || (networkAvailable && key === 'network')
      || (sensorAvailable && (key === 'temperature' || key === 'cpu.frequency'))
    // network 已由 #77 实现：默认不可用的现实原因是配置组关闭（disabled_by_config），
    // 不再是 not_implemented；温度/频率按各自的 B 组语义给 reason。
    const reason = key === 'temperature'
      ? 'no_sensor'
      : (key === 'cpu.frequency' ? 'unsupported' : (key === 'network' ? 'disabled_by_config' : 'not_implemented'))
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
    volumes: options.volumes !== undefined
      ? options.volumes
      : DEFAULT_VOLUMES.map(volume => ({ ...volume })),
    disk_io: options.diskIo !== undefined
      ? options.diskIo
      : DEFAULT_DISK_IO.map(entry => ({ ...entry })),
    network: options.network !== undefined
      ? options.network
      : (options.virtualNetwork === true
          ? [...VIRTUAL_NETWORK, ...DEFAULT_NETWORK].map(entry => ({ ...entry }))
          : (resourceAvailable ? DEFAULT_NETWORK.map(entry => ({ ...entry })) : null)),
    shm_dir: options.shmDir !== undefined ? options.shmDir : { ...DEFAULT_SHM_DIR },
    availability: {
      platform_description: 'Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)',
      metrics,
      // 后端只在 disk_io 能力可用时携带该口径提示（契约 sysmon §2.5），夹具保持一致。
      semantic_hints: {
        ...(options.semanticHints
          ?? (metrics.disk_io?.available === true ? { disk_io: DEFAULT_DISK_IO_HINT } : {})),
      },
    },
  }
}
