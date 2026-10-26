import type { SysmonHistoryPoint } from '@/types/api'
import { formatDuration } from '@/composables/format'

/**
 * 内联趋势（sparkline）的数据整形（#127）。
 *
 * 数据源与实时数值是**同一份事实**：历史 REST 补齐最近 15 分钟的桶，之后每个实时快照
 * 追加一个点。因此末点恒等于旁边的当前值（不是另一次采样）——这是验收标准里
 * 「末点必须等于旁边的当前值」的结构性保证，不靠两路数据偶然一致。
 *
 * 本模块只放纯函数（整形 / 增量推进 / SVG 几何），便于不依赖真实时钟与 DOM 的确定性测试；
 * 取数与实时追加在 `stores/sysmonTrend.ts`。
 */

/** 一条 sparkline 数据点：Unix 秒 + 指标原始值（比率 / 负载 / 字节，与实时快照同一单位） */
export interface SparklinePoint {
  ts: number
  value: number
}

/** 时间窗：最近 15 分钟（验收标准；与「刚才发生了什么」这个注视时长匹配） */
export const SPARKLINE_WINDOW_SEC = 15 * 60

/** 点数上限（与历史查询的 `max_points` 同值，保证「REST 补齐 + WS 推进」后仍不超过它） */
export const SPARKLINE_MAX_POINTS = 60

/**
 * 实时点的桶宽（秒）= 时间窗 / 点数上限 = 15 秒。
 *
 * 实时推送间隔默认 5 秒，若每个推送都追加一个点，60 点上限会把 15 分钟窗口压缩成 5 分钟
 * （窗口被点数上限挤掉）。按桶宽对齐：同一桶内只**替换**末点的值（末点仍等于当前值），
 * 跨桶才追加，于是 15 分钟窗口内点数恒 ≤ 60，时间窗不被点数上限侵蚀。
 */
export const SPARKLINE_BUCKET_SEC = SPARKLINE_WINDOW_SEC / SPARKLINE_MAX_POINTS

/** SVG viewBox 尺寸：横向 100 单位由 CSS 拉伸到容器宽度，纵向 24 单位固定（sparkline 是辅助，不抢首屏） */
const SVG_WIDTH = 100
const SVG_HEIGHT = 24
/** 上下留白：给折线留出边框，避免贴边被裁 */
const PLOT_PAD = 2
const PLOT_HEIGHT = SVG_HEIGHT - PLOT_PAD * 2

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value)
}

/**
 * 历史桶 → sparkline 点：取桶内均值作为趋势值（`avg` 缺失时回退 `last`）。
 * 该桶无有效样本时四值为 null（契约 sysmon §6.1），跳过而不是用 0 冒充；
 * 结果按 `ts` 升序（接口契约已升序，这里仍排序以消除消费方对发送顺序的依赖）。
 */
export function historySparklinePoints(points: SysmonHistoryPoint[]): SparklinePoint[] {
  const result: SparklinePoint[] = []
  for (const point of points) {
    if (!isFiniteNumber(point?.ts)) continue
    const value = isFiniteNumber(point.avg) ? point.avg : (isFiniteNumber(point.last) ? point.last : null)
    if (value === null) continue
    result.push({ ts: point.ts, value })
  }
  return result.sort((a, b) => a.ts - b.ts)
}

/**
 * 追加一个实时点（WS 增量推进）：同一时刻替换末点、更旧的点忽略、时间窗外的旧点丢弃、
 * 超过点数上限时丢最旧的。纯函数——返回新数组，不修改入参，便于视图层直接替换引用触发重渲染。
 */
export function appendSparklinePoint(
  series: SparklinePoint[],
  point: SparklinePoint,
  windowSec: number = SPARKLINE_WINDOW_SEC,
  maxPoints: number = SPARKLINE_MAX_POINTS,
): SparklinePoint[] {
  if (!isFiniteNumber(point?.ts) || !isFiniteNumber(point?.value)) return series
  const last = series[series.length - 1]
  if (last) {
    // 时钟回退 / 乱序推送：忽略，不让折线折返
    if (point.ts < last.ts) return series
    // 同一秒内的多次推送：替换末点，不堆积（推送间隔远小于 1 秒的场景）
    if (point.ts === last.ts) return [...series.slice(0, -1), point]
  }
  const next = [...series, point].filter(entry => entry.ts >= point.ts - windowSec)
  return next.length > maxPoints ? next.slice(next.length - maxPoints) : next
}

