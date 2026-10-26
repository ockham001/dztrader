import type { SysmonHistoryPoint } from '@/types/api'

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

/** SVG 折线几何：`points` 可直接给 `<polyline points="...">`，`lastValue` 供末点数值断言 */
export interface SparklineGeometry {
  points: string
  lastValue: number
}

/**
 * 数据点 → SVG 折线（viewBox `0 0 100 24`，按序号等距）。
 *
 * 按序号而非时间戳取 x：sparkline 用来看形状，缺口不该在迷你图上变成一段空白宽度；
 * 缺口/中断的解释由历史视图（包络 + 缺口底纹）承担。值域压到 [2, 22]，等值序列画中线
 * （不除零）。有效点少于 2 个时返回 null —— 一个点画不出趋势，调用方据此不渲染。
 */
export function sparklineGeometry(series: SparklinePoint[]): SparklineGeometry | null {
  const valid = series.filter(point => isFiniteNumber(point?.ts) && isFiniteNumber(point?.value))
  if (valid.length < 2) return null

  const values = valid.map(point => point.value)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const span = max - min
  const round2 = (value: number): number => Math.round(value * 100) / 100
  const width = valid.length - 1

  const coordinates = valid.map((point, index) => {
    const x = (index / width) * SVG_WIDTH
    const ratio = span === 0 ? 0.5 : (point.value - min) / span
    const y = PLOT_PAD + PLOT_HEIGHT * (1 - ratio)
    return `${round2(x)},${round2(y)}`
  })

  return {
    points: coordinates.join(' '),
    lastValue: valid[valid.length - 1].value,
  }
}
