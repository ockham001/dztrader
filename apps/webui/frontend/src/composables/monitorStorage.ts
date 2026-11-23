/**
 * 存储块的展示口径（#450；契约 sysmon §2.4 `volumes` / `disk_io` / `shm_dir`、§3.1 阈值）
 *
 * 契约是唯一语义真相源；本模块只承载**展示侧的口径**——把 store 已净化的数据翻译成
 * 「顶部四数字 / 卷行 / 盘行 / 共享内存一行」要显示的字符串与等级，不含协议判断（净化在
 * `stores/sysmon.ts`）、不含渲染（在 `views/MonitorView.vue`）。
 *
 * 五条不可动摇的约定：
 * 1. **空间不足按契约阈值判**（`thresholds.volume_free_pct`，默认 10）：判据是**四舍五入到
 *    展示精度后的空闲百分比** `<` 阈值。若按原始浮点判，`used_ratio = 0.9` 会算出
 *    `(1 − 0.9) × 100 = 9.999…`，恰好卡在默认阈值 10 上显示「空间不足」——一个精度噪声
 *    伪装成的越线结论。展示值（`10.0% 可用`）与判定值必须是同一个数。
 * 2. **容量条的填充是「已用」**（与资源管理器 / 存储设置同读法）：填充比例 = `used_ratio`；
 *    颜色是**数据型图形**（#450 拍板 4）：正常中性，可用 < 30% 橙（提前提醒），可用低于契约
 *    `volume_free_pct` 红——danger 与「空间不足」chip / 顶部计数同源（freePercent），
 *    不与 70 / 90 展示分档混用（那套只用于 CPU / 内存使用率等状态型数值）。
 *
 * **已知差异（≤0.05pp）**：契约日志按原始 `free_pct = 1 − used_ratio` 判，例如
 * `used_ratio = 0.9005` 原始 free = 9.95 → 日志 warning，页面展示 10.0 → 不标不足；
 * 这是「页面只承诺自己的展示精度」的取舍，对账时不要当成回归。
 * 3. **不可用不等于 0**：`null` 走 `format.ts` 的不可用占位；字节为 0 只在求和结果确实是 0
 *    （无可用卷）时出现，且空态文案另有说明。
 * 4. **磁盘忙碌占比不叫「利用率」**：`utilization` 度量的是设备忙时间占比（契约 §2.4），
 *    叫「利用率」会被读成「吞吐占带宽的比例」。
 * 5. **延迟 / 队列深度复用 `format.ts`**：不在这里另写 `toFixed`（同一单位族的格式化只有
 *    一份实现，#126）。
 */
import {
  UNAVAILABLE_TEXT,
  formatAge,
  formatByteRate,
  formatBytes,
  formatCount,
  formatDecimal,
  formatLatency,
  formatPercent,
} from '@/composables/format'
import {
  DISABLED_BY_CONFIG_TEXT,
  unavailableReasonText,
} from '@/composables/monitorAvailability'
import { resourceLevel, type ResourceLevel } from '@/composables/useResourceLevel'
import type {
  SysmonDiskIoInfo,
  SysmonShmDirInfo,
  SysmonThresholdsConfigPayload,
  SysmonVolumeInfo,
} from '@/types/generated'

/**
 * 契约 sysmon §3.1 `thresholds.volume_free_pct` 的默认值（**副本，用于配置镜像尚未到达时的
 * 展示**）。发送方是唯一真相源：`dzsysmon` 未收到配置帧时也按 10 工作，前端在配置到达前的
 * 窗口内必须给出同一结论，否则「空间不足」会先按一套阈值显示、配置到达后跳变。
 */
export const DEFAULT_VOLUME_FREE_PCT = 10

/** 解析生效的空闲阈值：配置镜像未到达 / 字段非法时回退契约默认值 */
export function resolveVolumeFreePct(
  thresholds: SysmonThresholdsConfigPayload | null | undefined,
): number {
  const value = thresholds?.volume_free_pct
  return typeof value === 'number' && Number.isFinite(value) ? value : DEFAULT_VOLUME_FREE_PCT
}

/**
 * 卷的空闲百分比（1 位小数）——**展示与判定共用**这一个值。
 *
 * `used_ratio` 已由 store 净化为 `used_bytes / total_bytes`；`1 − used_ratio` 的浮点噪声在
 * 这里按展示精度（1 位小数）取整消掉，避免「显示 10.0% 可用」却标「空间不足」。
 * `used_ratio === null`（不可读 / 总量为 0）返回 null——没有分母就没有可用比例。
 */
export function volumeFreePercent(volume: Pick<SysmonVolumeInfo, 'used_ratio'>): number | null {
  if (volume.used_ratio === null) return null
  return Math.round((1 - volume.used_ratio) * 1_000) / 10
}

