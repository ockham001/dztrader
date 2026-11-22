/**
 * CPU 块的两条纯规则（#435）：固定 0–100% 的趋势折线，与每核条形的列数。
 *
 * 为什么单独成模块：两条规则都依赖「序列 / 容器宽度」这类输入，而 jsdom 没有布局；抽成
 * 纯函数后「折线怎么投影」「多行时每行几个」可以逐条锁死在单测里，组件只负责测量与渲染
 * （与 `topbarOverflow` 同一先例）。
 *
 * 本模块不读 store、不读 DOM：调用方把已净化的序列与实测宽度传进来。
 */
import type { SparklinePoint } from '@/composables/sysmonSparkline'

/** 趋势绘图区的 viewBox：横向 100 单位由 CSS 拉伸到容器宽度，纵向 34 单位 */
const TREND_SVG_WIDTH = 100
const TREND_SVG_HEIGHT = 34
/** 上下留白：折线不贴上下边框（上下各 2 单位） */
const TREND_PAD = 2
const TREND_PLOT_HEIGHT = TREND_SVG_HEIGHT - TREND_PAD * 2

/**
 * 固定纵轴的网格线 y 值（viewBox 坐标，**自下而上 = 数值递减**）：0% / 25% / 50% / 75% / 100%。
 *
 * SVG 的 y=0 在顶部，因此 0% 的线 y 最大、100% 的线 y 最小。纵轴固定 0–100%（#435 决策 2），
 * 这组线是**静态几何**而不是由序列算出来的刻度；视图只渲染、不参与计算。
 * 读数标签不画（决策 2 明确去掉左侧刻度列），网格线承担「尺度感」。
 */
export const PERCENT_TREND_GRIDLINE_YS: readonly number[] = [
  TREND_PAD + TREND_PLOT_HEIGHT,
  TREND_PAD + TREND_PLOT_HEIGHT * 0.75,
  TREND_PAD + TREND_PLOT_HEIGHT * 0.5,
  TREND_PAD + TREND_PLOT_HEIGHT * 0.25,
  TREND_PAD,
]

/** 固定 0–100% 趋势折线的几何：`points` 可直接给 `<polyline points="...">` */
export interface PercentTrendGeometry {
  points: string
  /** 末点值（与旁边的当前数字同源，供末点断言） */
  lastValue: number
  /** 首个有效点的时刻（Unix 秒）：横轴左端标签取它，而不是原始序列的首项 */
  firstTs: number
  /** 末个有效点的时刻（Unix 秒）：横轴右端标签取它 */
  lastTs: number
}

/**
 * 序列 → 固定 0–100% 纵轴的 SVG 折线（#435）。
 *
 * y 一律按 `value ∈ [0,1]` 映射到整幅高度：**不随序列 min/max 拉伸**（#361 的自适应纵轴对
 * CPU 图作废，理由见 #435 拍板记录 2）。代价是低占用时曲线贴底变平；换来的是跨时间可比、
 * 一眼知道离 100% 还有多远。越界值夹到 [0,1]（契约的 `usage_ratio` 本就在范围内，夹取只
 * 防御异常快照，不让曲线画到轴外）。
 *
 * x 按时间比例（历史桶与实时点桶距不同、采集缺口都如实反映为横向疏密）；时间跨度为零
 * （重复时刻，理论上不应出现）时退回序号等距，避免除零。
 *
 * @param points 序列（Unix 秒 + 比率 0–1）；非有限值的点逐点丢弃
 * @returns 有效点少于 2 个时为 null —— 一个点画不出趋势，调用方据此不渲染
 */
export function percentTrendGeometry(
  points: readonly SparklinePoint[],
): PercentTrendGeometry | null {
  const valid = points.filter(point => Number.isFinite(point?.ts) && Number.isFinite(point?.value))
  if (valid.length < 2) return null

  const firstTs = valid[0].ts
  const timeSpan = valid[valid.length - 1].ts - firstTs
  const indexWidth = valid.length - 1
  const round2 = (value: number): number => Math.round(value * 100) / 100

  const coordinates = valid.map((point, index) => {
    const x = timeSpan > 0
      ? ((point.ts - firstTs) / timeSpan) * TREND_SVG_WIDTH
      : (index / indexWidth) * TREND_SVG_WIDTH
    const clamped = Math.min(1, Math.max(0, point.value))
    const y = TREND_PAD + TREND_PLOT_HEIGHT * (1 - clamped)
    return `${round2(x)},${round2(y)}`
  })

  return {
    points: coordinates.join(' '),
    lastValue: valid[valid.length - 1].value,
    firstTs: valid[0].ts,
    lastTs: valid[valid.length - 1].ts,
  }
}

