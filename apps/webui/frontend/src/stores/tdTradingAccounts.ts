import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { tdApi } from '@/api/td'
import { createTdRetryTimer, tdDegradedText } from '@/composables/tdListStore'
import type { TdDegradedReason, TdRecentPayload, TdTradingAccountRow } from '@/types/api'

/**
 * 交易数据 · 资金 store（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8，契约 webui-ws §2.5/§5）。
 *
 * 为什么**不用** `createTdListStore`（不是风格取舍，是语义冲突）：
 * - 列表工厂按 `sort_key` 字符串降序插入、跟随 `cursor` 分页；资金行**没有 `sort_key`**
 *   （契约 rest §2.9.4 字段表），端点**不分页**（`next_cursor` 恒 null、`limit`/`cursor` 被
 *   忽略，契约 rest §2.9.2）。
 * - 列表工厂是有界窗口（超过 capacity 丢最旧行）；资金是**绝对态**——一行 = 一个已登记账户，
 *   丢一行就是向用户宣告一个并不存在的账户（契约 webui-ws §5 第 13 条：不用假数据冒充）。
 * - 资金行集合本身就是**账户全集**（含离线与 `has_data:false` 的无记录账户，契约 rest
 *   §2.9.4），不存在列表工厂那种「在线集合 vs 已返回账户全集」的筛选推导。
 *
 * 因此本文件是资金**更小的一份** store：REST 基线（单请求）+ WS 单行绝对态覆盖（同键
 * `seq` 大者胜）+ 降级退避 + `data_changed` 重置。展示/格式化纯函数在
 * `composables/tdFundView`，本文件不重复实现。
 *
 * 与持仓的两处关键差异，均在契约里有依据：
 * - **业务键 = account_id**（契约 rest §2.9.6），一个账户只有一行资金卡；
 * - **`has_data:false` 的行也是有效数据**（无资金记录的账户仍返回，契约 rest §2.9.4），
 *   因此绝不能按「资金字段全 null」筛掉它——那会让「无记录」从面板上消失。
 */
