import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { sysmonApi, type SysmonConfigPatch } from '@/api/sysmon'
import { PENDING_TIMEOUT, usePending } from '@/composables/usePending'
import type {
  SysmonAlertInfo,
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
  SysmonWatchPathInfo,
} from '@/types/generated'

/** 网络配置（network.hide_virtual）切换的 pending key；WS `sysmon_config` 到达时清除 */
export const SYSMON_NETWORK_CONFIG_KEY = 'sysmon:network_config'
/** 完整配置保存（admin 合并 patch）的 pending key；WS `sysmon_config` 到达时清除 */
export const SYSMON_CONFIG_KEY = 'sysmon:config'
/** 配置类操作超时（对齐 settings / mdConfig 的 CONFIG_OP_TIMEOUT_MS） */
export const SYSMON_CONFIG_TIMEOUT_MS = 10_000

/**
 * 系统监控领域独立 pending 实例（P3 范式：本领域无跨 store 交互，独立空间）。
 * 模块级单例导出供 store 与 spec 共享同一实例（可重置 / 可断言）。
 */
export const sysmonPending = usePending()

/** 阈值告警 kind（契约 sysmon §2.8）：快照 `alerts[].kind` 的稳定取值。 */
export type SysmonAlertKind = 'memory_used' | 'swap_used' | 'volume_free'

/** 净化后的活动告警条目（快照 `alerts[]`）。 */
export interface SysmonActiveAlert {
  kind: SysmonAlertKind
  /** `volume_free` 的挂载点；其余 kind 为空串 */
  subject: string
  /** 该次告警首次满足阈值的墙钟时刻（Unix 秒）；0 = 缺失/非法（前端不展示"始于"） */
  since: number
}

const SYSMON_ALERT_KINDS: readonly SysmonAlertKind[] = ['memory_used', 'swap_used', 'volume_free']

/**
 * 告警对象的短标签（顶栏摘要 / 监控页横幅共用）：kind → 中文名，卷告警带挂载点。
 * 集中一处，避免"kind → 文案"映射在多个组件里各写一份而漂移。
 */
export function sysmonAlertLabel(alert: Pick<SysmonActiveAlert, 'kind' | 'subject'>): string {
  if (alert.kind === 'memory_used') return '内存'
  if (alert.kind === 'swap_used') return '交换'
  return `关注卷 ${alert.subject}`
}

/** 有限数字校验：null / undefined / NaN / Infinity 一律视为不可用（不得用 0 冒充） */
function toFiniteNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

/** 非负整数字节数校验：null / undefined / NaN / Infinity / 负数 / 非整数一律视为不可用 */
function toNonNegativeInteger(value: unknown): number | null {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 ? value : null
}

/** 非负数值校验：null / undefined / NaN / Infinity / 负数一律视为不可用（不得用 0 冒充） */
function toNonNegativeNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : null
}

/**
 * 全量配置载荷形状校验（契约 sysmon §3.1）：六个段必须齐全且为对象，`network.hide_virtual`
 * 必须是布尔。dzweb 已按 `SysmonConfig::validate` 校验后才会写镜像/广播，前端仍做浅层防御——
 * 半截/畸形配置不得写进镜像，否则配置 tab 会把不存在或残缺的「生效值」展示给用户，
 * 编辑表单也会拿假值做差异比较。
 */
function isFullConfigPayload(payload: Partial<SysmonConfigPayload> | null | undefined): boolean {
  if (!payload || typeof payload !== 'object') return false
  if (!payload.sampling || typeof payload.sampling !== 'object') return false
  if (!payload.disk || typeof payload.disk !== 'object') return false
  if (!payload.network || typeof payload.network !== 'object') return false
  if (typeof payload.network.hide_virtual !== 'boolean') return false
  if (!payload.history || typeof payload.history !== 'object') return false
  if (!payload.realtime || typeof payload.realtime !== 'object') return false
  if (!payload.alerts || typeof payload.alerts !== 'object') return false
  return true
}

/**
 * 系统监控 store：契约 sysmon 快照域（固定实例 dzsysmon）的前端唯一写入点。
 *
 * 数据流：dzsysmon → 事件通道 → dzweb 镜像 → WS `sysmon_snapshot`（增量 / snapshot 快照域）。
 * 首次挂载时经 REST `GET /api/sysmon/snapshot` 补拉一次镜像（503 = dzsysmon 尚未上报）；
 * 之后只由 WS 推送刷新 —— 刷新基线由发送方 `realtime.push_interval_sec`（默认 5s）决定，
 * 前端不轮询、不额外发帧（契约 sysmon §4、契约 rest §2.8）。
 */