/**
 * 实时推进（WS 增量）：保证**末点恒为当前实时值**，再按时间窗与点数上限收敛。
 *
 * 与 `appendSparklinePoint` 的区别只在「实时桶的时刻不晚于末点」这一情形：契约 §6.1 的历史桶
 * 是 `[since, until)` 的桶起点，可能比 5 秒前的快照时间戳更靠右（桶边界对齐、快照时间戳滞后、
 * 时钟抖动）；此时若按「更旧的点忽略」处理，末点会落在历史桶值上、与旁边的当前数字不一致，
 * 直到下一个实时桶（最长 15 秒）。本函数改为**替换末点的值**（保留末点时刻，横轴单调），
 * 让「末点 == 当前值」在任何时序下都成立（#127 AC）。
 */
export function upsertLiveSparklinePoint(
  series: SparklinePoint[],
  point: SparklinePoint,
  windowSec: number = SPARKLINE_WINDOW_SEC,
  maxPoints: number = SPARKLINE_MAX_POINTS,
): SparklinePoint[] {
  if (!isFiniteNumber(point?.ts) || !isFiniteNumber(point?.value)) return series
  const last = series[series.length - 1]
  if (!last) return [point]
  if (point.ts > last.ts) return appendSparklinePoint(series, point, windowSec, maxPoints)
  // 实时桶不晚于末点（含同一刻）：只更新值，末点位置不变
  return [...series.slice(0, -1), { ts: last.ts, value: point.value }]
}

/**
 * 历史补拉响应里与时间窗口径相关的元信息（#142）。
 *
 * `intervalSec = null` = 范围内没有任何桶（契约 sysmon §6.1）：此时 `from`/`to` 只是请求范围，
 * 不能拿来声称窗口已覆盖——窗口只能按实时点跨度计。
 */
export interface SparklineHistoryWindow {
  from: number
  to: number
  truncated: boolean
  intervalSec: number | null
}

/**
 * sparkline 时间窗的如实口径（#142）。
 *
 * #127 的实现无论实际数据多少都把标题写成「最近 15 分钟」，三种边界下会骗人：历史可用但
 * 范围内无桶（实际只有几十秒）、`record_interval_sec < 15` 时 `max_points=60` 被截断（实际
 * 只有 `60 × interval` 秒）、单指标补拉降级（序列沿用上一轮）。视图据本口径如实标注。
 */
export interface SparklineWindow {
  /** 实际覆盖秒数；无点（不渲染）时为 null */
  coveredSec: number | null
  /** true = 已覆盖完整时间窗（title 可称「最近 15 分钟」） */
  complete: boolean
  /** true = 服务端因 `max_points` 截断了窗口（契约 sysmon §6.1.2），呈现与历史视图同口径 */
  truncated: boolean
  /** true = 本轮历史补拉失败/降级，序列沿用上一轮（#142） */
  stale: boolean
}

/**
 * 计算时间窗的实际覆盖口径（#142）。
 *
 * - 历史有桶时以**生效窗口**为准（契约 §6.1.2：截断时 `from` 已前移为首个返回桶）：
 *   起点取生效 `from`；但历史刚启用时首个桶可能晚于 `from` 一个桶宽以上，此时按首个桶
 *   起点计（数据不足 15 分钟不得冒充完整）。末端取 `to` 与末点中较大者——页面停留期间
 *   实时点会推进到查询时刻之后。
 * - 历史范围内无桶（`intervalSec = null`）或从未拿到历史窗口时，只能按**实时点跨度**
 *   标注：这正是「新装机器 / 刚启用历史」的实际可见范围。
 *
 * 判定容忍一个桶宽：桶按墙钟对齐，正常连续历史的第一个桶可以晚于 `from` 但不超过一个
 * 桶宽，这属于桶粒度而非窗口缺失。
 */
export function sparklineWindow(
  points: readonly SparklinePoint[],
  history: SparklineHistoryWindow | null,
  stale = false,
): SparklineWindow {
  const first = points[0]
  const last = points[points.length - 1]
  if (!first || !last) {
    return { coveredSec: null, complete: false, truncated: history?.truncated === true, stale }
  }
  let coveredSec: number
  if (history && history.intervalSec !== null) {
    const dataStart = first.ts - history.from > history.intervalSec ? first.ts : history.from
    coveredSec = Math.max(history.to, last.ts) - dataStart
  } else {
    coveredSec = last.ts - first.ts
  }
  coveredSec = Math.round(Math.max(0, coveredSec))
  return {
    coveredSec,
    complete: coveredSec >= SPARKLINE_WINDOW_SEC,
    truncated: history?.truncated === true,
    stale,
  }
}

