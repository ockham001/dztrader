/**
 * 快照数据龄与停推降级（#371；契约 sysmon §2 快照 / §4 推送场景）
 *
 * 「数据龄」的术语定义见 GLOSSARY.md：**某个实时快照距此刻的时长**，由快照自带 `timestamp`
 * 与本机此刻作差得到。它与 WebSocket 连接状态是两个独立事实——连接正常但发送方停推时，
 * 连接状态仍是「已连接」而数据龄持续增长。因此本模块**不读**连接状态：「连接断开」由
 * AppShell 的全页横幅承担（那是另一件事），这里只回答「我看的数据有多旧」。
 *
 * 三条约定：
 * 1. **阈值不硬编码**：降级阈值为 `3 × realtime.push_interval_sec`（契约 sysmon §3.1，
 *    默认 5 秒 → 15 秒）。取 3 倍而不是 2 倍，是给正常的推送抖动留余量：WS 重连、事件通道
 *    唤醒延迟都能占掉一两个周期，2 倍会把一次正常的晚点报成停推。
 * 2. **负值不判停推**：发送方时间戳比本机快时差值为负（两机时钟偏差），此时既不判停推、
 *    也不显示负数——「时钟不准」不是「数据停了」。
 * 3. **文案复用 formatAge**：「快照已停 18 秒 / 2 分钟 / 3 小时」与页面上其它「多久以前」
 *    共用同一份时长格式化（`format.ts` 是唯一实现），不另写一套取整口径。
 *
 * 纯函数模块：不 import vue / pinia / store，可脱离组件单测（本仓「谁能单测」的边界）。
 */
import { formatAge } from '@/composables/format'

/** 降级阈值 = 推送间隔的倍数（#331 triage 结论：3 倍，默认 5s → 15s） */
export const SNAPSHOT_STALE_PUSH_MULTIPLE = 3

/** 契约 sysmon §3.1 `realtime.push_interval_sec` 的默认值（配置镜像未到达时的兜底） */
export const DEFAULT_PUSH_INTERVAL_SEC = 5

/** 契约 sysmon §3.1 的取值范围；越界值按边界收窄，不让一个坏字段把阈值变成 0 或无穷 */
const PUSH_INTERVAL_MIN_SEC = 1
const PUSH_INTERVAL_MAX_SEC = 3600

/**
 * 解析生效推送间隔（契约 sysmon §3.1 `realtime` 段）。
 *
 * 配置镜像尚未到达（`config === null`）、字段缺失或非法（非有限正数）时回退契约默认值 5 秒：
 * 降级判定不能等配置到达才成立——停推最可能发生在任何配置都没变的时候。
 * 超过 `[1, 3600]` 的值按边界收窄（发送方自己也会按这个区间校验）。
 */
export function resolvePushIntervalSec(
  realtime: { push_interval_sec?: unknown } | null | undefined,
): number {
  const raw = realtime?.push_interval_sec
  if (typeof raw !== 'number' || !Number.isFinite(raw) || raw <= 0) return DEFAULT_PUSH_INTERVAL_SEC
  return Math.min(Math.max(raw, PUSH_INTERVAL_MIN_SEC), PUSH_INTERVAL_MAX_SEC)
}

/** 降级阈值（毫秒）= 3 × 推送间隔；入参同样按契约区间收窄 */
export function staleThresholdMs(pushIntervalSec: number): number {
  return resolvePushIntervalSec({ push_interval_sec: pushIntervalSec })
    * SNAPSHOT_STALE_PUSH_MULTIPLE
    * 1_000
}

/**
 * 数据龄（毫秒）。快照无 `timestamp`（缺失 / 非有限 / ≤ 0 = 不是一次有效的生成时刻）时返回
 * null —— 不拿 0 冒充「刚刚生成」，也不拿本机墙钟冒充发送方的时间戳。
 *
 * 返回值可为**负**：发送方时间戳比本机快（两机时钟偏差）时就是负的，由调用方按约定 2 处理。
 */
export function snapshotAgeMs(snapshotTimestampMs: number, nowMs: number): number | null {
  if (typeof snapshotTimestampMs !== 'number'
    || !Number.isFinite(snapshotTimestampMs)
    || snapshotTimestampMs <= 0) {
    return null
  }
  if (typeof nowMs !== 'number' || !Number.isFinite(nowMs)) return null
  return nowMs - snapshotTimestampMs
}

/**
 * 数据龄是否超过降级阈值（3 × push_interval_sec）。
 *
 * 边界取严格大于：正好等于 3 倍不算停推（阈值是「超过多久」而不是「到了多久」）。
 * 无 timestamp（null）与负值（时钟偏差）一律**不判**停推——见模块头约定 2。
 */
export function isSnapshotStale(ageMs: number | null, pushIntervalSec: number): boolean {
  if (ageMs === null || !Number.isFinite(ageMs)) return false
  if (ageMs <= 0) return false
  return ageMs > staleThresholdMs(pushIntervalSec)
}

/**
 * 降级文案（如「快照已停 18 秒」）。未超阈值返回 null（调用方据此保留「实时快照」）。
 *
 * 秒数向下取整（与 `formatAge` 一致）：数据龄 18.9 秒显示「18 秒」——向上取整会把 17.1 秒
 * 报成 18 秒，读者会以为比实际更久。
 */
export function staleSnapshotText(ageMs: number | null, pushIntervalSec: number): string | null {
  if (ageMs === null || !isSnapshotStale(ageMs, pushIntervalSec)) return null
  return `快照已停 ${formatAge(Math.floor(ageMs / 1_000))}`
}
