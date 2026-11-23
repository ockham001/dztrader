/**
 * 内存块的展示口径（#445；契约 sysmon §2.4 / §3.1）
 *
 * 本模块承载内存块独有的纯逻辑：
 * - 组成带（已用 / 缓存 / 缓冲 / 空闲）的派生与退化口径（自 #363 从台账模块迁入，#445 内存卡化）；
 * - Meter / 数字格的悬停与读屏事实串（可用、空闲、阈值、余量）；
 * - 交换的标签与平台口径（Windows 的 swap = 页面文件支撑的提交上限）。
 *
 * 阈值默认值与台账同一纪律：配置镜像未到达时回退契约 §3.1 的默认值，
 * 让「刚进页面」与「配置到达后」不出现阈值跳变。
 */
import {
  formatBytes,
  formatPercent,
} from '@/composables/format'
import type {
  SysmonMemoryInfo,
  SysmonSwapInfo,
} from '@/types/generated'

/** 契约 §3.1 默认阈值副本（与发送方 dzsysmon 未收配置时的默认值一致） */
export const MEMORY_DEFAULT_USED_PCT = 90
export const SWAP_DEFAULT_USED_PCT = 50

/** 取阈值：配置已到达用它，未到达回退契约默认（配置镜像到达后不跳变） */
export function resolveThresholdPct(
  configured: number | null | undefined,
  fallback: number,
): number {
  return typeof configured === 'number' && Number.isFinite(configured) ? configured : fallback
}

/**
 * 字节余量文案（`余量 = 总量 × 阈值% − 已用`）：非负写剩余量，越线写「已超 X」。
 * 先按整字节取整再判符号：`total × pct / 100` 是浮点乘法，卡线时会算出 `-0.2 B` 这类残差，
 * 按原始值判符号会把「刚好到阈值」报成越线（与 #382 的台账字节余量同一坑）。
 */
export function bytesHeadroomText(headroomBytes: number): string {
  const rounded = Math.round(headroomBytes)
  return rounded >= 0 ? formatBytes(rounded) : `已超 ${formatBytes(-rounded)}`
}

/** 阈值上限 − 已用（字节）；调用方只在有读数时调用 */
function headroomBytes(totalBytes: number, usedBytes: number, thresholdPct: number): number {
  return totalBytes * (thresholdPct / 100) - usedBytes
}

/**
 * 阈值余量的事实串（#445 第四轮文案口径）：`距阈值 X` / `已超阈值 X`。
 * 「余量」容易被读成「还剩多少空闲内存」，而这里算的是「离告警线还有多少」，
 * 因此统一写成「距阈值」——与「告警阈值」配套，避免和可用/空闲混淆。
 */
export function headroomFactText(headroomBytes: number): string {
  const rounded = Math.round(headroomBytes)
  return rounded >= 0 ? `距阈值 ${formatBytes(rounded)}` : `已超阈值 ${formatBytes(-rounded)}`
}

/** 资源类阈值行（可见小字 / 悬停共用）：总量 + 已用 + 阈值 → `距阈值 X` / `已超阈值 X` */
export function resourceHeadroomText(
  totalBytes: number,
  usedBytes: number,
  thresholdPct: number,
): string {
  return headroomFactText(headroomBytes(totalBytes, usedBytes, thresholdPct))
}

/**
 * 内存数字格的悬停 / 读屏事实串（#445 第二轮拍板 B1 + B2）：
 * 已用/总量、可用（含可回收缓存/缓冲；与「空闲」不是一个口径）、空闲、阈值、余量。
 *
 * @param freeText 组成带「空闲」段的可读字节；组成带退化（缓存/缓冲不可得）时传 null，
 *   此时不写「空闲」——退化形态下组成带的 free 段其实是 available，不能拿它冒充空闲。
 */
export function memoryFacts(
  memory: SysmonMemoryInfo,
  thresholdPct: number,
  freeText: string | null,
): string {
  const parts = [
    `已用 ${formatBytes(memory.used_bytes)} / 共 ${formatBytes(memory.total_bytes)}`,
    `可用 ${formatBytes(memory.available_bytes)}（含可回收缓存/缓冲）`,
  ]
  if (freeText !== null) parts.push(`空闲 ${freeText}`)
  parts.push(`告警阈值 ${thresholdPct}%`)
  parts.push(headroomFactText(headroomBytes(memory.total_bytes, memory.used_bytes, thresholdPct)))
  return parts.join(' · ')
}

/**
 * 交换数字格的悬停 / 读屏事实串（#445 第二轮拍板 B3 + B4 + B6）：
 * 已用/总量、剩余、阈值、余量；Windows 另附提交上限口径说明（由调用方拼平台说明）。
 */
export function swapFacts(swap: SysmonSwapInfo, thresholdPct: number): string {
  const parts = [
    `已用 ${formatBytes(swap.used_bytes)} / 共 ${formatBytes(swap.total_bytes)}`,
    `剩余 ${formatBytes(Math.max(0, swap.total_bytes - swap.used_bytes))}`,
    `告警阈值 ${thresholdPct}%`,
    headroomFactText(headroomBytes(swap.total_bytes, swap.used_bytes, thresholdPct)),
  ]
  return parts.join(' · ')
}

