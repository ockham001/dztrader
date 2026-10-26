import { defineStore } from 'pinia'
import { ref, watch } from 'vue'
import { sysmonApi } from '@/api/sysmon'
import {
  SPARKLINE_BUCKET_SEC,
  SPARKLINE_MAX_POINTS,
  SPARKLINE_WINDOW_SEC,
  appendSparklinePoint,
  historySparklinePoints,
  type SparklinePoint,
} from '@/composables/sysmonSparkline'
import { useSysmonStore } from '@/stores/sysmon'

/**
 * 内联趋势（sparkline）覆盖的标量指标（#127，验收标准）。
 *
 * 只列**标量指标**：它们无需维度即可查询（契约 sysmon §6.1 的 `metric` 参数），与历史视图
 * 下拉同一真相源口径。维度指标（每核 / 每盘 / 每网卡 / 每传感器）第一版不做 sparkline ——
 * 维度查询需要 `dimension` 参数，而维度选择器不在本版本范围。
 *
 * `key` 必须与 `libs/sysmonstore` 的 `kScalarMetrics[].key` 一致，否则后端回 400。
 */
export interface SysmonTrendMetric {
  /** 历史库指标键（与 `stores/sysmonHistory.ts` 的 `SYSMON_HISTORY_METRICS` 同源命名） */
  key: string
  /** 展示名（sparkline 的 title / aria-label） */
  label: string
}

export const SYSMON_TREND_METRICS: SysmonTrendMetric[] = [
  { key: 'cpu.usage_ratio', label: 'CPU 使用率' },
  { key: 'cpu.load_average.one', label: '系统负载（1 分钟）' },
  { key: 'memory.used_ratio', label: '内存使用率' },
  { key: 'swap.used_ratio', label: '交换使用率' },
  { key: 'shm_dir.total_bytes', label: '共享内存目录占用' },
]

/**
 * 系统监控内联趋势 store（#127）。
 *
 * 数据源（验收标准「不新增后端接口」）：进入监控页时按 `max_points=60` 拉取最近 15 分钟的
 * 历史序列补齐，之后每个实时快照推送追加一个点并丢弃最旧点。历史查询走既有
 * `GET /api/sysmon/history`（契约 sysmon §6.1），不新增帧、不新增 REST。
 *
 * **末点 == 旁边的当前值**是结构性的：追加点取的是与页面数字**同一个** store computed
 * （`cpuUsageRatio` / `memoryInfo.used_ratio` / …），不是另一次采样；历史补拉完成时也立刻
 * 把当前快照接到末点，不存在两路数据偶然一致的问题。
 *
 * **静默降级**：历史库不可用（`available=false`，含历史被关闭的 `not_found`）或 HTTP 失败时
 * 该指标不产生任何序列、也不追加实时点 —— 视图不渲染 sparkline，数值照常显示。降级不是错误：
 * 本 store 不保存错误状态、不 toast。
 */
