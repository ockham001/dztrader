import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { sysmonApi } from '@/api/sysmon'
import type {
  SysmonHistoryGap,
  SysmonHistoryLevel,
  SysmonHistoryMark,
  SysmonHistoryPoint,
  SysmonHistoryResponse,
  SysmonHistoryUnavailableReason,
} from '@/types/api'

/**
 * 历史视图的指标选项（《帧契约：系统监控》§6.1 的 metric 参数）。
 *
 * 只列宽表逻辑键：它们无需维度即可查询，是历史视图的下拉内容。维度指标（每核 / 每卷 /
 * 每盘 / 每网卡 / 每传感器）需要先选定维度，本版本不提供维度选择器（每次查询单序列），
 * 因此不进下拉——需要时先显式请求 `dimension`。
 *
 * `key` 必须与 `libs/sysmonstore` 的 `kScalarMetrics[].key` 一致，否则后端回 400
 * （《帧契约：系统监控》§6.1「参数非法」），这是前后端契约的一致性要求。
 */
export interface SysmonHistoryMetricOption {
  key: string
  label: string
  /** 值的展示形态：比率按百分比、字节按容量、计数取整带千分位、其余按数值 */
  format: SysmonHistoryMetricFormat
}

/**
 * 历史指标的展示形态（#126）。
 *
 * `count` 与 `number` 分开是必要的：AC3 要求「计数取整且带千分位」，而负载/速率要固定小数位，
 * 同一个 `number` 档无法同时满足两者（页文件数与系统负载会落到同一条格式化路径上）。
 */
export type SysmonHistoryMetricFormat = 'ratio' | 'bytes' | 'count' | 'number'

export const SYSMON_HISTORY_METRICS: SysmonHistoryMetricOption[] = [
  { key: 'cpu.usage_ratio', label: 'CPU 使用率', format: 'ratio' },
  { key: 'cpu.load_average.one', label: '系统负载（1 分钟）', format: 'number' },
  { key: 'cpu.load_average.five', label: '系统负载（5 分钟）', format: 'number' },
  { key: 'cpu.load_average.fifteen', label: '系统负载（15 分钟）', format: 'number' },
  { key: 'cpu.context_switches_per_sec', label: '上下文切换（/秒）', format: 'number' },
  { key: 'cpu.interrupts_per_sec', label: '中断（/秒）', format: 'number' },
  { key: 'memory.used_ratio', label: '内存使用率', format: 'ratio' },
  { key: 'memory.used_bytes', label: '内存已用', format: 'bytes' },
  { key: 'memory.available_bytes', label: '内存可用', format: 'bytes' },
  { key: 'swap.used_ratio', label: '交换使用率', format: 'ratio' },
  { key: 'shm_dir.total_bytes', label: '共享内存目录占用', format: 'bytes' },
  { key: 'shm_dir.page_files', label: '共享内存页文件数', format: 'count' },
]

/** 时间范围预设（秒）：历史视图的快捷选项 */
export interface SysmonHistoryRangePreset {
  label: string
  seconds: number
}

export const SYSMON_HISTORY_RANGES: SysmonHistoryRangePreset[] = [
  { label: '最近 15 分钟', seconds: 15 * 60 },
  { label: '最近 1 小时', seconds: 3600 },
  { label: '最近 6 小时', seconds: 6 * 3600 },
  { label: '最近 24 小时', seconds: 24 * 3600 },
  { label: '最近 7 天', seconds: 7 * 86400 },
]

/** 粒度选项（《帧契约：系统监控》§6.1 的 level 参数；auto 由服务端按范围宽度选择） */
export interface SysmonHistoryLevelOption {
  value: SysmonHistoryLevel
  label: string
}

export const SYSMON_HISTORY_LEVELS: SysmonHistoryLevelOption[] = [
  { value: 'auto', label: '自动' },
  { value: 'raw', label: '原始记录' },
  { value: 'minute', label: '一分钟聚合' },
]

/**
 * 历史视图三态（验收标准）：有数据 / 无数据 / 历史不可用。
 * `loading` 是请求在途（第四态，视图像「加载中」而不是任何结论）。
 */
export type SysmonHistoryState = 'idle' | 'loading' | 'ready' | 'empty' | 'unavailable'

/** 不可用原因的文案（《帧契约：系统监控》§6.1.3 的前端文案列）。 */
export function historyUnavailableText(reason: SysmonHistoryUnavailableReason): string {
  if (reason === 'not_found') return '历史记录未启用或尚无数据'
  if (reason === 'schema_mismatch') return '历史库版本与当前程序不符'
  return '历史库无法读取'
}

/**
 * 系统监控历史 store（《帧契约：系统监控》§6.1、《帧契约：REST》§2.8）。
 *
 * **与实时 store 完全独立**（验收标准：实时/历史切换不影响 WS 连接与实时数据）：
 * 本 store 只发 REST 查询、不订阅任何 WS 消息、不写实时镜像。监控页切换视图时只是换
 * 渲染来源，实时 store 仍在后台接收 `sysmon_snapshot`，切回实时视图立刻是最新数据。
 */