export const useSysmonStore = defineStore('sysmon', () => {
  const snapshot = ref<SysmonSnapshotPayload | null>(null)
  const loaded = ref(false)
  const error = ref<string | null>(null)

  /**
   * 首次挂载补拉镜像：快照（GET /api/sysmon/snapshot）与配置（GET /api/sysmon/config）并发。
   * 已加载过的部分跳过（WS 推送负责后续更新）；配置补拉失败不抛错——配置 tab 显示
   * 「配置尚未到达」，后续 WS `sysmon_config`（快照域）到达会自然补上。
   */
  async function init(): Promise<void> {
    // 两路独立补拉并发发出：配置请求不阻塞页面主内容（快照）的首次渲染
    const configTask = config.value === null ? loadConfig() : null
    if (!loaded.value) {
      try {
        snapshot.value = await sysmonApi.getSnapshot()
        error.value = null
      } catch (e) {
        // 503（未上报）/网络失败：保持无快照态，页面显示等待空态，后续 WS 推送会补上
        error.value = e instanceof Error ? e.message : String(e)
      } finally {
        loaded.value = true
      }
    }
    if (configTask) await configTask
  }

  /**
   * WS `sysmon_snapshot` 增量与 `snapshot` 快照域共用（幂等全量覆盖）。
   * 非法 payload 忽略，不写不抛（契约 sysmon §5：dzweb 已校验，前端仍做防御）。
   */
  function applySnapshot(payload: unknown): void {
    const p = payload as Partial<SysmonSnapshotPayload> | null | undefined
    if (!p || typeof p !== 'object') return
    if (typeof p.timestamp !== 'number') return
    if (!p.availability || typeof p.availability !== 'object') return
    if (!p.availability.metrics || typeof p.availability.metrics !== 'object') return
    snapshot.value = p as SysmonSnapshotPayload
    loaded.value = true
    error.value = null
  }

  /** 指标可用性条目（契约 sysmon §2.5；缺失 = 不可用） */
  function availabilityOf(metric: string): SysmonAvailabilityEntry | undefined {
    return snapshot.value?.availability.metrics[metric]
  }

  /**
   * CPU 总使用率（比率）。仅在可用性映射标记可用且值为有效数字时返回，
   * 否则 null —— 首次采样/计数器不可用一律不得用 0 冒充（契约 sysmon §2.7）。
   */
  const cpuUsageRatio = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.total']?.available !== true) return null
    const ratio = s.cpu.total.usage_ratio
    return typeof ratio === 'number' && Number.isFinite(ratio) ? ratio : null
  })

  /**
   * 每核使用率（契约 sysmon §2.3）。仅当可用性映射标记 cpu.cores 可用时返回数组，
   * 否则空数组 —— 视图据此区分「指标不可用」与「可用但暂无数据」，不得用 0 冒充。
   * 核数随快照变化（热插拔 / 虚拟机缩放），每次按当前快照数组渲染，不跨快照合并。
   */
  const cpuCores = computed<SysmonCpuCore[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.cores']?.available !== true) return []
    const cores = Array.isArray(s.cpu?.cores) ? s.cpu.cores : []
    return cores.map((core, position) => ({
      // index 非法时退化为数组下标，保证列表 key / 展示稳定（正常 payload 不会走到）
      index: typeof core?.index === 'number' && Number.isFinite(core.index) ? core.index : position,
      usage_ratio: toFiniteNumber(core?.usage_ratio),
      saturated: core?.saturated === true,
    }))
  })

  /** 持续饱和核 index 集合（契约 sysmon §2.3）：cpu.cores 不可用时为空数组 */
  const saturatedCores = computed<number[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.cores']?.available !== true) return []
    const indices = Array.isArray(s.cpu?.saturated_cores) ? s.cpu.saturated_cores : []
    const unique = new Set<number>()
    for (const index of indices) {
      if (typeof index === 'number' && Number.isInteger(index) && index >= 0) unique.add(index)
    }
    return [...unique].sort((a, b) => a - b)
  })

  /**
   * 系统负载（契约 sysmon §2.3）。仅在可用性映射标记可用且快照携带对象时返回；
   * semantic 说明口径（Linux load_average / Windows processor_queue），前端不做跨口径比较。
   */
  const cpuLoadAverage = computed<SysmonLoadAverage | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.load_average']?.available !== true) return null
    const load = s.cpu?.load_average
    if (!load || typeof load !== 'object') return null
    return {
      one: toFiniteNumber(load.one),
      five: toFiniteNumber(load.five),
      fifteen: toFiniteNumber(load.fifteen),
      semantic: typeof load.semantic === 'string' ? load.semantic : '',
    }
  })

  /**
   * 每秒上下文切换 / 每秒中断（契约 sysmon §2.3、§2.5）。
   * 两者共用可用性键 `cpu.context_switches`；值不可用时返回 null，不得用 0 冒充。
   */
  const cpuContextSwitchesPerSec = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.context_switches']?.available !== true) return null
    return toFiniteNumber(s.cpu?.context_switches_per_sec)
  })

  const cpuInterruptsPerSec = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.context_switches']?.available !== true) return null
    return toFiniteNumber(s.cpu?.interrupts_per_sec)
  })

  /**
   * 温度传感器读数（契约 sysmon §2.4，B 组可用性样板 #73）。仅当可用性映射标记
   * `temperature` 可用时返回过滤后的数组，否则空数组 —— 视图据此整块不渲染
   * （不可用不是 0，也不是「不可用」占位）。过滤空 sensor / 非有限 celsius。
   */
  const temperatures = computed<SysmonTemperatureInfo[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['temperature']?.available !== true) return []
    const readings = Array.isArray(s.temperature) ? s.temperature : []
    const result: SysmonTemperatureInfo[] = []
    for (const reading of readings) {
      const sensor = typeof reading?.sensor === 'string' ? reading.sensor.trim() : ''
      const celsius = toFiniteNumber(reading?.celsius)
      if (!sensor || celsius === null) continue
      result.push({ sensor, celsius })
    }
    return result
  })

  /**
   * 每核当前频率（契约 sysmon §2.3，B 组可用性样板 #73）。仅当可用性映射标记
   * `cpu.frequency` 可用时返回过滤后的数组，否则空数组 —— 视图据此整块不渲染。
   * index 与 cpu.cores[].index 同源（OS CPU 编号，不保证连续）；mhz 须为正整数。
   */
  const coreFrequencies = computed<SysmonCoreFrequency[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.frequency']?.available !== true) return []
    const frequencies = Array.isArray(s.cpu?.core_frequencies) ? s.cpu.core_frequencies : []
    const result: SysmonCoreFrequency[] = []
    for (const frequency of frequencies) {
      const index = frequency?.index
      const mhz = frequency?.mhz
      if (typeof index !== 'number' || !Number.isInteger(index) || index < 0) continue
      if (typeof mhz !== 'number' || !Number.isInteger(mhz) || mhz <= 0) continue
      result.push({ index, mhz })
    }
    return result
  })

  /**
   * 内存信息（契约 sysmon §2.4）。仅当可用性映射标记 `memory` 可用且快照携带对象时
   * 返回净化后的对象，否则 null —— 视图据此区分「指标不可用」与「可用但本帧未采集」。
   *
   * 唯一口径 `used_bytes = total_bytes − available_bytes`、`used_ratio = used_bytes / total_bytes`
   * 均按公式重算，不信任 payload 中的派生值（防发送端口径漂移，保证字节与百分比一致）。
   * 核心口径非法（NaN/Infinity/负数/总量为 0/可用大于总量）时整块返回 null，不用 0 冒充；
   * `cached_bytes` / `buffers_bytes` 非法/负数 → null（本平台不提供/本帧未读到，视图不渲染该行）。
   */
  const memoryInfo = computed<SysmonMemoryInfo | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['memory']?.available !== true) return null
    const memory = s.memory
    if (!memory || typeof memory !== 'object') return null
    const total = toNonNegativeInteger(memory.total_bytes)
    const available = toNonNegativeInteger(memory.available_bytes)
    if (total === null || available === null || total <= 0 || available > total) return null
    const used = total - available
    return {
      total_bytes: total,
      available_bytes: available,
      used_bytes: used,
      used_ratio: used / total,
      cached_bytes: toNonNegativeInteger(memory.cached_bytes),
      buffers_bytes: toNonNegativeInteger(memory.buffers_bytes),
    }
  })

  /**
   * 交换分区 / 页面文件信息（契约 sysmon §2.4）。仅当可用性映射标记 `swap` 可用且快照
   * 携带对象时返回净化后的对象，否则 null。无交换时 payload 为 `total_bytes = 0` 的对象，
   * 此时仍返回对象（视图显示「无交换分区」而非「不可用」），`used_ratio` 恒为 null。
   * `used_bytes` 钳制到 `[0, total_bytes]`，`used_ratio` 按 `used_bytes / total_bytes` 重算
   * （不信任 payload 中的派生值，保证字节与百分比一致）。
   */
  const swapInfo = computed<SysmonSwapInfo | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['swap']?.available !== true) return null
    const swap = s.swap
    if (!swap || typeof swap !== 'object') return null
    const total = toNonNegativeInteger(swap.total_bytes)
    const used = toNonNegativeInteger(swap.used_bytes)
    if (total === null || used === null) return null
    const clampedUsed = Math.min(used, total)
    return {
      total_bytes: total,
      used_bytes: clampedUsed,
      used_ratio: total > 0 ? clampedUsed / total : null,
    }
  })

  /**
   * 卷容量与可用性（契约 sysmon §2.4，#75）。仅当可用性映射标记 `volumes` 可用且快照携带
   * 数组时返回净化后的数组，否则空数组 —— 视图据此区分「指标不可用」与「可用但本帧无卷」。
   *
   * 与内存 / 交换不同：`available=false` 的卷**必须保留**（"卷存在但容量读不到"是真实事实，
   * 消费方要能显示「不可用」+ 原因），但容量字段一律净化成 null，不得用 0 冒充。
   * 净化规则：mount 为空串剔除；`used_bytes` 钳制到 `[0, total]`；`used_ratio` 按
   * `used_bytes / total_bytes` 重算（不信任 payload 派生值，保证字节与百分比一致），
   * `total_bytes = 0`（异常/特殊卷）时为 null。按 watched 优先、其次 mount 升序排序，
   * 顶栏徽章因此天然把关注卷排在前面（不依赖发送端顺序）。
   */
  const volumes = computed<SysmonVolumeInfo[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['volumes']?.available !== true) return []
    const entries = Array.isArray(s.volumes) ? s.volumes : []
    const result: SysmonVolumeInfo[] = []
    for (const entry of entries) {
      const mount = typeof entry?.mount === 'string' ? entry.mount.trim() : ''
      if (!mount) continue
      // available=false 的卷保留：容量不可读是真实事实，视图显示「不可用」+ reason
      if (entry?.available !== true) {
        result.push({
          mount,
          watched: entry?.watched === true,
          available: false,
          total_bytes: null,
          used_bytes: null,
          used_ratio: null,
          reason: typeof entry.reason === 'string' && entry.reason.trim() ? entry.reason.trim() : undefined,
        })
        continue
      }
      const total = toNonNegativeInteger(entry.total_bytes)
      const used = toNonNegativeInteger(entry.used_bytes)
      // 标称可用但容量字段非法：按不可读处理（保留卷，容量 null），不得用 0 冒充
      if (total === null || used === null) {
        result.push({
          mount,
          watched: entry.watched === true,
          available: false,
          total_bytes: null,
          used_bytes: null,
          used_ratio: null,
          reason: typeof entry.reason === 'string' && entry.reason.trim() ? entry.reason.trim() : 'collect_failed',
        })
        continue
      }
      const clampedUsed = Math.min(used, total)
      result.push({
        mount,
        watched: entry.watched === true,
        available: true,
        total_bytes: total,
        used_bytes: clampedUsed,
        used_ratio: total > 0 ? clampedUsed / total : null,
      })
    }
    return result.sort((a, b) => {
      if (a.watched !== b.watched) return a.watched ? -1 : 1
      return a.mount.localeCompare(b.mount)
    })
  })

  /**
   * 关注路径的落卷解析（契约 sysmon §2.4，#75）。仅当快照携带数组时返回过滤后的数组
   * （`watch_paths` 没有独立可用性键，落卷解析不受 `volumes` 采集开关影响，见 §2.5）。
   * path 为空串的条目剔除；`available=false` 的条目保留（`mount` 强制 null + reason），
   * 视图据此显示「未解析」而不是猜一个卷。mount 由发送端与 `volumes[].mount` 相等比较，
   * 前端不再二次匹配（契约保证同一帧内可直接相等比较）。
   */
  const watchPaths = computed<SysmonWatchPathInfo[]>(() => {
    const s = snapshot.value
    if (!s) return []
    const entries = Array.isArray(s.watch_paths) ? s.watch_paths : []
    const result: SysmonWatchPathInfo[] = []
    for (const entry of entries) {
      const path = typeof entry?.path === 'string' ? entry.path.trim() : ''
      if (!path) continue
      const mount = typeof entry?.mount === 'string' && entry.mount.trim() ? entry.mount.trim() : null
      const available = entry?.available === true && mount !== null
      result.push({
        path,
        mount: available ? mount : null,
        available,
        reason: available
          ? undefined
          : (typeof entry?.reason === 'string' && entry.reason.trim() ? entry.reason.trim() : 'collect_failed'),
      })
    }
    return result
  })

  /**
   * 共享内存目录占用（契约 sysmon §2.4，#75）：页清理是否生效的可观测项。
   * 仅在可用性映射标记 `shm_dir` 可用且快照携带对象时返回净化后的对象，否则 null。
   * `path` 非法或 `total_bytes` / `page_files` 非法（NaN/Infinity/负数/非整数）整块返回 null，
   * 不用 0 冒充（契约明确：读取失败不得输出全 0 对象）；
   * `oldest_file_age_sec` 非法/负数 → null（无页文件或本帧未读到，视图不渲染该行）。
   */
  const shmDirInfo = computed<SysmonShmDirInfo | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['shm_dir']?.available !== true) return null
    const info = s.shm_dir
    if (!info || typeof info !== 'object') return null
    const path = typeof info.path === 'string' ? info.path.trim() : ''
    const total = toNonNegativeInteger(info.total_bytes)
    const pageFiles = toNonNegativeInteger(info.page_files)
    if (!path || total === null || pageFiles === null) return null
    return {
      path,
      total_bytes: total,
      page_files: pageFiles,
      oldest_file_age_sec: toNonNegativeInteger(info.oldest_file_age_sec),
    }
  })

  /**
   * 每物理盘 I/O（契约 sysmon §2.4，#76）。仅当可用性映射标记 `disk_io` 可用且快照携带
   * 数组时返回净化后的数组，否则空数组 —— 视图据此区分「指标不可用」与「可用但暂无数据」。
   *
   * 七项计量字段一律净化：首次采样 / 计数器重置时发送端输出 null，**不得用 0 冒充**。
   * 速率 / IOPS / 延迟 / 队列深度须为有限非负数（负数/NaN/Infinity → null，0 是合法真实值）；
   * `utilization` 为 [0,1] 比率，越界/非有限 → null。`device` 为空串的条目剔除；
   * 按 device `localeCompare` 升序排序，不依赖发送端顺序。
   */
  const diskIo = computed<SysmonDiskIoInfo[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['disk_io']?.available !== true) return []
    const devices = Array.isArray(s.disk_io) ? s.disk_io : []
    const result: SysmonDiskIoInfo[] = []
    for (const entry of devices) {
      const device = typeof entry?.device === 'string' ? entry.device.trim() : ''
      if (!device) continue
      const utilization = toNonNegativeNumber(entry?.utilization)
      result.push({
        device,
        read_bytes_per_sec: toNonNegativeNumber(entry?.read_bytes_per_sec),
        write_bytes_per_sec: toNonNegativeNumber(entry?.write_bytes_per_sec),
        read_iops: toNonNegativeNumber(entry?.read_iops),
        write_iops: toNonNegativeNumber(entry?.write_iops),
        latency_ms: toNonNegativeNumber(entry?.latency_ms),
        queue_depth: toNonNegativeNumber(entry?.queue_depth),
        utilization: utilization !== null && utilization <= 1 ? utilization : null,
      })
    }
    return result.sort((a, b) => a.device.localeCompare(b.device))
  })

  /** 跨平台语义提示（契约 sysmon §2.5）：缺失 / 非字符串返回空串，由视图决定是否展示 */
  function semanticHint(key: string): string {
    const hints = snapshot.value?.availability.semantic_hints
    if (!hints || typeof hints !== 'object') return ''
    const hint = hints[key]
    return typeof hint === 'string' ? hint.trim() : ''
  }

  // ===== network（#77）：每网卡累计收发与速率 + 显示全部网卡开关 =====

  /**
   * 每网卡收发（契约 sysmon §2.4 `network[]`）。仅当可用性映射标记 `network` 可用且快照携带
   * 数组时返回净化后的数组，否则空数组 —— 视图据此区分「指标不可用」与「可用但本帧无网卡」。
   *
   * 关键语义（契约 §2.5）：`hide_virtual=true` 时发送方已过滤虚拟网卡；若**全部**网卡都被
   * 滤掉，payload 是**空数组**而非 null、且 `available=true`（"全部被过滤"与"采集失败"是不同
   * 事实）。因此空数组必须原样返回空数组，**不得**降级成「不可用」。
   *
   * 净化规则：interface 空串剔除；八个 `*_total` 用 `toNonNegativeInteger`（累计计数器没有
   * 不可用语义，非法值即剔除该条——不产出残缺行）；六个 `*_per_sec` 用 `toNonNegativeNumber`
   * （0 是合法真实值，负数/NaN/Infinity → null 表示不可用，不得用 0 冒充）；`is_virtual` 严格
   * 取 `=== true`。按 interface `localeCompare` 升序排序，不依赖发送端顺序。
   */
  const network = computed<SysmonNetworkInfo[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['network']?.available !== true) return []
    const entries = Array.isArray(s.network) ? s.network : []
    const result: SysmonNetworkInfo[] = []
    for (const entry of entries) {
      const name = typeof entry?.interface === 'string' ? entry.interface.trim() : ''
      if (!name) continue
      const rxBytes = toNonNegativeInteger(entry?.rx_bytes_total)
      const txBytes = toNonNegativeInteger(entry?.tx_bytes_total)
      const rxPackets = toNonNegativeInteger(entry?.rx_packets_total)
      const txPackets = toNonNegativeInteger(entry?.tx_packets_total)
      const rxErrors = toNonNegativeInteger(entry?.rx_errors_total)
      const txErrors = toNonNegativeInteger(entry?.tx_errors_total)
      const rxDrops = toNonNegativeInteger(entry?.rx_drops_total)
      const txDrops = toNonNegativeInteger(entry?.tx_drops_total)
      // 八个累计计数器是必备字段（无"不可用"语义）：任一非法说明该条不可信，整条剔除
      if (rxBytes === null || txBytes === null || rxPackets === null || txPackets === null
        || rxErrors === null || txErrors === null || rxDrops === null || txDrops === null) continue
      result.push({
        interface: name,
        rx_bytes_total: rxBytes,
        tx_bytes_total: txBytes,
        rx_packets_total: rxPackets,
        tx_packets_total: txPackets,
        rx_errors_total: rxErrors,
        tx_errors_total: txErrors,
        rx_drops_total: rxDrops,
        tx_drops_total: txDrops,
        rx_bytes_per_sec: toNonNegativeNumber(entry?.rx_bytes_per_sec),
        tx_bytes_per_sec: toNonNegativeNumber(entry?.tx_bytes_per_sec),
        rx_errors_per_sec: toNonNegativeNumber(entry?.rx_errors_per_sec),
        tx_errors_per_sec: toNonNegativeNumber(entry?.tx_errors_per_sec),
        rx_drops_per_sec: toNonNegativeNumber(entry?.rx_drops_per_sec),
        tx_drops_per_sec: toNonNegativeNumber(entry?.tx_drops_per_sec),
        is_virtual: entry?.is_virtual === true,
      })
    }
    return result.sort((a, b) => a.interface.localeCompare(b.interface))
  })

  /**
   * 采集到但数组为空（契约 §2.5：hide_virtual 过滤后可能为空数组，且 `available` 仍为 true）。
   * 看的是**发送端数组**而非净化结果：净化剔掉的非法条目不算「被隐藏」，
   * 视图据此只在 hide_virtual=true 时显示「已隐藏全部虚拟网卡」，否则显示「暂无数据」。
   * `network` 为 null / 缺失（能力可用但本帧未采集）时返回 false —— 那是「暂无数据」，
   * 不是「全部被隐藏」，两者是不同事实。
   */
  const networkFilteredAll = computed<boolean>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['network']?.available !== true) return false
    return Array.isArray(s.network) && s.network.length === 0
  })

  /**
   * 当前活动阈值告警（契约 sysmon §2.8）：快照 `alerts[]` 的净化结果。
   *
   * 这是**持续状态**（冷却期内被抑制的通知不影响它），供监控页常驻提示使用；
   * 与 `notify_ui` 通知链路互补——通知是事件，本数组是状态。
   *
   * 净化规则：未知 kind / volume_free 空挂载点剔除；非 volume_free 的 subject 强制空串；
   * `since` 非法（NaN/负数/非整数）置 0（不展示"始于"，不得用假时间冒充）；
   * `(kind, subject)` 去重；按 kind 升序、卷按挂载点升序稳定排序，不依赖发送端顺序。
   */
  const activeAlerts = computed<SysmonActiveAlert[]>(() => {
    const entries = Array.isArray(snapshot.value?.alerts) ? snapshot.value.alerts : []
    const result: SysmonActiveAlert[] = []
    const seen = new Set<string>()
    for (const entry of entries as SysmonAlertInfo[]) {
      const kind = SYSMON_ALERT_KINDS.find(candidate => candidate === entry?.kind)
      if (!kind) continue
      const subject = kind === 'volume_free'
        ? (typeof entry.subject === 'string' ? entry.subject.trim() : '')
        : ''
      if (kind === 'volume_free' && !subject) continue
      const key = `${kind}|${subject}`
      if (seen.has(key)) continue
      seen.add(key)
      const since = toNonNegativeInteger(entry.since) ?? 0
      result.push({ kind, subject, since })
    }
    const kindOrder = (kind: SysmonAlertKind): number => SYSMON_ALERT_KINDS.indexOf(kind)
    return result.sort((a, b) => {
      const byKind = kindOrder(a.kind) - kindOrder(b.kind)
      return byKind !== 0 ? byKind : a.subject.localeCompare(b.subject)
    })
  })

  /** 生效配置镜像（契约 sysmon §3 RTN_SYSMON_CONFIG 全量回推）；未收到时为 null */
  const config = ref<SysmonConfigPayload | null>(null)
  /** 配置补拉进行中（防 Topbar / 监控页并发 init 重复请求） */
  const configLoading = ref(false)

  /**
   * 补拉生效配置镜像（契约 rest §2.8 `GET /api/sysmon/config`）。
   *
   * 503（dzweb 镜像尚未就绪）与网络失败都**不抛错、不弹错**：保持 config=null，
   * 页面显示「配置尚未到达」；WS `sysmon_config`（快照域，契约 webui-ws §2.1）到达即补上。
   * 失败原因不对外暴露：页面只需要「有没有生效值」这一事实。
   */
  async function loadConfig(): Promise<void> {
    if (configLoading.value || config.value !== null) return
    configLoading.value = true
    try {
      const payload = await sysmonApi.getConfig()
      // 等待期间 WS `sysmon_config` 可能已经到达（它才是生效信号）：REST 响应更旧，
      // 不得覆盖已生效镜像，否则配置展示会短暂回退到旧值。
      if (config.value !== null) return
      if (!isFullConfigPayload(payload)) return  // 半截配置不入镜像，等 WS 全量回推
      config.value = payload
    } catch {
      // 503 / 网络失败：保持空镜像，等待 WS 补上（不弹错、不伪造默认值）
    } finally {
      configLoading.value = false
    }
  }

  /**
   * WS `sysmon_config` / `snapshot` 快照域共用（幂等全量覆盖，契约 sysmon §3.2）。
   * 非法 payload 忽略、不写不清 pending（对齐 settings.applyEventShmConfig）：
   * 只有形状可用的全量配置才刷新镜像，并同时清「完整配置保存」与「显示全部网卡」两个 pending
   * （REST 成功只表示"已下发"，真正生效信号是本消息；校验失败时发送方回推旧值，pending 同样清）。
   */
  function applyConfig(payload: unknown): void {
    const p = payload as Partial<SysmonConfigPayload> | null | undefined
    if (!isFullConfigPayload(p)) return
    config.value = p as SysmonConfigPayload
    sysmonPending.resolve(SYSMON_CONFIG_KEY)
    sysmonPending.resolve(SYSMON_NETWORK_CONFIG_KEY)
  }

  /**
   * 「隐藏虚拟网卡」开关的当前值（契约 sysmon §3.1 `network.hide_virtual`，默认 true）。
   * 从生效配置镜像读；镜像未到达时按契约默认值 true 处理（不是「未知」占位）——
   * 这样开关初值不会因为配置帧晚到而闪成 false。
   */
  const hideVirtual = computed<boolean>(() => config.value?.network.hide_virtual !== false)

  /** 配置切换挂起中（REST 已发出、等 WS `sysmon_config`）：开关 disabled 防重复提交 */
  const hideVirtualPending = computed(() => sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY] ?? false)

  /**
   * 提交 `network.hide_virtual`（契约 rest §2.8 → SET_SYSMON_CONFIG）。
   *
   * pending 语义：成功保持挂起直到 WS `sysmon_config` 到达（applyConfig 清）；HTTP 失败立即清。
   * 返回值对齐 settings 的配置操作：true = 已下发；false = 防重入 / HTTP 失败；
   * PENDING_TIMEOUT = 超时（usePending 已 toast，调用方据此去重双 toast）。
   * 实际生效值以 `hideVirtual`（配置镜像）为准，视图不做乐观更新 —— 否则下发的值被 dzsysmon
   * 校验拒绝时 UI 会显示一个并未生效的状态。
   */
  async function setHideVirtual(hide: boolean): Promise<boolean | typeof PENDING_TIMEOUT> {
    if (hideVirtualPending.value) return false
    const ok = await sysmonPending.run(
      SYSMON_NETWORK_CONFIG_KEY,
      () => sysmonApi.setConfig({ network: { hide_virtual: hide } }),
      {
        opLabel: '显示全部网卡切换',
        timeout: SYSMON_CONFIG_TIMEOUT_MS,
        distinguishTimeout: true,
      },
    )
    if (ok === PENDING_TIMEOUT) return PENDING_TIMEOUT
    return ok !== undefined
  }

  /** 完整配置保存挂起中（REST 已发出、等 WS `sysmon_config`）：表单 / 保存按钮 disabled 防重复提交 */
  const configPending = computed(() => sysmonPending.pending[SYSMON_CONFIG_KEY] ?? false)

  /**
   * 提交完整配置 merge patch（契约 rest §2.8 → SET_SYSMON_CONFIG，admin）。
   *
   * pending 语义与 setHideVirtual 一致：成功保持挂起直到 WS `sysmon_config` 到达（applyConfig 清）；
   * HTTP 失败立即清。返回值：true = 已下发；false = 防重入 / HTTP 失败；
   * PENDING_TIMEOUT = 超时（usePending 已 toast，调用方据此去重双 toast）。
   * 实际生效值以 `config`（配置镜像）为准，视图不做乐观更新——校验失败时 dzsysmon 回推旧值，
   * 表单必须跟着镜像回退，不能显示一个并未生效的值。
   */
  async function setConfig(patch: SysmonConfigPatch): Promise<boolean | typeof PENDING_TIMEOUT> {
    if (configPending.value) return false
    const ok = await sysmonPending.run(
      SYSMON_CONFIG_KEY,
      () => sysmonApi.setConfig(patch),
      {
        opLabel: '系统监控配置保存',
        timeout: SYSMON_CONFIG_TIMEOUT_MS,
        distinguishTimeout: true,
      },
    )
    if (ok === PENDING_TIMEOUT) return PENDING_TIMEOUT
    return ok !== undefined
  }

  return {
    snapshot,
    loaded,
    error,
    cpuUsageRatio,
    cpuCores,
    saturatedCores,
    cpuLoadAverage,
    cpuContextSwitchesPerSec,
    cpuInterruptsPerSec,
    temperatures,
    coreFrequencies,
    memoryInfo,
    swapInfo,
    volumes,
    watchPaths,
    shmDirInfo,
    diskIo,
    network,
    networkFilteredAll,
    activeAlerts,
    config,
    hideVirtual,
    hideVirtualPending,
    configPending,
    semanticHint,
    init,
    loadConfig,
    applySnapshot,
    applyConfig,
    setConfig,
    setHideVirtual,
    availabilityOf,
  }
})