export const useSysmonTrendStore = defineStore('sysmonTrend', () => {
  const sysmon = useSysmonStore()

  /** 各指标的 sparkline 序列（Unix 秒 + 指标原始值）；历史不可用的指标为空数组 */
  const series = ref<Record<string, SparklinePoint[]>>({})
  /** 历史库对各指标是否可用（false = 静默不渲染；实时追加也据此跳过） */
  const available = ref<Record<string, boolean>>({})
  /** 历史补拉已完成（含降级）；重复 init 不再请求 */
  const loaded = ref(false)
  const loading = ref(false)

  /** 某指标的序列（历史不可用 / 未加载时为**新**空数组，调用方不得缓存引用） */
  function seriesOf(key: string): SparklinePoint[] {
    return series.value[key] ?? []
  }

  /** 某指标的历史是否可用（视图渲染 sparkline 的前置条件；实时值是否可读另由调用方判断） */
  function isAvailable(key: string): boolean {
    return available.value[key] === true
  }

  /** 指标展示名（sparkline 的 title / aria-label；未知键回退空串） */
  function labelOf(key: string): string {
    return SYSMON_TREND_METRICS.find(metric => metric.key === key)?.label ?? ''
  }

  /**
   * 指标的当前实时值 —— 必须与监控页展示的数字取自**同一个** store computed，
   * 这样 sparkline 末点与旁边数字天然一致（验收标准「同源同刷新时刻」）。
   * 值不可用时返回 null（不得用 0 冒充；置 null 的历史视图口径见契约 sysmon §6.1）。
   */
  function liveValueOf(key: string): number | null {
    switch (key) {
      case 'cpu.usage_ratio':
        return sysmon.cpuUsageRatio
      case 'cpu.load_average.one':
        return sysmon.cpuLoadAverage?.one ?? null
      case 'memory.used_ratio':
        return sysmon.memoryInfo?.used_ratio ?? null
      case 'swap.used_ratio':
        return sysmon.swapInfo?.used_ratio ?? null
      case 'shm_dir.total_bytes':
        return sysmon.shmDirInfo?.total_bytes ?? null
      default:
        return null
    }
  }

  /**
   * 把当前快照的实时值接到每个序列末尾（WS 增量推进）。
   *
   * 历史不可用的指标跳过：sparkline 的语义是「历史库补齐 + 实时推进」，没有历史时
   * 只靠实时点堆出的曲线会让用户以为看到了 15 分钟趋势（验收标准要求此时静默不渲染）。
   */
  function appendLive(): void {
    const snapshot = sysmon.snapshot
    if (!snapshot) return
    const ts = Math.floor(snapshot.timestamp / 1000)
    if (!Number.isFinite(ts) || ts <= 0) return
    // 对齐到 15 秒桶：推送间隔（默认 5s）远小于桶宽，同桶内只更新值，窗口不被点数上限压缩
    const bucketTs = Math.floor(ts / SPARKLINE_BUCKET_SEC) * SPARKLINE_BUCKET_SEC

    let next: Record<string, SparklinePoint[]> | null = null
    for (const metric of SYSMON_TREND_METRICS) {
      if (!isAvailable(metric.key)) continue
      const value = liveValueOf(metric.key)
      if (value === null || !Number.isFinite(value)) continue
      next ??= { ...series.value }
      next[metric.key] = appendSparklinePoint(next[metric.key] ?? [], { ts: bucketTs, value })
    }
    if (next) series.value = next
  }

  // 同步 flush：快照写入与序列推进发生在同一个 tick，组件重渲染时两者已经是同一份事实
  watch(() => sysmon.snapshot, () => { appendLive() }, { flush: 'sync' })

  /**
   * 进入监控页时补拉历史（每个标量指标一次）。请求失败/历史不可用都**静默**降级：
   * 不抛错、不 toast、不保存错误状态，视图只是不渲染 sparkline。
   */
  async function init(): Promise<void> {
    if (loading.value || loaded.value) return
    loading.value = true
    const to = Math.floor(Date.now() / 1000)
    const from = to - SPARKLINE_WINDOW_SEC
    try {
      const responses = await Promise.allSettled(
        SYSMON_TREND_METRICS.map(metric => sysmonApi.getHistory({
          metric: metric.key,
          from,
          to,
          max_points: SPARKLINE_MAX_POINTS,
        })),
      )
      const nextSeries: Record<string, SparklinePoint[]> = {}
      const nextAvailable: Record<string, boolean> = {}
      responses.forEach((response, index) => {
        const key = SYSMON_TREND_METRICS[index].key
        // 只有 200 + available=true 才算历史可用；HTTP 失败与降级响应同样静默
        if (response.status !== 'fulfilled' || response.value.available !== true) {
          nextSeries[key] = []
          nextAvailable[key] = false
          return
        }
        nextSeries[key] = historySparklinePoints(response.value.series[0]?.points ?? [])
        nextAvailable[key] = true
      })
      series.value = nextSeries
      available.value = nextAvailable
      loaded.value = true
      // 历史返回时快照可能已经到达：立刻接上当前值，末点不落后于旁边的数字
      appendLive()
    } catch {
      // 任何意外异常都静默降级：sparkline 是辅助信息，不得让监控页因它失败
    } finally {
      loading.value = false
    }
  }

  return {
    seriesOf,
    isAvailable,
    labelOf,
    loaded,
    init,
  }
})