/** 卷的一行（视图只负责摆；`readableVolumes` 已保证 `used_ratio` 非 null） */
export interface VolumeRow {
  mount: string
  /** 容量条按「已用」填充的百分比文本（`formatPercent` 口径） */
  fill: string
  /**
   * 容量条的展示等级：**日常中性**（normal 也不上色），按 `useResourceLevel` 的 70 / 90
   * 展示分档取 warn / danger——与顶栏卷徽章共用同一套色标（#369 一致性）。
   * null = 不可读、不套色；与契约阈值的「空间不足」chip 是两套数字，不得互相推导。
   */
  level: ResourceLevel | null
  /** `可用 60.0 GiB / 共 100.0 GiB`（可用值在视图里加粗为主色） */
  availableText: string
  /** 可用容量（字节，供视图加粗段与测试取数） */
  freeBytes: number | null
  /** 总量（字节，供视图渲染「/ 共 Y」段） */
  totalBytes: number | null
  /** `60.0% 可用` */
  freeText: string
  /** 可用比例低于契约阈值（判据见模块头约定 1） */
  insufficient: boolean
}

/** 构造单个卷行；`freeThresholdPct` 来自 {@link resolveVolumeFreePct} */
export function buildVolumeRow(volume: SysmonVolumeInfo, freeThresholdPct: number): VolumeRow {
  const total = volume.total_bytes
  const used = volume.used_bytes
  const free = total === null || used === null ? null : total - used
  const freePercent = volumeFreePercent(volume)
  // 容量条色标与顶栏卷徽章同源（70 / 90 展示分档）；「空间不足」chip 仍按契约阈值判。
  const level: ResourceLevel | null = volume.used_ratio === null ? null : resourceLevel(volume.used_ratio)
  return {
    mount: volume.mount,
    fill: formatPercent(volume.used_ratio),
    level,
    availableText: `${formatBytes(free)} / ${formatBytes(total)}`,
    freeBytes: free,
    totalBytes: total,
    freeText: freePercent === null ? `${UNAVAILABLE_TEXT} 可用` : `${freePercent.toFixed(1)}% 可用`,
    insufficient: freePercent !== null && freePercent < freeThresholdPct,
  }
}

/** 存储块顶部四数字的输入（全部由 store 已净化的 computed 给值，本模块不读 store） */
export interface StorageSummaryInput {
  readableVolumes: readonly SysmonVolumeInfo[]
  zeroCapacityVolumes: readonly SysmonVolumeInfo[]
  diskCount: number
  freeThresholdPct: number
}

/** 最紧的卷：空闲百分比最低者（同值保持 store 的固定顺序，不重排） */
export interface TightestVolume {
  mount: string
  freePercent: number
  freeBytes: number | null
}

/** 存储块顶部四数字的展示模型 */
export interface StorageSummary {
  /** 可读卷数（容量可读且总量 > 0） */
  volumeCount: number
  /** 无容量 / 不可读挂载数（顶部「卷」的 context） */
  zeroCount: number
  /** 目标物理盘数（块头 meta / 磁盘 I/O 子区共用；能力不可用时调用方传 0） */
  diskCount: number
  /** 可读卷 total 之和 */
  totalBytes: number
  /** 可读卷 (total − used) 之和 */
  freeBytes: number
  /** 可用占总量的百分比文本（`72.1%`；总量为 0 时 `—`） */
  freeTotalPercentText: string
  /** 可用比例低于契约阈值的卷数 */
  insufficientCount: number
  /** 可用比例最低的卷（无可用卷为 null）；context 用它写「最紧的卷」 */
  tightest: TightestVolume | null
}

/** 构造存储块顶部四数字；求和只对本帧真实可读的卷，不用 0 代替缺失（约定 3） */
export function buildStorageSummary(input: StorageSummaryInput): StorageSummary {
  let totalBytes = 0
  let freeBytes = 0
  let insufficientCount = 0
  let tightest: TightestVolume | null = null
  for (const volume of input.readableVolumes) {
    const total = volume.total_bytes
    const used = volume.used_bytes
    if (total !== null) totalBytes += total
    if (total !== null && used !== null) {
      const free = total - used
      freeBytes += free
      const freePercent = volumeFreePercent(volume)
      if (freePercent !== null) {
        if (freePercent < input.freeThresholdPct) insufficientCount += 1
        // 严格小于才替换：同百分比时保留先出现的卷（store 固定顺序 → 展示稳定）
        if (tightest === null || freePercent < tightest.freePercent) {
          tightest = { mount: volume.mount, freePercent, freeBytes: free }
        }
      }
    }
  }
  return {
    volumeCount: input.readableVolumes.length,
    zeroCount: input.zeroCapacityVolumes.length,
    diskCount: input.diskCount,
    totalBytes,
    freeBytes,
    freeTotalPercentText: totalBytes > 0 ? formatPercent(freeBytes / totalBytes) : '—',
    insufficientCount,
    tightest,
  }
}

