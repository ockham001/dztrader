import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { tdApi } from '@/api/td'
import { TD_UNAVAILABLE_TEXT } from '@/composables/tdOrderView'
import type { TdStatsDegradedReason, TdStatsValues } from '@/types/api'

/**
 * 交易数据 · 统计条 store（契约 rest §2.9.5、契约 webui-ws §2.3/§5 第 10 条）。
 *
 * 职责边界：
 * - **无 WS 推送**：统计只有 REST 基线（契约 webui-ws §5 第 7 条），按需轮询
 *   {@link TD_STATS_POLL_INTERVAL_MS}（≤1Hz，与列表批量刷新同节流，§5 第 10 条）。
 * - **降级 = 计数器未就绪**（reason 只有 `not_ready`），**不是「当日 0 委托」**：`available:false`
 *   时绝不清零 4 个值（契约 rest §2.9.5 + webui-ws §5 第 13 条：不用 0 冒充数据）。
 *   HTTP 失败同样保留旧值 + 记录 `error`。
 * - **重置触发**：日切（`trading_day` 变化）与 `data_changed{scope:"td"}` → 丢弃旧值后重拉，
 *   不混入上一交易日（契约 webui-ws §5 第 10 条）。
 * - `dispose()`：离开页面即停轮询（store 是单例，无视图时不产生请求）。
 */

/** 轮询间隔（契约 webui-ws §5 第 10 条：统计无 WS 推送，按需轮询 REST，≤1Hz） */
export const TD_STATS_POLL_INTERVAL_MS = 1_000

/** 统计口径（纯展示切换，不发请求） */
export type TdStatsUnit = 'orders' | 'volume'

/** 轮询调度器（可注入：测试用可控回调替代浏览器定时器，与 `TdBatchScheduler` 同型） */
export interface TdStatsScheduler {
  set(callback: () => void, delayMs: number): unknown
  clear(handle: unknown): void
}

const defaultScheduler: TdStatsScheduler = {
  set: (callback, delayMs) => setTimeout(callback, delayMs),
  clear: handle => clearTimeout(handle as ReturnType<typeof setTimeout>),
}

let scheduler: TdStatsScheduler = defaultScheduler

/** 测试专用：注入轮询调度器（生产路径使用默认 setTimeout） */
export function __setTdStatsSchedulerForTests(next: TdStatsScheduler): void {
  scheduler = next
}

/** 测试专用：恢复默认轮询调度器 */
export function __resetTdStatsSchedulerForTests(): void {
  scheduler = defaultScheduler
}

/** 降级文案（统一口径；契约 rest §2.9.5 只有 not_ready 一种原因） */
const DEGRADED_TEXT = '统计暂不可用，正在自动重试'

