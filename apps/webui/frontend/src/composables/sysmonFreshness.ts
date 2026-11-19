/**
 * 快照停推判定（#371；术语见 GLOSSARY.md「数据龄」）
 *
 * 判据是**本机收到新快照的时刻**，不是发送方 `snapshot.timestamp`。这不是口味问题：两机时钟
 * 一旦有偏差，用「本机 now − 发送方 timestamp」会在两个方向同时坏掉，而 3 倍阈值给的容差对
 * 几十秒级的偏差毫无帮助。
 * - 发送方钟**落后** Δ：差值恒为 +Δ，数据再新鲜也永远超阈值 → 页面永久「快照已停 N 秒」，
 *   用户很快学会忽略这个提示，比没有更糟。
 * - 发送方钟**超前** Δ：差值恒为 −Δ，永远不到阈值 → 真停推时静默失效，一句谎话说到底。
 *
 * 「多久没收到新快照」只取决于**本机**这口钟：收到就记一次 `Date.now()`，之后与本机此刻作差。
 * 发送方 `timestamp` 仍是快照生成时刻（契约 sysmon §2），继续用于「更新于 …」这类展示，但**不**
 * 参与停推判定。
 *
 * 三条约定：
 * 1. **阈值不硬编码**：降级阈值为 `3 × realtime.push_interval_sec`（契约 sysmon §3.1，
 *    默认 5 秒 → 15 秒）。取 3 倍是给正常抖动留余量：WS 重连、事件通道唤醒延迟都能占掉一两个
 *    周期，2 倍会把一次正常的晚点报成停推。
 * 2. **不知道就不判**：还没收到过任何快照（首次挂载的短暂窗口）返回 null，而不是「从此刻开始
 *    计时」——后者会把「不知道」写成「刚刚收到」。
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
 * 距上次收到新快照的时长（毫秒）。
 *
 * @param receivedAtMs 本机收到该快照的时刻（store 写入快照时记下的 `Date.now()`）；
 *        null = 还没有任何接收记录 → 返回 null（约定 2：不拿「不知道」冒充「刚刚」）
 * @param nowMs 本机此刻
 *
 * **时钟倒流返回 null**（`now < receivedAt`）：两个数出自同一口本机钟，它们互相矛盾说明这口钟
 * 此刻不可信，于是既不断言停推、也不断言新鲜。钳 0 会把「不可信」写成「刚刚收到」，与本模块
 * 「不用 0 冒充刚生成」是同一笔账；判停推则会把时钟回拨说成数据停了，两个都是无中生有的断言。
 */
export function receivedAgeMs(receivedAtMs: number | null, nowMs: number): number | null {
  if (typeof receivedAtMs !== 'number' || !Number.isFinite(receivedAtMs) || receivedAtMs <= 0) {
    return null
  }
  if (typeof nowMs !== 'number' || !Number.isFinite(nowMs)) return null
  const ageMs = nowMs - receivedAtMs
  if (ageMs < 0) return null
  return ageMs
}

/**
 * 是否停推：距上次收到新快照的时长超过降级阈值（3 × push_interval_sec）。
 *
 * 边界取严格大于：正好等于 3 倍不算停推（阈值是「超过多久」而不是「到了多久」）。
 * null（还没有接收记录 / 时钟倒流）一律**不判**停推——不知道就不说。
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
