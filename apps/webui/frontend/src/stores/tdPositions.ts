import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { tdApi } from '@/api/td'
import { TD_LIST_PAGE_SIZE, createTdRetryTimer, tdDegradedText } from '@/composables/tdListStore'
import type { TdListQuery } from '@/composables/tdListStore'
import {
  groupPositionRows,
  mergePositionRows,
  reconcilePositionBaseline,
  tdPositionBusinessKey,
} from '@/composables/tdPositionView'
import type { TdDegradedReason, TdPositionRow, TdRecentPayload } from '@/types/api'

/**
 * 交易数据 · 持仓 store（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8，契约 webui-ws §2.5/§5）。
 *
 * **为什么不用 `createTdListStore`**（不是风格取舍，是语义冲突）：
 * - 列表工厂的合并按 `sort_key` 字符串降序插入；持仓行**没有 `sort_key`**（契约 rest
 *   §2.9.4 字段表），列表顺序是服务端 `(account_id, instrument_id, direction)` 升序。
 * - 列表工厂是有界窗口（超过 capacity 丢最旧行）；持仓是**绝对态**，丢一行就是向用户宣告
 *   一份并不存在的空仓（契约 webui-ws §5 第 13 条：不用假数据/假空态冒充）。
 * - 列表工厂的账户筛选选项来自信封 `online_accounts`；持仓的账户全集**含离线**账户
 *   （§5 第 11 条），必须由数据自身推导（见 {@link useTdPositionsStore} 的 `accounts`）。
 *
 * 因此本文件是持仓**更小的一份** store：REST 基线 + 全量分页、WS 单行绝对态覆盖（同键
 * `seq` 大者胜，`volume=0` 删行）、降级退避、`data_changed` 重置。合并/分组纯函数在
 * `composables/tdPositionView`，本文件不重复实现。
 *
 * 与委托/成交不同的两处行为，均在契约里有依据：
 * - **绝对态清行**：`volume=0` 的行不返回 REST（契约 rest §2.9.1 空语义），WS 的零帧正是
 *   「该持仓已消失」的表达（契约 td-data-sync §3：消失 side 发 volume=0 零帧）。因此零帧
 *   必须**删除**对应业务键，而不是插入一条 0 行。
 * - **行 `date` 是账户锚点日**，不是列表作用域的当前交易日；不同账户的 anchor 可能不同，
 *   日切判定不按行 date 做（以 `data_changed{scope:"td"}` 为准），避免把「另一账户还在
 *   上一交易日」误判成迟到数据。
 */

/** 首屏/每页行数（契约 rest §2.9.2 limit 默认 200，与委托/成交同一口径） */
export const TD_POSITIONS_PAGE_SIZE = TD_LIST_PAGE_SIZE

/**
 * 单次基线拉取的最大页数（防病态数据把一次刷新拖成无限请求）。
 *
 * 持仓行数 = 账户 × 合约，正常量级远小于 50 页（10 000 行）；触发上限时置 `truncated`，
 * 面板显示「仅显示前 N 行」——**不静默截断**（静默截断等于假空仓的反面：假完整列表）。
 */
export const TD_POSITIONS_MAX_PAGES = 50