/**
 * 历史补拉序列与上一轮序列合并（#142）。
 *
 * 补拉替换的是历史覆盖的时段，历史末桶之后已推进的实时点必须保留（否则每次进入页面都会
 * 丢掉最近一个桶的实时趋势）。**历史范围内无桶时保留上一轮序列**：历史刚启用时不能把已经
 * 攒下的实时趋势清空（本轮补拉只是确认历史还没有数据）。收敛口径与实时推进一致。
 */
export function mergeSparklinePoints(
  history: readonly SparklinePoint[],
  previous: readonly SparklinePoint[],
): SparklinePoint[] {
  if (history.length === 0) return [...previous]
  const lastHistoryTs = history[history.length - 1].ts
  const merged = [...history, ...previous.filter(point => point.ts > lastHistoryTs)]
  const last = merged[merged.length - 1]
  const inWindow = merged.filter(point => point.ts >= last.ts - SPARKLINE_WINDOW_SEC)
  return inWindow.length > SPARKLINE_MAX_POINTS
    ? inWindow.slice(inWindow.length - SPARKLINE_MAX_POINTS)
    : inWindow
}

/** 时间窗文案：只有覆盖满整个窗口才称「最近 15 分钟」，否则用实际覆盖时长（#142） */
export function sparklineWindowText(window: SparklineWindow): string {
  if (window.coveredSec === null) return '时间窗未知'
  if (window.complete) return `最近 ${formatDuration(SPARKLINE_WINDOW_SEC)}`
  return `最近 ${formatDuration(window.coveredSec)}`
}

/** 窗口口径的限定说明（截断 / 历史未刷新）；无修饰时为空数组 */
function sparklineWindowNotes(window: SparklineWindow): string[] {
  const notes: string[] = []
  if (window.truncated) notes.push('点数超限，仅显示最近一段')
  if (window.stale) notes.push('历史未刷新')
  return notes
}

/** 组件 title：指标名 · 时间窗（限定说明；点击查看历史） */
export function sparklineTitle(label: string, window: SparklineWindow): string {
  const notes = [...sparklineWindowNotes(window), '点击查看历史']
  return `${label} · ${sparklineWindowText(window)}（${notes.join('；')}）`
}

/** 组件 aria-label：指标名 + 时间窗 + 限定说明 + 查看历史（无障碍文案不重复视觉细节） */
export function sparklineAriaLabel(label: string, window: SparklineWindow): string {
  const notes = sparklineWindowNotes(window)
  const suffix = notes.length > 0 ? `（${notes.join('；')}）` : ''
  return `${label} ${sparklineWindowText(window)}趋势${suffix}，查看历史`
}

/** SVG 折线几何：`points` 可直接给 `<polyline points="...">`，`lastValue` 供末点数值断言 */
export interface SparklineGeometry {
  points: string
  lastValue: number
}

/**
 * 数据点 → SVG 折线（viewBox `0 0 100 24`）。
 *
 * x 按**时间比例**：序列是「历史 60 秒桶 + 实时 15 秒桶」的混合桶距，按序号等距会把最近几分钟
 * 拉伸、让「最近 15 分钟」在前 15 分钟失真（#127 审查发现）。按时间取 x 后，桶距不均与采集缺口
 * 都如实反映为横向疏密。时间跨度为零（重复时刻，理论上不应出现）时退回序号等距，避免除零。
 *
 * 值域压到 [2, 22]，等值序列画中线。有效点少于 2 个时返回 null —— 一个点画不出趋势，
 * 调用方据此不渲染。
 */
export function sparklineGeometry(series: SparklinePoint[]): SparklineGeometry | null {
  const valid = series.filter(point => isFiniteNumber(point?.ts) && isFiniteNumber(point?.value))
  if (valid.length < 2) return null

  const values = valid.map(point => point.value)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const span = max - min
  const round2 = (value: number): number => Math.round(value * 100) / 100
  const firstTs = valid[0].ts
  const timeSpan = valid[valid.length - 1].ts - firstTs
  const indexWidth = valid.length - 1

  const coordinates = valid.map((point, index) => {
    const x = timeSpan > 0
      ? ((point.ts - firstTs) / timeSpan) * SVG_WIDTH
      : (index / indexWidth) * SVG_WIDTH
    const ratio = span === 0 ? 0.5 : (point.value - min) / span
    const y = PLOT_PAD + PLOT_HEIGHT * (1 - ratio)
    return `${round2(x)},${round2(y)}`
  })

  return {
    points: coordinates.join(' '),
    lastValue: valid[valid.length - 1].value,
  }
}
