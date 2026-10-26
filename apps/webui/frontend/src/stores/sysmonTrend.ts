import { defineStore } from 'pinia'
import { ref, watch } from 'vue'
import { sysmonApi } from '@/api/sysmon'
import {
  SPARKLINE_BUCKET_SEC,
  SPARKLINE_MAX_POINTS,
  SPARKLINE_WINDOW_SEC,
  historySparklinePoints,
  mergeSparklinePoints,
  sparklineWindow,
  upsertLiveSparklinePoint,
  type SparklineHistoryWindow,
  type SparklinePoint,
  type SparklineWindow,
} from '@/composables/sysmonSparkline'
import { SYSMON_HISTORY_METRICS } from '@/stores/sysmonHistory'
import { useSysmonStore } from '@/stores/sysmon'

/**
 * 内联趋势（sparkline）覆盖的标量指标（#127，验收标准）。
 *
 * 只列**标量指标**：它们无需维度即可查询（契约 sysmon §6.1 的 `metric` 参数），是历史视图
 * 指标下拉的子集。维度指标（每核 / 每盘 / 每网卡 / 每传感器）第一版不做 sparkline ——
 * 维度查询需要 `dimension` 参数，而维度选择器不在本版本范围。
 */
export const SYSMON_TREND_METRIC_KEYS = [
  'cpu.usage_ratio',
  'cpu.load_average.one',
  'memory.used_ratio',
  'swap.used_ratio',
  'shm_dir.total_bytes',
] as const

export type SysmonTrendMetricKey = typeof SYSMON_TREND_METRIC_KEYS[number]

/**
 * 指标的展示名取自 `SYSMON_HISTORY_METRICS`（历史视图下拉是这批指标键的唯一 UI 真相源），
 * 不在本模块复制一份 label——两处各写一份必然在改名时漂移（review-checklist §2.4）。
 * 未注册的键回退显示键本身（让遗漏在界面上可见，而不是静默空标题）。
 */
export function trendMetricLabel(key: string): string {
  return SYSMON_HISTORY_METRICS.find(metric => metric.key === key)?.label ?? key
}

/**
 * 单指标的内联趋势状态（#142）。
 *
 * `history` 只记录**最近一次成功**响应的窗口元信息；补拉失败/降级时不清空它，窗口口径
 * 因此保持如常，另以 `stale` 标记「本轮未刷新」。
 */
interface TrendSeriesState {
  /** sparkline 序列（Unix 秒 + 指标原始值）；不可用且无可保留序列时为空数组 */
  points: SparklinePoint[]
  /** 最近一次成功历史响应的窗口元信息；`null` = 从未成功补拉 */
  history: SparklineHistoryWindow | null
  /** 是否有可用序列（历史可用或保留了上一轮）；false = 静默不渲染、实时推进也跳过 */
  hasSeries: boolean
  /** 最近一次补拉失败/降级但保留了上一轮序列（#142：不清空，标记未刷新） */
  stale: boolean
}

const EMPTY_STATE: TrendSeriesState = {
  points: [],
  history: null,
  hasSeries: false,
  stale: false,
}

/**
 * 系统监控内联趋势 store（#127）。
 *
 * 数据源（验收标准「不新增后端接口」）：**每次进入监控页**按 `max_points=60` 拉取最近 15 分钟的
 * 历史序列补齐，之后每个实时快照推送推进一个点。历史查询走既有 `GET /api/sysmon/history`
 * （契约 sysmon §6.1），不新增帧、不新增 REST、不改快照形状。
 *
 * **末点 == 旁边的当前值**是结构性的：推进点取的是与页面数字**同一个** store computed
 * （`cpuUsageRatio` / `memoryInfo.used_ratio` / …），不是另一次采样；且 `upsertLiveSparklinePoint`
 * 在「历史末桶比实时桶更靠右」时仍把末点值更新为当前值，任何时序下都成立。
 *
 * **静默降级**：历史库从未可用（`available=false`，含历史被关闭的 `not_found`）或**首次**补拉
 * HTTP 失败时，该指标不产生任何序列、也不推进实时点 —— 视图不渲染 sparkline，数值照常显示。
 * 降级不是错误：本 store 不保存错误状态、不 toast。**每次进入页面都会重试补拉**：历史库可能在
 * 进程运行期间才建立（首次进入时 `not_found` 是常见状态），一次失败不应让整个 SPA 会话失去
 * sparkline。
 *
 * **降级保留（#142）**：若该指标上一轮已有可用序列，本次补拉失败/降级**不清空它**——保留曲线
 * 并标记 `stale`，实时推进继续（否则一次瞬时失败会让 sparkline 闪一下消失、再等下一次进入
 * 页面才回来）。首次进入即不可用时没有可保留的数据，仍按「静默不渲染」处理。
 */