export const useTdPositionsStore = defineStore('tdPositions', () => {
  // === 数据 ===
  /** 持仓行（绝对态；按服务端 `(account_id, instrument_id, direction)` 升序） */
  const rows = ref<TdPositionRow[]>([])
  /**
   * 账户全集（含离线；契约 webui-ws §5 第 11 条）：在线集合 ∪ 已返回行的 account_id。
   * 信封没有独立的账户全集字段，因此由数据推导；筛选生效期间只增不减（见 `ingestResponse`）。
   */
  const accounts = ref<string[]>([])
  /** 在线（Ready）账户集合（信封 online_accounts；仅用于展示与推导全集，不是筛选上限） */
  const onlineAccounts = ref<string[]>([])
  /** 用户选中的账户（默认全选；空 Set 且未改过筛选 = 全部已登记账户） */
  const accountFilter = ref<Set<string>>(new Set())
  /** 当前交易日（信封 trading_day；持仓行 date 是各账户锚点日，两者不同） */
  const tradingDay = ref<string | null>(null)

  // === 加载/降级 ===
  const loading = ref(false)
  const initialized = ref(false)
  const error = ref<string | null>(null)
  const available = ref(true)
  const degradedReason = ref<TdDegradedReason | null>(null)
  /** true = 行数超过 {@link TD_POSITIONS_MAX_PAGES} 页上限，列表不完整（不得宣称完整） */
  const truncated = ref(false)

  // === 内部状态（非响应式）===
  /** 每个账户已见过的最大 seq（同账户 seq 回退 = 数据重置，契约 webui-ws §5 第 8 条） */
  let maxSeqByAccount = new Map<string, number>()
  /** 是否由用户改过筛选（未改过 = 全选，且是账户全集的权威来源） */
  let filterTouched = false
  /** 请求代号：只有最后一次发起的请求允许写状态（筛选/重置并发时旧响应不得覆盖新基线） */
  let requestSeq = 0
  /** 降级重试定时器（与委托/成交工厂共用 `createTdRetryTimer`，退避口径唯一；dispose 后由 init 重新武装） */
  const retryTimer = createTdRetryTimer()
  let retryAttempt = 0
  let disposed = false

  // === 派生状态 ===
  /** 面板展示行：一行 = (account_id, instrument_id)，默认显示轧差后的账户净仓 */
  const groups = computed(() => groupPositionRows(rows.value))

  /** 筛选后实际参与查询的账户（空数组 = 全选 → 不传 accounts，契约 rest §2.9.2） */
  const effectiveAccounts = computed<string[]>(() => {
    const all = accounts.value
    if (all.length === 0) return []
    if (!filterTouched) return [...all]
    return [...accountFilter.value].filter(account => all.includes(account)).sort()
  })

  /** 是否全选（决定请求是否省略 accounts 参数） */
  const allAccountsSelected = computed<boolean>(() => {
    const total = accounts.value.length
    if (total === 0) return true
    const effective = effectiveAccounts.value.length
    return effective === 0 || effective === total
  })

  /**
   * 是否「无数据」空态：**成功**读到基线且没有持仓行。
   * 降级与请求失败都不是「无数据」——不能让用户把「库读不了」看成「空仓」
   * （契约 webui-ws §5 第 13 条）。
   */
  const isEmpty = computed<boolean>(
    () => available.value && error.value === null && rows.value.length === 0)

  /** 降级提示文案（可用时为空串；与委托/成交共用 `tdDegradedText`，同一故障同一句话） */
  const degradedText = computed<string>(() => (available.value ? '' : tdDegradedText(degradedReason.value)))

  /** 行内数据日期的去重集合（升序）；多个 = 各账户锚点日不同，面板不得只标一个日期 */
  const anchorDates = computed<string[]>(() => {
    const dates = new Set<string>()
    for (const row of rows.value) dates.add(row.date)
    return [...dates].sort()
  })

  /** 请求用的 accounts 参数：全选时 undefined（契约 rest §2.9.2 缺省 = 全部已登记账户） */
  function accountsParam(): string[] | undefined {
    if (allAccountsSelected.value) return undefined
    return effectiveAccounts.value
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

  /**
   * 清空**行数据**（重置触发共用；不发起请求）。
   *
   * 刻意不清 `accounts` / `accountFilter`：账户全集是筛选的输入，清掉它会让
   * `allAccountsSelected` 瞬间变成 true（空全集视为全选），正在生效的筛选被静默丢弃——
   * 「清空重拉」就退化成了「取消筛选」。全集由重拉后的响应重新推导（未筛选时权威替换，
   * 筛选生效时只增不减，见 `ingestAccountUniverse`）。
   */
  function clearData(): void {
    rows.value = []
    onlineAccounts.value = []
    tradingDay.value = null
    truncated.value = false
    maxSeqByAccount = new Map()
  }

  function rebuildMaxSeq(): void {
    const map = new Map<string, number>()
    for (const row of rows.value) {
      const current = map.get(row.account_id)
      if (current === undefined || row.seq > current) map.set(row.account_id, row.seq)
    }
    maxSeqByAccount = map
  }

  /**
   * 账户全集推导（契约 webui-ws §5 第 11 条：持仓/资金用返回的账户全集，含离线）。
   *
   * 信封没有独立的账户全集字段，因此：
   * - **未筛选**时响应即全集：在线集合 ∪ 本页行出现的账户（权威，可增可减）；
   * - **筛选生效**时响应只含被选中账户，用它重建全集会把其他账户从筛选项里抹掉：
   *   只做「并入在线集合」，不缩小已知道的全集。用户回到全选后由权威来源重建。
   */
  function ingestAccountUniverse(responseAccounts: string[], responseRows: TdPositionRow[]): void {
    if (!filterTouched) {
      const universe = new Set(responseAccounts)
      for (const row of responseRows) universe.add(row.account_id)
      accounts.value = [...universe].sort()
      accountFilter.value = new Set(accounts.value)
      return
    }
    const merged = new Set(accounts.value)
    for (const account of responseAccounts) merged.add(account)
    accounts.value = [...merged].sort()
  }

  /**
   * 拉完整基线（跟随 `next_cursor` 直到 null 或达到页数上限）。
   *
   * 持仓是绝对态，面板需要完整集合才能正确聚合（缺一页会让净仓数字偏小，属「用假数据冒充」）。
   * 因此这里主动翻完全部分页，而不是等用户滚动——与列表工厂的按需预取刻意不同。
   */
  async function loadBaseline(): Promise<void> {
    if (disposed) return
    const seq = ++requestSeq
    loading.value = true
    try {
      const query: TdListQuery = { limit: TD_POSITIONS_PAGE_SIZE, accounts: accountsParam() }
      const collected: TdPositionRow[] = []
      let cursor: string | null = null
      let page = 0
      let hitPageCap = false
      do {
        const response = await tdApi.getPositions(cursor === null ? query : { ...query, cursor })
        // 请求在途时被卸载 / 被更新的请求取代：丢弃响应，别让旧基线覆盖新基线
        if (disposed || seq !== requestSeq) return
        if (response.available === false) {
          available.value = false
          degradedReason.value = response.reason
          // 降级时**保留**已有行（旧快照比假空仓安全），但绝不显示 0
          scheduleRetry()
          return
        }
        available.value = true
        degradedReason.value = null
        error.value = null
        retryAttempt = 0
        clearRetry()
        tradingDay.value = response.trading_day
        onlineAccounts.value = response.online_accounts
        collected.push(...response.items)
        cursor = response.next_cursor
        page += 1
        if (cursor !== null && page >= TD_POSITIONS_MAX_PAGES) {
          hitPageCap = true
          break
        }
      } while (cursor !== null)

      // 基线 = 权威快照（绝对态），因此**替换**而不是合并：REST 没有返回的业务键 = 该持仓
      // 已消失（查询快照整组原子替换，契约 td-data-sync §3），保留旧行会留下幽灵持仓。
      // 唯一的例外是同键 WS 增量更新：REST 可能先于 WS 持久化可见（异步提交），窗口内
      // 行已有更大 seq 时保留本地版本（契约 webui-ws §5 第 8 条：同键 seq 大者胜）。
      rows.value = reconcilePositionBaseline(collected, rows.value)
      truncated.value = hitPageCap
      // 账户全集以「本次未筛选的基线」为准；筛选生效时见 ingestAccountUniverse 的取舍
      ingestAccountUniverse(onlineAccounts.value, rows.value)
      rebuildMaxSeq()
    } catch (e) {
      if (disposed || seq !== requestSeq) return
      error.value = e instanceof Error ? e.message : String(e)
      // HTTP 失败（dzweb 重启/网络抖动）同样退避重试；保留已有行，不伪造空态
      scheduleRetry()
    } finally {
      if (seq === requestSeq) {
        loading.value = false
        initialized.value = true
      }
    }
  }

  /**
   * 页面挂载：拉完整基线。
   * 每次挂载都重拉：store 是单例，离开期间 WS 增量仍在到达且可能跨日切，旧窗口不可信。
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
   * 重置并重拉（`data_changed{scope:"td"}` / 重连 / 手动刷新 / 筛选变化）。
   *
   * 先清空再拉：绝对态下旧行属于旧账户集合或旧锚点日，留着会与「清空重拉」的契约义务
   * 冲突（契约 webui-ws §5 第 10 条）。降级时 `loadBaseline` 会保留已有行——此时行已被
   * 清空，屏幕由降级提示条承担，不会出现「假空仓」。
   */
  async function resetAndReload(): Promise<void> {
    clearData()
    await loadBaseline()
  }

  /** 手动刷新（面板「刷新」按钮）：丢弃本地窗口重拉基线 */
  async function refresh(): Promise<void> {
    await resetAndReload()
  }

  /**
   * 收到一条 `td_position_info`（契约 webui-ws §2.5）：绝对态按业务键覆盖。
   *
   * 规则（契约 webui-ws §5 第 8 条 + 契约 rest §2.9.1/§2.9.6）：
   * - `volume === 0` → **删除**该业务键（零帧 = 持仓已消失，不是「持仓 0 手」）；
   * - 同键 `seq` 更大才覆盖；同键 `date` 更旧的事件视为过期快照丢弃；
   * - 同账户 `seq` 回退且业务键未知 → 数据重置（进程重启/库重置），清空重拉基线；
   * - 账户全集并入该行账户（离线账户也可能推进量）。
   *
   * 不做 10Hz 批量：持仓事件由查询/成交驱动，量级远低于委托成交，逐条覆盖是安全的；
   * 引入批量只会让「零帧删行」与「基线到达」的竞态更难解释。
   */
  function applyPositionEvent(row: TdPositionRow): void {
    if (disposed) return
    if (!row || typeof row !== 'object') return
    if (typeof row.account_id !== 'string' || typeof row.instrument_id !== 'string') return
    if (typeof row.seq !== 'number') return
    const key = tdPositionBusinessKey(row)
    const index = rows.value.findIndex(item => tdPositionBusinessKey(item) === key)
    if (row.volume === 0) {
      if (index >= 0) {
        const next = rows.value.slice()
        next.splice(index, 1)
        rows.value = next
      }
      absorbUniverse(row.account_id)
      return
    }
    if (index >= 0) {
      const current = rows.value[index]
      // 同键：seq 大者胜；seq 相等视为重复消息；date 更旧 = 过期快照（绝对态不接受旧快照回写）
      if (row.seq <= current.seq || row.date < current.date) return
      const next = rows.value.slice()
      next[index] = row
      rows.value = next
      absorbSeq(row)
      absorbUniverse(row.account_id)
      return
    }
    // 新业务键：同账户 seq 低于水位 = 数据重置（契约 webui-ws §5 第 8 条）
    const watermark = maxSeqByAccount.get(row.account_id)
    if (watermark !== undefined && row.seq < watermark) {
      void resetAndReload()
      return
    }
    rows.value = mergePositionRows(rows.value, [row])
    absorbSeq(row)
    absorbUniverse(row.account_id)
  }

  function absorbSeq(row: TdPositionRow): void {
    const current = maxSeqByAccount.get(row.account_id)
    if (current === undefined || row.seq > current) maxSeqByAccount.set(row.account_id, row.seq)
  }

  function absorbUniverse(account: string): void {
    if (!account || accounts.value.includes(account)) return
    const next = [...accounts.value, account].sort()
    accounts.value = next
    if (!filterTouched) accountFilter.value = new Set(next)
  }

  /**
   * 收到建连补发 `td_recent`（契约 webui-ws §2.5）：认领 `td_position_info` 后并入，
   * 随后重拉 REST 基线（补发有界，不能替代基线；契约 §5 第 7 条）。
   *
   * 补发环从旧到新，其 seq 可能低于断连前水位——因此**不判 seq 回退**（一次正常重连不该被
   * 误判成数据重置），同键仍按 seq 大者胜覆盖，基线到达后收敛到权威值。
   * 页面未初始化时不发请求（store 是单例，无视图时不产生流量）。
   */
  function applyRecent(payload: TdRecentPayload | undefined): void {
    if (disposed || !initialized.value) return
    if (!Array.isArray(payload?.events)) return
    const seeded: TdPositionRow[] = []
    for (const event of payload.events) {
      if (event?.type !== 'td_position_info') continue
      const row = event.data as TdPositionRow | undefined
      if (row && typeof row === 'object' && typeof row.account_id === 'string') seeded.push(row)
    }
    // 先并入补发（同键 seq 大者胜），再重拉基线；基线到达后按同键 seq 大者胜收敛
    if (seeded.length > 0) {
      const live = seeded.filter(row => row.volume > 0)
      const zeros = seeded.filter(row => row.volume === 0)
      if (live.length > 0) rows.value = mergePositionRows(rows.value, live)
      for (const row of zeros) {
        const key = tdPositionBusinessKey(row)
        rows.value = rows.value.filter(item => tdPositionBusinessKey(item) !== key)
      }
      for (const row of live) absorbSeq(row)
    }
    void resetAndReload()
  }

  /** `data_changed` 分发（契约 webui-ws §2.3）：scope=td 才清空重拉 */
  async function handleDataChanged(scope: string): Promise<void> {
    if (scope !== 'td') return
    if (!initialized.value) return
    await resetAndReload()
  }

  /** 账户筛选变化：清空重拉（契约 webui-ws §5 第 11 条：筛选变化必须重置游标从首页开始） */
  async function setAccountFilter(next: string[]): Promise<void> {
    filterTouched = true
    accountFilter.value = new Set(next)
    await resetAndReload()
  }

  /** 恢复默认全选（筛选面板「全选」） */
  async function selectAllAccounts(): Promise<void> {
    filterTouched = false
    accountFilter.value = new Set(accounts.value)
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
    groups,
    accounts,
    onlineAccounts,
    accountFilter,
    tradingDay,
    loading,
    initialized,
    error,
    available,
    degradedReason,
    degradedText,
    truncated,
    isEmpty,
    effectiveAccounts,
    allAccountsSelected,
    anchorDates,
    init,
    refresh,
    resetAndReload,
    applyPositionEvent,
    applyRecent,
    handleDataChanged,
    setAccountFilter,
    selectAllAccounts,
    dispose,
  }
})