/**
 * 交换区的展示标签（#445 第二轮拍板 B6）：Windows 的 swap 语义是「页面文件支撑的提交上限」，
 * 不是 Linux 的交换分区——标签随平台改口，另在悬停文案里带上完整口径说明。
 * 其它平台 / 平台信息缺失一律按 Linux 口径写「交换分区」（契约 §2.4）。
 */
export function swapLabel(platformFamily: string | null | undefined): string {
  return platformFamily === 'windows' ? '页面文件' : '交换分区'
}

/** Windows 交换口径说明（拼在悬停文案末尾；其它平台为空串） */
export function swapPlatformNote(platformFamily: string | null | undefined): string {
  return platformFamily === 'windows' ? '（页面文件支撑的提交上限）' : ''
}

// ---- 内存组成带（#363，自台账模块迁入；调用方只有内存卡）----

/** 组成带的一段：`ratio` 是占**总量**的比例（所有可画段之和 + 空闲 = 1） */
export interface MemoryCompositionSegment {
  id: 'used' | 'cached' | 'buffers' | 'free'
  label: string
  /** 该段的字节数（不可得时为 null，**不得**补 0） */
  bytes: number | null
  /** 该段占总量比例 [0,1]；null = 不可得（不画这一段） */
  ratio: number | null
  /** 该段的可读字节标注（不可得时为 null，由视图显示「不可得」注记） */
  text: string | null
}

/** 组成带的完整结果：分段 + 是否退化为两段（缓存 / 缓冲不可得时的标注口径） */
export interface MemoryComposition {
  segments: MemoryCompositionSegment[]
  /** 缓存或缓冲不可得 → 组成带只画「已用 + 可用」两段，视图须给出注记（不得静默补 0） */
  degraded: boolean
  /** 退化原因的可读文案（完整时为空串） */
  degradedText: string
}

/**
 * 内存组成带（#363；契约 sysmon §2.4 / ADR 0024）。
 *
 * **两个口径不合并**（这是本函数存在的唯一理由）：
 * - 主数值是 `used_ratio = (total − available) / total`（ADR 0024 的唯一口径，
 *   `available` 含**可回收**的缓存与缓冲）；
 * - 组成带要回答的是「这 32 GiB 被谁占着」，因此逐段必须相加回到 `total`：
 *   `已用 = total − free − cached − buffers`，其中 `free` **不是快照字段**，
 *   由 `free = total − used − cached − buffers` 派生（钳到 ≥0）。
 *
 * 两段数字在数值上必然自洽（把派生的 `free` 代回即得 `used`），但它们**不是同一个量**：
 * 主数值是「用量比率」，组成带是「总量分解」，视图必须分别标注分母，不得把同一个数字渲染两次
 * 而声称一个是「已用」另一个是「可用」。
 *
 * 缓存或缓冲为 null（Windows 恒无缓冲 / 本帧未读到）时**不补 0**：退化为「已用 + 可用」两段
 * （`used = total − available`、`available` 原样），并以 {@link MemoryComposition.degraded}
 * 标注——补 0 会把「不知道」画成「没有」。
 *
 * @returns 内存对象为 null（或总量非正）时返回 null（调用方不渲染组成带）
 */
export function buildMemoryComposition(memory: SysmonMemoryInfo | null): MemoryComposition | null {
  if (memory === null) return null
  const total = memory.total_bytes
  if (!(total > 0)) return null
  const cached = typeof memory.cached_bytes === 'number' ? memory.cached_bytes : null
  const buffers = typeof memory.buffers_bytes === 'number' ? memory.buffers_bytes : null
  const segment = (
    id: MemoryCompositionSegment['id'],
    label: string,
    bytes: number | null,
  ): MemoryCompositionSegment => ({
    id,
    label,
    bytes,
    ratio: bytes === null ? null : Math.min(1, Math.max(0, bytes / total)),
    text: bytes === null ? null : formatBytes(bytes),
  })

  if (cached === null || buffers === null) {
    // 退化形态：`available` 是契约里唯一可信的「可用」字段（含可回收），把它当空闲段并标注
    return {
      segments: [
        segment('used', '已用', Math.max(0, total - memory.available_bytes)),
        segment('free', '可用（含缓存/缓冲）', memory.available_bytes),
      ],
      degraded: true,
      degradedText: cached === null && buffers === null
        ? '缓存与缓冲不可得，组成带只画已用与可用两段'
        : (cached === null ? '缓存不可得' : '缓冲不可得') + '，组成带只画已用与可用两段',
    }
  }

  const free = Math.max(0, total - memory.used_bytes - cached - buffers)
  return {
    segments: [
      segment('used', '已用', Math.max(0, total - free - cached - buffers)),
      segment('cached', '缓存', cached),
      segment('buffers', '缓冲', buffers),
      segment('free', '空闲', free),
    ],
    degraded: false,
    degradedText: '',
  }
}

/** 组成带某段占总量比例 → 供视图注入的百分比文本（`formatPercent`）；不可得时为 null */
export function memorySegmentPercent(segment: MemoryCompositionSegment): string | null {
  return segment.ratio === null ? null : formatPercent(Math.min(1, Math.max(0, segment.ratio)))
}