/**
 * 卷不可用原因 → 文案（契约 §2.4 建议取值）。只有「无容量挂载」一行使用：
 * 读不到容量与「总量确实为 0」是两件事，由调用方分别给出原因。
 */
export function volumeReasonText(reason: string | undefined): string {
  if (!reason) return '容量读取失败'
  // 原因文案只有一份（`monitorAvailability.unavailableReasonText`）：配置关闭复用同一常量，
  // 未知 reason 原样透出（与契约 §2.5 的自由文案约定一致）；此处只补「无 reason」的卷语境兜底。
  if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
  return unavailableReasonText(reason)
}

/**
 * 无容量 / 不可读挂载的折叠行文本（#122 的口径保留，#450 收敛成**一行**）。
 *
 * 空数组返回 null（视图据此不渲染该行）；否则形如
 * `无容量挂载（2） · /backup（无访问权限） · /run/user/1000（总容量为 0）`。
 */
export function zeroCapacityText(volumes: readonly SysmonVolumeInfo[]): string | null {
  if (volumes.length === 0) return null
  const items = volumes.map(volume => {
    const reason = volume.available ? '总容量为 0' : volumeReasonText(volume.reason)
    return `${volume.mount}（${reason}）`
  })
  return `无容量挂载（${volumes.length}） · ${items.join(' · ')}`
}

/** 单块物理盘的一行（视图只负责摆） */
export interface DiskRow {
  device: string
  /** `读 64.0 MiB/s · 写 32.0 MiB/s`（null 走不可用占位） */
  ioText: string
  /** 忙碌条填充百分比文本；utilization 为 null 时为空串（不注入变量 → 空轨道） */
  busyFill: string
  /** 忙碌占比的展示等级（70 / 90 分档；null 不套色） */
  busyLevel: ResourceLevel | null
  /** `忙碌 72.0%`（null → `忙碌 不可用`） */
  busyText: string
  /** `延迟 0.45 ms · 队列 1.25`（任一 null 走不可用占位） */
  extraText: string
}

/**
 * 构造单块物理盘行。
 *
 * `utilization` 是 [0,1] 的设备忙时间占比（契约 §2.4）——**不叫利用率**，也不与网卡的
 * 「链路速率占比」混为一谈；色标同样只按 70 / 90 展示分档。
 */
export function buildDiskRow(disk: SysmonDiskIoInfo): DiskRow {
  const utilization = disk.utilization
  return {
    device: disk.device,
    ioText: `读 ${formatByteRate(disk.read_bytes_per_sec)} · 写 ${formatByteRate(disk.write_bytes_per_sec)}`,
    busyFill: utilization === null ? '' : formatPercent(utilization),
    busyLevel: utilization === null ? null : resourceLevel(utilization),
    busyText: `忙碌 ${formatPercent(utilization)}`,
    extraText: `延迟 ${formatLatency(disk.latency_ms)} · 队列 ${formatDecimal(disk.queue_depth)}`,
  }
}

/**
 * 共享内存（应用数据）一行：`720.0 MiB · 12 页文件 · 最旧 8 分钟`。
 *
 * `oldest_file_age_sec === null` 表示「当前没有页文件」这一合法事实（不是 0 秒），按契约写
 * 「不可用」占位——同一行里其它两项照常显示，不整行退化。
 */
export function shmUsageText(shm: SysmonShmDirInfo): string {
  return `${formatBytes(shm.total_bytes)} · ${shmDetailText(shm)}`
}

/**
 * 共享内存的细节段（`12 页文件 · 最旧 8 分钟`）：总量按钮之后的同一行小字与 tooltip 共用。
 *
 * `oldest_file_age_sec === null` 是「当前没有页文件」这一合法事实（契约 §2.4），
 * 不是「值缺失」——`page_files === 0` 时写「无页文件」，只有页文件数非 0 却读不到年龄
 * 才写「不可用」（否则会把一个可自行确认的空状态伪装成采集失败）。
 */
export function shmDetailText(shm: SysmonShmDirInfo): string {
  let oldest: string
  if (shm.page_files === 0) oldest = '无页文件'
  else if (shm.oldest_file_age_sec === null) oldest = UNAVAILABLE_TEXT
  else oldest = formatAge(shm.oldest_file_age_sec)
  return `${formatCount(shm.page_files)} 页文件 · 最旧 ${oldest}`
}