/**
 * 每核条形的最小格宽（px）：列数在 JS 里算，CSS 不再有对应的网格下限 token
 * （#435 取代 #362 的 auto-fit 规则）。56px 让 1440 视口下 16 核排成一行、32/48 核排成
 * 2–3 行；再窄（<lg 或手机）才继续折行。
 */
export const CORE_BAR_MIN_WIDTH = 56
/**
 * 每核条形的格间距（px）：与间距 token `--spacer-8` 的实际值一致（token 名是步进序号，不是像素值）。
 * 单测会解析 `design-tokens.css` 交叉校验这个常量，防止 JS 与 CSS 各排一份间距。
 */
export const CORE_BAR_GAP = 10
/** 为凑整行最多允许多出来的行数（见 {@link coreBarColumns}） */
const MAX_EXTRA_ROWS = 2

/**
 * 每核条形的列数（#435 决策 6：按容器宽度自适应，多行时每行数量尽量相等）。
 *
 * 规则（从「用户要什么」倒推）：
 * 1. 宽度定上限：`maxColumns = floor((容器宽 + 间距) / (最小格宽 + 间距))`，保证每格不窄于
 *    最小格宽；容器宽为 0（jsdom / 尚未测量）时退回单行，不会把条形误折成多行。
 * 2. 行数先取最少：`minRows = ceil(数量 / maxColumns)`。
 * 3. **尽量等分**：在「行数不超过 `minRows + 2`」的候选列数里，选**末行空格最少**的那个
 *    （空格为 0 即每行数量完全相等）；同空格数时取行数少的。允许最多多 2 行，是为了避免为了
 *    「恰好整除」把 16 核从 3 行拖成 16 行；找不到等分才退回最少行数，由 CSS 网格把余数摊到末行。
 *    在常见核数（8/16/32/48/64/96/128）与常见容器宽度下不会出现 23 + 1 那种极端失衡
 *    （单测矩阵覆盖）；**已知边界**：容器极窄而核数极高时（如 128 核 × 500px），要完全等分
 *    只能把每格压到最小格宽以下或折出 30+ 行，本实现选择「少行 + 末行偏短」，不为此牺牲可读性。
 *
 * @param count 条形总数（核数）；非正数返回 0
 * @param containerWidth 容器实测宽度（px）；未测量（0 / NaN）时返回 `count`（单行）
 * @param minCellWidth 最小格宽（px），默认 {@link CORE_BAR_MIN_WIDTH}
 * @param gap 格间距（px），默认 {@link CORE_BAR_GAP}
 * @returns 列数（恒为正整数；`count` 为 0 时为 0）
 */
export function coreBarColumns(
  count: number,
  containerWidth: number,
  minCellWidth: number = CORE_BAR_MIN_WIDTH,
  gap: number = CORE_BAR_GAP,
): number {
  if (!Number.isFinite(count) || count <= 0) return 0
  const safeGap = Number.isFinite(gap) && gap > 0 ? gap : 0
  const safeMin = Number.isFinite(minCellWidth) && minCellWidth > 0 ? minCellWidth : 1
  if (!Number.isFinite(containerWidth) || containerWidth <= 0) return Math.ceil(count)

  const maxColumns = Math.max(1, Math.floor((containerWidth + safeGap) / (safeMin + safeGap)))
  const minRows = Math.ceil(count / maxColumns)
  const maxRows = minRows + MAX_EXTRA_ROWS
  // 候选列数 = 所有「行数不超过 minRows + 2」的列数；按「末行空格最少」排序，同空格数时行数少者优先。
  // 末行空格 = rows × columns − count：为 0 即每行完全相等（32 → 16 + 16，48 → 16 + 16 + 16）。
  let bestColumns = Math.ceil(count / minRows)
  let bestHoles = Number.POSITIVE_INFINITY
  let bestRows = Number.POSITIVE_INFINITY
  for (let columns = 1; columns <= maxColumns; columns += 1) {
    const rows = Math.ceil(count / columns)
    if (rows > maxRows) continue
    const holes = rows * columns - count
    if (holes < bestHoles || (holes === bestHoles && rows < bestRows)) {
      bestHoles = holes
      bestRows = rows
      bestColumns = columns
    }
  }
  return bestColumns
}