export const useTdStatsStore = defineStore('tdStats', () => {
  /** 4 个计数器值；null = 还没成功拉过（组件显示占位，不显示 0） */
  const values = ref<TdStatsValues | null>(null)
  /** 当前交易日；null = 无在线账户（契约 rest §2.9.5 trading_day 语义同列表信封） */
  const tradingDay = ref<string | null>(null)
  /** true = 计数器已就绪；false = 降级（计数器未就绪） */
  const available = ref(true)
  /** 降级原因；非降级时为 null */
  const degradedReason = ref<TdStatsDegradedReason | null>(null)
  /** 展示口径（笔数 / 手数） */
  const unit = ref<TdStatsUnit>('orders')
  /** 是否已发起过首次请求（区分「加载中」与「还没拉过」） */
  const initialized = ref(false)
  /** HTTP 失败信息（保留旧值，不清零） */
  const error = ref<string | null>(null)

  // === 内部状态（非响应式）===
  let pollHandle: unknown = null
  let disposed = false
  /**
   * 请求代号：每发起一次 `refresh()` 自增。只有「最后一次发起的请求」允许写状态——
   * 轮询与 `data_changed` 触发的重拉可能重叠，旧响应晚到会覆盖新基线（例如日切后把上一
   * 交易日的数字写回屏幕）。丢弃过期响应，保证「不混入上一交易日」（webui-ws §5 第 10 条）。
   */
  let requestSeq = 0

  /** 单位文案（笔 / 手） */
  const unitLabel = computed<string>(() => (unit.value === 'orders' ? '笔' : '手'))

  /** 未成交数量（按 unit 取笔数或手数）；无值显示占位，不显示 0 */
  const unfilledText = computed<string>(() => {
    if (values.value === null) return TD_UNAVAILABLE_TEXT
    return String(unit.value === 'orders'
      ? values.value.unfilled_orders
      : values.value.unfilled_volume)
  })

  /** 总量（按 unit 取笔数或手数）；无值显示占位，不显示 0 */
  const totalText = computed<string>(() => {
    if (values.value === null) return TD_UNAVAILABLE_TEXT
    return String(unit.value === 'orders' ? values.value.total_orders : values.value.total_volume)
  })

  /**
   * 是否「成功但无数据」空态：**可用**读到基线且当日没有委托。
   * 降级与请求失败都不是空态（契约 webui-ws §5 第 13 条：不用假空态冒充）。
   */
  const isEmpty = computed<boolean>(
    () => available.value && error.value === null && values.value !== null && values.value.total_orders === 0)

  /** 降级提示文案（可用时为空串；统一降级提示条的唯一文案来源） */
  const degradedText = computed<string>(() => (available.value ? '' : DEGRADED_TEXT))

  /** 清除待执行的轮询定时器（不改变 disposed 语义） */
  function clearPoll(): void {
    if (pollHandle !== null) {
      scheduler.clear(pollHandle)
      pollHandle = null
    }
  }

  /**
   * 排下一次轮询（先清旧的，避免手动 refresh 后叠加定时器）。
   * 降级期间照常轮询：契约 rest §2.9.5 的恢复路径就是下一次成功重算，间隔保持 1Hz（简单且满足
   * 「按需轮询」）。
   */
  function schedulePoll(): void {
    if (disposed) return
    clearPoll()
    pollHandle = scheduler.set(() => {
      pollHandle = null
      void refresh()
    }, TD_STATS_POLL_INTERVAL_MS)
  }

  /**
   * 拉一次统计基线（`GET /api/td/stats`，无参数：stats 是全局口径）。
   *
   * 响应判别（契约 rest §2.9.5）：
   * - `available:true` → 写入 4 值 + 交易日。交易日变化 = 日切：响应是新一天的**完整基线**，
   *   整对象替换（计数器不是列表，没有可增量合并的行），因此不会混入上一交易日
   *   （webui-ws §5 第 10 条）。
   * - `available:false` → 进降级态，**保留旧值**（计数器未就绪 ≠ 当日 0 委托）。
   * - HTTP 失败 → 记录 error，保留旧值与上一次 available（不清零、不编造降级原因）。
   */
  async function refresh(): Promise<void> {
    if (disposed) return
    const seq = ++requestSeq
    try {
      const response = await tdApi.getStats()
      // 请求在途时页面被卸载 → 丢弃；或被更新的请求取代 → 丢弃（旧基线不得覆盖新基线）
      if (disposed || seq !== requestSeq) return
      if (response.available === false) {
        available.value = false
        degradedReason.value = response.reason
        error.value = null
        return
      }
      // 日切：旧值属于上一交易日。响应是**新一天的完整基线**，整对象替换（计数器不是列表，
      // 没有可增量合并的行），因此不会混入上一交易日的数字（契约 webui-ws §5 第 10 条）。
      available.value = true
      degradedReason.value = null
      error.value = null
      tradingDay.value = response.trading_day
      values.value = response.stats
    } catch (e) {
      if (disposed || seq !== requestSeq) return
      error.value = e instanceof Error ? e.message : String(e)
    } finally {
      // 已卸载或已被更新的请求取代：不排轮询（新的那次 refresh 会自己排），避免叠加定时器
      if (!disposed && seq === requestSeq) {
        initialized.value = true
        schedulePoll()
      }
    }
  }

  /** 页面挂载：拉一次基线 + 启动 ≤1Hz 轮询（与 OrdersTab 同型，组件自行 init/dispose） */
  async function init(): Promise<void> {
    // 重新挂载时解除 dispose（dispose 只停定时器，不终结 store 生命周期）
    disposed = false
    clearPoll()
    await refresh()
  }

  /** 切换展示口径（纯展示，不发请求） */
  function setUnit(next: TdStatsUnit): void {
    unit.value = next
  }

  /**
   * `data_changed` 分发（契约 webui-ws §2.3/§5 第 10 条）：统计无 WS 推送，本消息只负责
   * 「标记基线失效 + 立即重拉」。清空旧值（不把上一交易日/上一账户集合的数字留在屏幕），
   * 随后 `refresh()` 取新基线；未挂载（未初始化）不产生流量。
   */
  async function handleDataChanged(): Promise<void> {
    if (disposed || !initialized.value) return
    values.value = null
    await refresh()
  }

  /** 页面卸载：停轮询与在途响应写入（离开页面后不再请求） */
  function dispose(): void {
    disposed = true
    clearPoll()
  }

  return {
    values,
    tradingDay,
    available,
    degradedReason,
    unit,
    unitLabel,
    initialized,
    error,
    unfilledText,
    totalText,
    isEmpty,
    degradedText,
    init,
    refresh,
    setUnit,
    handleDataChanged,
    dispose,
  }
})