export const useSysmonHistoryStore = defineStore('sysmonHistory', () => {
  /** 已生效的查询参数（视图控件绑定这些值） */
  const metricKey = ref<string>(SYSMON_HISTORY_METRICS[0].key)
  const rangeSeconds = ref<number>(6 * 3600)
  const level = ref<SysmonHistoryLevel>('auto')

  const state = ref<SysmonHistoryState>('idle')
  const response = ref<SysmonHistoryResponse | null>(null)
  const error = ref<string | null>(null)
  /** 请求序号：丢弃过期响应（快速切换范围时前一次请求可能后返回） */
  let requestSeq = 0

  /** 当前选择的指标定义（视图据 format 决定数值展示形态） */
  const metricOption = computed<SysmonHistoryMetricOption | undefined>(
    () => SYSMON_HISTORY_METRICS.find(m => m.key === metricKey.value),
  )

  /** 历史库是否可用（false 时视图显示「历史不可用」+ 原因，不显示任何伪造数据） */
  const available = computed<boolean>(
    () => response.value?.available === true,
  )

  /** 不可用原因（available=true 或还没查过时为 null） */
  const unavailableReason = computed<SysmonHistoryUnavailableReason | null>(() => {
    const r = response.value
    return r && r.available === false ? r.reason : null
  })

  /** 时间点序列（unavailable / 空结果时为空数组） */
  const points = computed<SysmonHistoryPoint[]>(() => {
    const r = response.value
    if (!r || !r.available) return []
    return r.series[0]?.points ?? []
  })

  /** 历史标记（事实清单；unavailable 时为空数组） */
  const marks = computed<SysmonHistoryMark[]>(() => {
    const r = response.value
    if (!r || !r.available) return []
    return r.marks
  })

  /** 缺口区间（unavailable 时为空数组） */
  const gaps = computed<SysmonHistoryGap[]>(() => {
    const r = response.value
    if (!r || !r.available) return []
    return r.gaps
  })

  /** 实际使用的层级（auto 由服务端选定并回显） */
  const effectiveLevel = computed<'raw' | 'minute' | null>(() => {
    const r = response.value
    return r && r.available ? r.level : null
  })

  /** 桶宽（秒）；无桶时为 null */
  const intervalSec = computed<number | null>(() => {
    const r = response.value
    return r && r.available ? r.interval_sec : null
  })

  /** 是否因点数上限而只显示了最近一段（视图据此提示用户） */
  const truncated = computed<boolean>(() => {
    const r = response.value
    return r && r.available ? r.truncated : false
  })

  /** 生效的时间范围（[from, to) Unix 秒；未查过/不可用时为 null） */
  const effectiveRange = computed<{ from: number; to: number } | null>(() => {
    const r = response.value
    return r && r.available ? { from: r.from, to: r.to } : null
  })

  /**
   * 生效范围的实际秒数（《帧契约：系统监控》§6.1.2：`truncated=true` 时前端必须把范围控件同步到生效值）。
   * 截断后实际只有 `to − from` 秒，视图据它把「最近 N」的显示改成真实覆盖时长，
   * 避免用户以为看的是完整 N 而实际只看到最近一小段。
   */
  const effectiveRangeSeconds = computed<number | null>(() => {
    const range = effectiveRange.value
    return range ? range.to - range.from : null
  })

  /**
   * 查询历史。`to` 取当前时刻、`from = to - rangeSeconds`（前端不持有绝对时间选择器：
   * 历史视图给的是「最近 N」，与验收标准的「时间范围选择」一致，且避免时区/绝对时间的歧义）。
   *
   * 错误处理：
   * - 后端回 200 + available=false ⇒ state=unavailable（**不是** error：这是降级，不是失败）；
   * - HTTP 失败（网络/500）⇒ state=error，视图显示失败文案并可重试；
   * - 过期响应（期间又发了新查询）直接丢弃，不覆盖新结果。
   */
  async function load(): Promise<void> {
    const seq = ++requestSeq
    state.value = 'loading'
    error.value = null
    const to = Math.floor(Date.now() / 1000)
    const from = to - rangeSeconds.value
    try {
      const result = await sysmonApi.getHistory({
        metric: metricKey.value,
        from,
        to,
        level: level.value,
      })
      if (seq !== requestSeq) return  // 过期响应：丢弃
      response.value = result
      if (result.available === false) {
        state.value = 'unavailable'
      } else {
        state.value = (result.series[0]?.points.length ?? 0) > 0 ? 'ready' : 'empty'
      }
    } catch (e) {
      if (seq !== requestSeq) return
      response.value = null
      error.value = e instanceof Error ? e.message : String(e)
      state.value = 'idle'
    }
  }

  /** 切换指标后重新查询（视图控件 onchange 调用） */
  async function setMetric(key: string): Promise<void> {
    if (metricKey.value === key) return
    metricKey.value = key
    await load()
  }

  /**
   * 预选指标（#127）：实时视图点击 sparkline / 数值跳转历史时用。
   * **不触发查询** —— 查询由历史视图挂载时统一发起，避免「设置指标 + 挂载再查」两次请求；
   * 未注册的指标键忽略（下拉之外的键会让后端回 400，契约 sysmon §6.1）。
   */
  function preselect(key: string): void {
    if (!SYSMON_HISTORY_METRICS.some(metric => metric.key === key)) return
    metricKey.value = key
  }

  /** 切换时间范围后重新查询 */
  async function setRange(seconds: number): Promise<void> {
    if (rangeSeconds.value === seconds) return
    rangeSeconds.value = seconds
    await load()
  }

  /** 切换粒度后重新查询 */
  async function setLevel(next: SysmonHistoryLevel): Promise<void> {
    if (level.value === next) return
    level.value = next
    await load()
  }

  return {
    metricKey,
    rangeSeconds,
    level,
    state,
    response,
    error,
    metricOption,
    available,
    unavailableReason,
    points,
    marks,
    gaps,
    effectiveLevel,
    intervalSec,
    truncated,
    effectiveRange,
    effectiveRangeSeconds,
    load,
    setMetric,
    setRange,
    setLevel,
    preselect,
  }
})