export const useTdTradingAccountsStore = defineStore('tdTradingAccounts', () => {
  // === 数据 ===
  /** 资金行（每账户一行；按服务端 `account_id` 升序） */
  const rows = ref<TdTradingAccountRow[]>([])
  /** 在线（Ready）账户集合（信封 online_accounts；用于状态列的 在线/离线 判定） */
  const onlineAccounts = ref<string[]>([])
  /** 当前交易日（信封 trading_day；资金行 date 是各账户锚点日，两者不同） */
  const tradingDay = ref<string | null>(null)

  // === 加载/降级 ===
  const loading = ref(false)
  const initialized = ref(false)
  const error = ref<string | null>(null)
  const available = ref(true)
  const degradedReason = ref<TdDegradedReason | null>(null)

  // === 内部状态（非响应式）===
  /** 每个账户已见过的最大 seq（同账户 seq 严格回退 = 数据重置，契约 webui-ws §5 第 8 条） */
  let maxSeqByAccount = new Map<string, number>()
  /** 请求代号：只有最后一次发起的请求允许写状态（重置/重拉并发时旧响应不得覆盖新基线） */
  let requestSeq = 0
  /** 降级重试定时器（与委托/成交/持仓共用 `createTdRetryTimer`，退避口径唯一；dispose 后由 init 重新武装） */
  const retryTimer = createTdRetryTimer()
  let retryAttempt = 0
  let disposed = false

  // === 派生状态 ===
  /**
   * 是否「无数据」空态：**成功**读到基线且没有任何资金行。
   * 降级与请求失败都不是「无数据」——不能让用户把「库读不了」看成「没有账户」
   * （契约 webui-ws §5 第 13 条）。
   */
  const isEmpty = computed<boolean>(
    () => available.value && error.value === null && rows.value.length === 0)

  /** 降级提示文案（可用时为空串；与委托/成交/持仓共用 `tdDegradedText`，同一故障同一句话） */
  const degradedText = computed<string>(() => (available.value ? '' : tdDegradedText(degradedReason.value)))

  /**
   * 行内数据日期的去重集合（升序）；多个 = 各账户锚点日不同，面板不得只标一个日期。
   * `has_data:false` 行的 date 是 null，不参与（它不是数据日期）。
   */
  const dataDates = computed<string[]>(() => {
    const dates = new Set<string>()
    for (const row of rows.value) {
      if (row.date !== null) dates.add(row.date)
    }
    return [...dates].sort()
  })

  /**
   * 在线账户数（= 信封 online_accounts 的长度，契约 rest §2.9.2；面板聚合数「在线/总」的分子）。
   * 「总」= 已登记账户数 = 资金行数（每账户一行，含离线，契约 rest §2.9.4）。
   */
  const onlineCount = computed<number>(() => onlineAccounts.value.length)

  /** 已登记账户总数 = 资金行数（含离线与 has_data:false） */
  const totalAccounts = computed<number>(() => rows.value.length)

  /** 服务端顺序（契约 rest §2.9.2）：items 按 `account_id` 升序；字符串按原值比较（账户 ID 为 ASCII） */
  function compareTradingAccountRows(a: TdTradingAccountRow, b: TdTradingAccountRow): number {
    if (a.account_id === b.account_id) return 0
    return a.account_id < b.account_id ? -1 : 1
  }

  function clearRetry(): void {
    retryTimer.clear()
  }

  function scheduleRetry(): void {
    if (disposed) return
    retryTimer.schedule(retryAttempt, () => {
      void loadBaseline()
    })
    retryAttempt += 1
  }

  /** 清空**行数据**（重置触发共用；不发起请求） */
  function clearData(): void {
    rows.value = []
    onlineAccounts.value = []
    tradingDay.value = null
    maxSeqByAccount = new Map()
  }

  /** 从当前行重建每账户 seq 水位（水位只来自有记录的行；has_data:false 行 seq 为 null） */
  function rebuildMaxSeq(): void {
    const map = new Map<string, number>()
    for (const row of rows.value) {
      if (row.seq === null) continue
      const current = map.get(row.account_id)
      if (current === undefined || row.seq > current) map.set(row.account_id, row.seq)
    }
    maxSeqByAccount = map
  }

  /** 记录该账户已见的最大 seq（合并判据的唯一写入点） */
  function absorbSeq(row: TdTradingAccountRow): void {
    if (row.seq === null) return
    const current = maxSeqByAccount.get(row.account_id)
    if (current === undefined || row.seq > current) maxSeqByAccount.set(row.account_id, row.seq)
  }

  /** 把账户并入在线集合（资金事件只来自在线账户；集合顺序不承诺，排序只为稳定） */
  function absorbOnline(account: string): void {
    if (onlineAccounts.value.includes(account)) return
    onlineAccounts.value = [...onlineAccounts.value, account].sort()
  }

  /** 事件行是否可消费（协议载荷可能被中间层污染；坏行丢弃而不是让 store 抛错） */
  function isValidEventRow(row: TdTradingAccountRow | undefined): row is TdTradingAccountRow {
    return !!row && typeof row === 'object'
      && typeof row.account_id === 'string' && row.account_id !== ''
      && typeof row.seq === 'number'
  }

  /**
   * 拉资金基线（契约 rest §2.9.1–§2.9.4）。
   *
   * **单请求、不翻页**：端点不分页（`next_cursor` 恒 null，`limit`/`cursor` 被忽略，契约
   * rest §2.9.2）。实现上刻意不读 `next_cursor`——即使服务端异常返回非 null，也不得跟随，
   * 否则一次刷新可能被拖成无限翻页。
   *
   * 响应判别：
   * - `available:true` → 整体替换行集合（账户集合与各账户锚点日的权威快照）；
   * - `available:false` → 进降级态并退避重试，**保留已有行**（旧快照比假空态安全，但绝不显示 0）；
   * - HTTP 失败 → 记录 error，保留已有行（不把面板清成「没有账户」）。
   */
  async function loadBaseline(): Promise<void> {
    if (disposed) return
    const seq = ++requestSeq
    loading.value = true
    try {
      const response = await tdApi.getTradingAccounts()
      // 请求在途时被卸载 / 被更新的请求取代：丢弃响应，别让旧基线覆盖新基线
      if (disposed || seq !== requestSeq) return
      if (response.available === false) {
        available.value = false
        degradedReason.value = response.reason
        scheduleRetry()
        return
      }
      available.value = true
      degradedReason.value = null
      error.value = null
      retryAttempt = 0
      clearRetry()
      tradingDay.value = response.trading_day
      onlineAccounts.value = [...response.online_accounts]
      // 基线 = 权威快照（账户集合 + 各账户锚点日）：**整体替换**而不是合并——REST 没返回的
      // 账户 = 已不在已登记账户集合里（契约 rest §2.9.1/§2.9.4），保留旧行会留下幽灵资金卡。
      rows.value = [...response.items].sort(compareTradingAccountRows)
      rebuildMaxSeq()
    } catch (e) {
      if (disposed || seq !== requestSeq) return
      error.value = e instanceof Error ? e.message : String(e)
      // HTTP 失败（dzweb 重启/网络抖动）同样按退避重试；保留已有行，不伪造空态
      scheduleRetry()
    } finally {
      if (seq === requestSeq) {
        loading.value = false
        initialized.value = true
      }
    }
  }

  /**
   * 重置并重拉（`data_changed{scope:"td"}` / 页面挂载的统一入口）。
   * 先清空再拉：绝对态下旧行属于旧账户集合或旧锚点日，留着会与「清空重拉」的契约义务冲突
   * （契约 webui-ws §5 第 10 条）。
   */
  async function resetAndReload(): Promise<void> {
    clearData()
    await loadBaseline()
  }

  /**
   * 页面挂载：拉基线。每次挂载都重拉：store 是单例，离开期间 WS 增量仍在到达且可能跨日切，
   * 旧窗口不可信。
   *
   * 同时**重新武装退避定时器**：`dispose()` 会停用它，若不在挂载时 `reset()`，卸载再挂载后
   * 一旦基线降级就不会再退避重试（只剩 `data_changed` 兜底），违背契约 webui-ws §5 第 13 条。
   */
  async function init(): Promise<void> {
    // dispose 只停流量、不终结 store 生命周期；这里解除停用并重新武装重试定时器
    disposed = false
    retryTimer.reset()
    await resetAndReload()
  }

  /**
   * 把一条资金行并入绝对态集合（`judgeReset = true` 时判「同账户 seq 回退 = 数据重置」）。
   *
   * 合并规则（契约 webui-ws §2.5/§5 第 8 条 + 契约 rest §2.9.6）：
   * - 业务键 = `account_id`：同键 **`seq` 大者胜**；相等视为重复消息（幂等，重复无害）；
   * - 同账户 **`seq` 严格回退** = 数据重置（进程重启/库重置）→ 清空并重拉基线，本事件不再 apply；
   * - 新账户：按 `account_id` 升序插入（与服务端顺序一致），并入在线集合。
   *
   * 为什么严格回退直接判重置而不是当乱序丢弃：资金是**单账户单行**的绝对态，没有第二条记录
   * 能区分「更旧的乱序事件」与「重置后的新序列」；契约 webui-ws §5 第 8 条要求后者必须重拉，
   * 而前者按此处理的代价只是一次基线重拉（结果仍收敛到权威快照），不会写错数据。
   *
   * `has_data` 不做判定：WS 行恒为 `true`（契约 rest §2.9.4），无记录行只来自 REST 基线。
   */
  function mergeEventRow(row: TdTradingAccountRow, judgeReset: boolean): void {
    if (judgeReset) {
      const watermark = maxSeqByAccount.get(row.account_id)
      if (watermark !== undefined && row.seq !== null && row.seq < watermark) {
        void resetAndReload()
        return
      }
    }
    const index = rows.value.findIndex(item => item.account_id === row.account_id)
    if (index >= 0) {
      const currentSeq = rows.value[index].seq
      // 同键 seq 大者胜；相等 = 重复消息。当前行无 seq（has_data:false）时视为必被覆盖。
      if (row.seq !== null && currentSeq !== null && row.seq <= currentSeq) return
      const next = rows.value.slice()
      next[index] = row
      rows.value = next
    } else {
      rows.value = [...rows.value, row].sort(compareTradingAccountRows)
    }
    absorbSeq(row)
    absorbOnline(row.account_id)
  }

  /**
   * 收到一条 `td_trading_account`（契约 webui-ws §2.5）：绝对态按业务键覆盖。
   * 事件只来自在线账户、`has_data` 恒 true，因此一次事件 = 该账户的最新资金快照。
   */
  function applyTradingAccountEvent(row: TdTradingAccountRow): void {
    if (disposed) return
    if (!isValidEventRow(row)) return
    mergeEventRow(row, true)
  }

  /**
   * 收到建连补发 `td_recent`（契约 webui-ws §2.5）：认领 `td_trading_account` 事件并入，
   * 随后重拉 REST 基线（补发有界，不能替代基线；契约 webui-ws §5 第 7 条 / §6）。
   *
   * 补发环从旧到新，其 seq 可能低于本地水位——因此**不判 seq 回退**（一次正常重连不该被
   * 误判成数据重置），同键仍按 seq 大者胜并入；基线到达后按权威快照整体替换收敛。
   * 这里用 `loadBaseline` 而不是「清空重拉」：补发先并入可避免重拉窗口内的空屏，重拉失败时
   * 也保留已并入的补发与旧行（不把一次网络抖动变成「没有账户」）。
   * 页面未初始化时不发请求（store 是单例，无视图时不产生流量）。
   */
  function applyRecent(payload: TdRecentPayload | undefined): void {
    if (disposed || !initialized.value) return
    if (!Array.isArray(payload?.events)) return
    for (const event of payload.events) {
      if (event?.type !== 'td_trading_account') continue
      const row = event.data as TdTradingAccountRow | undefined
      if (!isValidEventRow(row)) continue
      mergeEventRow(row, false)
    }
    void loadBaseline()
  }

  /** `data_changed` 分发（契约 webui-ws §2.3）：scope=td 才清空重拉 */
  async function handleDataChanged(scope?: string): Promise<void> {
    if (scope !== 'td') return
    if (disposed || !initialized.value) return
    await resetAndReload()
  }

  /** 页面卸载：停退避重试与在途响应写入（卸载只停流量；重新挂载由 `init()` 重新武装定时器） */
  function dispose(): void {
    disposed = true
    clearRetry()
    retryTimer.dispose()
  }

  return {
    rows,
    onlineAccounts,
    tradingDay,
    loading,
    initialized,
    error,
    available,
    degradedReason,
    degradedText,
    isEmpty,
    dataDates,
    onlineCount,
    totalAccounts,
    init,
    loadBaseline,
    applyTradingAccountEvent,
    applyRecent,
    handleDataChanged,
    dispose,
  }
})