export const useSysmonTrendStore = defineStore('sysmonTrend', () => {
  const sysmon = useSysmonStore()

  /**
   * 指标键 → 当前实时值：与监控页展示的数字取自**同一个** store computed，这样 sparkline 末点
   * 与旁边数字天然一致（验收标准「同源同刷新时刻」）。值不可用时返回 null（不得用 0 冒充）。
   */
  const readers: Record<SysmonTrendMetricKey, () => number | null> = {
    'cpu.usage_ratio': () => sysmon.cpuUsageRatio,
    'cpu.load_average.one': () => sysmon.cpuLoadAverage?.one ?? null,
    'memory.used_ratio': () => sysmon.memoryInfo?.used_ratio ?? null,
    'swap.used_ratio': () => sysmon.swapInfo?.used_ratio ?? null,
    'shm_dir.total_bytes': () => sysmon.shmDirInfo?.total_bytes ?? null,
  }

  /** 各指标的序列状态（#142：序列 + 窗口元信息 + 降级保留标记） */
  const states = ref<Record<string, TrendSeriesState>>({})
  /** 历史补拉的 in-flight promise：并发调用共享同一次请求（完成后置空，下次进入页面重新补拉） */
  let inFlight: Promise<void> | null = null

  function stateOf(key: string): TrendSeriesState {
    return states.value[key] ?? EMPTY_STATE
  }

  /** 某指标的序列（历史从未可用 / 未加载时为空数组） */
  function seriesOf(key: string): SparklinePoint[] {
    return stateOf(key).points
  }

  /**
   * 某指标时间窗的如实口径（#142）：完整覆盖 / 实际覆盖时长 / 是否被 `max_points` 截断 /
   * 是否本轮未刷新。组件据它决定标题文案（不再无条件写「最近 15 分钟」）。
   */
  function windowOf(key: string): SparklineWindow {
    const state = stateOf(key)
    return sparklineWindow(state.points, state.history, state.stale)
  }

  /**
   * 把当前快照的实时值推进到每个序列（WS 增量）。
   *
   * 对齐到 15 秒桶（`SPARKLINE_BUCKET_SEC`）：推送间隔默认 5 秒，若每个推送都追加一个点，
   * 60 点上限会把 15 分钟窗口压缩成 5 分钟。同桶内只更新末点值（`upsertLiveSparklinePoint`
   * 保证末点 == 当前值），跨桶才追加。
   *
   * 无可用序列的指标跳过：sparkline 的语义是「历史库补齐 + 实时推进」，没有历史时只靠实时点
   * 堆出的曲线会让用户以为看到了 15 分钟趋势（验收标准要求此时静默不渲染）。降级保留的序列
   * 仍继续推进（#142：上一轮可用即算有序列）。
   *
   * watcher 常驻（应用级 store）：离开监控页后仍持续推进，下次进入页面时已有最近趋势；
   * 每次进入页面还会重新补拉历史（见 `init`），因此离开再回来不会看到过期窗口。
   */
  function appendLive(): void {
    const snapshot = sysmon.snapshot
    if (!snapshot) return
    const ts = Math.floor(snapshot.timestamp / 1000)
    if (!Number.isFinite(ts) || ts <= 0) return
    const bucketTs = Math.floor(ts / SPARKLINE_BUCKET_SEC) * SPARKLINE_BUCKET_SEC

    let next: Record<string, TrendSeriesState> | null = null
    for (const key of SYSMON_TREND_METRIC_KEYS) {
      const state = stateOf(key)
      if (!state.hasSeries) continue
      const value = readers[key]()
      if (value === null || !Number.isFinite(value)) continue
      next ??= { ...states.value }
      next[key] = {
        ...state,
        points: upsertLiveSparklinePoint(state.points, { ts: bucketTs, value }),
      }
    }
    if (next) states.value = next
  }

  // 同步 flush：快照写入与序列推进发生在同一个 tick，组件重渲染时两者已经是同一份事实
  watch(() => sysmon.snapshot, () => { appendLive() }, { flush: 'sync' })

  /**
   * 进入监控页时补拉历史（每个标量指标一次）。请求失败/历史不可用都**静默**降级：
   * 不抛错、不 toast、不保存错误状态；**首次进入即失败**时视图只是不渲染 sparkline，
   * 已有可用序列时保留并标记未刷新（见上「降级保留」）。
   * 并发调用共享同一次 in-flight 请求（第二个调用者等待它完成）；完成后可再次调用
   *（离开页面再回来会重试，历史库可能在运行期才建立）。
   */
  async function init(): Promise<void> {
    if (inFlight) return inFlight
    inFlight = loadHistory().finally(() => { inFlight = null })
    return inFlight
  }

  /** 补拉一次历史（`init` 的实体；失败静默降级，见上） */
  async function loadHistory(): Promise<void> {
    const to = Math.floor(Date.now() / 1000)
    const from = to - SPARKLINE_WINDOW_SEC
    try {
      const responses = await Promise.allSettled(
        SYSMON_TREND_METRIC_KEYS.map(metric => sysmonApi.getHistory({
          metric,
          from,
          to,
          max_points: SPARKLINE_MAX_POINTS,
        })),
      )
      const next: Record<string, TrendSeriesState> = { ...states.value }
      responses.forEach((response, index) => {
        const key = SYSMON_TREND_METRIC_KEYS[index]
        const previous = next[key] ?? EMPTY_STATE
        // 失败 / 降级：**不清空**上一轮可用序列（#142），保留并标记未刷新。
        // 首次进入即失败时没有可保留的数据，保持空序列（静默降级，不渲染）。
        if (response.status !== 'fulfilled' || response.value.available !== true) {
          next[key] = { ...previous, stale: previous.hasSeries }
          return
        }
        const value = response.value
        const historyPoints = historySparklinePoints(value.series[0]?.points ?? [])
        next[key] = {
          // 已经推进的实时点不能因补拉丢失；历史范围内无桶时上一轮序列整体保留
          points: mergeSparklinePoints(historyPoints, previous.points),
          history: {
            from: value.from,
            to: value.to,
            truncated: value.truncated,
            intervalSec: value.interval_sec,
            // 首桶时刻决定窗口起点口径（历史刚启用 / 记录间隔变更时首点可能是保留的实时点或上一轮旧桶）
            firstBucketTs: historyPoints[0]?.ts ?? null,
          },
          hasSeries: true,
          stale: false,
        }
      })
      states.value = next
      // 历史返回时快照可能已经到达：立刻接上当前值，末点不落后于旁边的数字
      appendLive()
    } catch {
      // 任何意外异常都静默降级：sparkline 是辅助信息，不得让监控页因它失败
    }
  }

  return {
    seriesOf,
    windowOf,
    init,
  }
})
