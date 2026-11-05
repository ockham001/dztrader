/**
 * 交易数据的**有界列表 store 工厂**（契约 rest §2.9、契约 webui-ws §2.5/§5，ADR 0043 决策 7）。
 *
 * 为什么是工厂：委托与成交在协议上是**同一能力的两份配置**——同一份 REST 信封
 * （契约 rest §2.9.2）、同一套合并/重置/降级义务（契约 webui-ws §5 第 7–13 条），差别只有
 * 端点、WS 事件名、业务键与合并纯函数。`createTdListStore` 是这份行为的**唯一实现**
 * （#248 设计 §4.1 / 冻结取舍 5：禁止把委托 store 复制一份改成成交）。
 *
 * 行为边界（与委托列表落地时逐条对齐，勿在其中任一侧单独改语义）：
 * - **REST 是基线唯一来源**：首屏 + 滚动游标分页；`snapshot` / `td_recent` 都不能替代基线
 *   （契约 webui-ws §5 第 7 条）。
 * - **WS 只承载增量**：按业务键去重 + 同键 `seq` 大者胜，按 `sort_key` 字符串降序插入
 *   （不是简单前插），约 10Hz 批量合并（§5 第 8/9 条）。
 * - **有界窗口**：内存上限 `config.capacity` 行，超出丢最旧（ADR 0043 决策 7）；
 *   淘汰后向上滚动从首页重拉基线。
 * - **重置触发**：日切 / `data_changed{scope:"td"}` / 重连（td_recent 到达）/ 同账户 seq 回退
 *   → 清空 + 丢游标从首页重拉（§5 第 8/10 条）。
 * - **降级**：`available:false` 时进降级态 + 退避重试，绝不用 0 冒充数据（§5 第 13 条）。
 */
import { computed, ref } from 'vue'
import type { ComputedRef, Ref } from 'vue'
import type { TdDegradedReason, TdRecentPayload } from '@/types/api'

/** 交易列表行的共同字段（契约 rest §2.9.3：委托行与成交行共有） */
export interface TdListRow {
  account_id: string
  /** 交易日 YYYYMMDD */
  date: string
  /** 不透明版本号：合并用，不显示、不参与业务（契约 rest §2.9.6） */
  seq: number
  /** 排序键：字符串降序 = 列表顺序（契约 rest §2.9.6） */
  sort_key: string
}

/** 首屏/每页行数（契约 rest §2.9.2 limit 默认 200） */
export const TD_LIST_PAGE_SIZE = 200

/** 有界窗口容量（ADR 0043 决策 7 建议 2000；组件与测试从 store 取，单一常量来源） */
export const TD_LIST_WINDOW_CAPACITY = 2000

/** 距列表底部多少行开始预取下一页（工厂内部参数，无外部消费者 → 不导出） */
const TD_LIST_PREFETCH_THRESHOLD = 30

/** 批量合并间隔：约 10Hz（ADR 0043 决策 7；定时器可注入以便测试） */
export const TD_BATCH_INTERVAL_MS = 100

/** 降级重试基准延迟（指数退避：2s / 4s / 8s …） */
export const TD_RETRY_BASE_DELAY_MS = 2_000

/** 降级重试延迟上限（7×24 下不无限拉长，恢复要快） */
export const TD_RETRY_MAX_DELAY_MS = 30_000

/** 批量合并调度器（可注入：测试用可控回调替代浏览器定时器） */
export interface TdBatchScheduler {
  set(callback: () => void): unknown
  clear(handle: unknown): void
}

const defaultBatchScheduler: TdBatchScheduler = {
  set: callback => setTimeout(callback, TD_BATCH_INTERVAL_MS),
  clear: handle => clearTimeout(handle as ReturnType<typeof setTimeout>),
}

let batchScheduler: TdBatchScheduler = defaultBatchScheduler

/** 测试专用：注入批量调度器（生产路径使用默认 setTimeout） */
export function __setTdBatchSchedulerForTests(scheduler: TdBatchScheduler): void {
  batchScheduler = scheduler
}

/** 测试专用：恢复默认批量调度器 */
export function __resetTdBatchSchedulerForTests(): void {
  batchScheduler = defaultBatchScheduler
}

/**
 * 列表查询参数（委托与成交同形，契约 rest §2.9.2）。
 * 前端不做静默钳制：limit 越界/非整数由服务端回 400。
 */
export interface TdListQuery {
  limit?: number
  cursor?: string
  accounts?: string[]
}

/** 列表响应信封（委托与成交同形，契约 rest §2.9.2/§2.9.8） */
export type TdListResponse<Row> =
  | {
      available: true
      trading_day: string | null
      online_accounts: string[]
      items: Row[]
      next_cursor: string | null
    }
  | { available: false; reason: TdDegradedReason }

/** WS 事件名（`td_recent` 补发事件与实时事件同名，契约 webui-ws §2.5） */
export type TdListEventType = 'td_order_report' | 'td_trade_report'

/** 有界列表 store 的配置：同一份能力的端点/事件/键差异（#248 设计 §4.1） */
export interface TdListStoreConfig<Row extends TdListRow> {
  /** 拉取一页（注入 tdApi 方法） */
  fetch: (query: TdListQuery) => Promise<TdListResponse<Row>>
  /** WS 事件 type（td_recent 事件与实时事件同名） */
  eventType: TdListEventType
  /** 业务键（契约 rest §2.9.6）：委托 = order_id；成交 = account|date|exchange|trade_id */
  keyOf: (row: Row) => string | number
  /** 有界窗口容量 */
  capacity: number
}

/** 有界列表 store 的公开面（委托与成交 store 都是它的包装） */
export interface TdListStoreApi<Row extends TdListRow> {
  rows: Ref<Row[]>
  tradingDay: Ref<string | null>
  onlineAccounts: Ref<string[]>
  accountFilter: Ref<Set<string>>
  nextCursor: Ref<string | null>
  loading: Ref<boolean>
  loadingMore: Ref<boolean>
  initialized: Ref<boolean>
  error: Ref<string | null>
  available: Ref<boolean>
  degradedReason: Ref<TdDegradedReason | null>
  degradedText: ComputedRef<string>
  windowTruncated: Ref<boolean>
  atTop: Ref<boolean>
  newCount: Ref<number>
  allAccountsSelected: ComputedRef<boolean>
  effectiveAccounts: ComputedRef<string[]>
  isEmpty: ComputedRef<boolean>
  init: () => Promise<void>
  resetAndReload: (seed?: Row[]) => Promise<void>
  applyRowEvent: (row: Row) => void
  applyRecent: (payload: TdRecentLike | undefined) => void
  handleDataChanged: (scope: string) => Promise<void>
  setAccountFilter: (accounts: string[]) => Promise<void>
  selectAllAccounts: () => Promise<void>
  prefetchIfNeeded: (visibleEndIndex: number) => void
  prefetchNearTop: (firstVisibleIndex: number) => void
  setAtTop: (value: boolean) => void
  clearNewCount: () => void
  dispose: () => void
}

/** `td_recent` 载荷（类型真源 = schema 生成物的 `TdRecentPayload`，此处不另写形状） */
type TdRecentLike = TdRecentPayload

/**
 * store 在**组件/调用方**看到的形态：Pinia setup store 会把 ref 解包成普通值
 * （`rows` 从 `Ref<Row[]>` 变 `Row[]`）。由 {@link TdListStoreApi} 派生而非手写第二份，
 * 保证「工厂返回的字段」与「tab 消费的字段」永远同源。
 */
export type TdListStoreView<Row extends TdListRow = TdListRow> = {
  [K in keyof TdListStoreApi<Row>]:
    TdListStoreApi<Row>[K] extends Ref<infer V> ? V : TdListStoreApi<Row>[K]
}

/** 降级原因文案（契约 rest §2.9.8 的 reason 取值 → 用户可读中文） */
const DEGRADED_REASON_TEXT: Record<TdDegradedReason, string> = {
  not_found: '交易库尚未建立',
  schema_mismatch: '交易库版本与当前程序不符',
  open_failed: '交易库打开失败',
  busy: '交易库繁忙',
  query_failed: '交易数据查询失败',
}

/**
 * 有界列表合并（委托与成交的**唯一实现**，契约 webui-ws §5 第 8/9 条 + ADR 0043 决策 7）。
 *
 * 1. 业务键（`keyOf`）去重：同键只在 `seq` **更大**时覆盖（重复/乱序无害）；
 * 2. 新行按 `sort_key` 字符串降序插入——一次排序而非逐行 splice（10Hz 批量合并下更省）；
 * 3. 结果按 `capacity` 截断，丢弃 `sort_key` 最小的行（= 最旧，列表尾部）。
 *
 * 业务键的组成是委托与成交**唯一**的合并差异（委托 = order_id，成交 =
 * account|date|exchange|trade_id），因此它只出现在 `keyOf` 里，合并算法本身只有这一份。
 *
 * `sort_key` 相同时按业务键降序兜底（契约禁止解析 sort_key 结构，这里只比较值）；
 * 兜底按 `keyOf` 的返回类型分派：number 走数值降序（委托的 `order_id`）、string 走字典降序
 * （成交的复合键），与两份配置此前的行为逐一等价。
 *
 * 纯函数：不修改 `existing` / `incoming`。
 */
export function mergeTdRows<Row extends TdListRow>(
  existing: readonly Row[],
  incoming: readonly Row[],
  capacity: number,
  keyOf: (row: Row) => string | number,
): Row[] {
  if (incoming.length === 0) return existing.slice()
  const byKey = new Map<string | number, Row>()
  for (const row of existing) byKey.set(keyOf(row), row)
  for (const row of incoming) {
    const key = keyOf(row)
    const current = byKey.get(key)
    // 同键 seq 大者胜；相等视为重复消息（保留先到的，两者等价）
    if (!current || row.seq > current.seq) byKey.set(key, row)
  }
  const merged = Array.from(byKey.values())
  merged.sort((a, b) => {
    // 字符串降序（首项最大）
    if (a.sort_key !== b.sort_key) return a.sort_key > b.sort_key ? -1 : 1
    // 同 sort_key：按业务键降序兜底，保证顺序稳定可测
    const keyA = keyOf(a)
    const keyB = keyOf(b)
    if (typeof keyA === 'number' && typeof keyB === 'number') return keyB - keyA
    return String(keyA) > String(keyB) ? -1 : String(keyA) < String(keyB) ? 1 : 0
  })
  return merged.length > capacity ? merged.slice(0, capacity) : merged
}

/**
 * 创建一份有界列表 store 的行为实现（Pinia setup store 的函数体）。
 *
 * 返回的是**普通对象**（refs + 函数）：由 `stores/tdOrders.ts` / `stores/tdTrades.ts`
 * 直接交给 `defineStore` 的 setup 回调，从而复用同一实现、各自持有独立单例。
 */
export function createTdListStore<Row extends TdListRow>(
  config: TdListStoreConfig<Row>,
): TdListStoreApi<Row> {
  const { fetch, eventType, keyOf, capacity } = config

  // === 列表数据 ===
  /** 有界窗口内的行（按 sort_key 字符串降序：索引 0 最新） */
  const rows = ref<Row[]>([]) as Ref<Row[]>
  /** 列表作用域的当前交易日；null = 无在线账户（契约 rest §2.9.1） */
  const tradingDay = ref<string | null>(null)
  /** 在线（Ready）账户集合；账户筛选选项来源（契约 webui-ws §5 第 11 条） */
  const onlineAccounts = ref<string[]>([])
  /** 用户选中的账户（默认全选；空 Set = 未筛选但无在线账户） */
  const accountFilter = ref<Set<string>>(new Set())

  // === 分页/加载 ===
  const nextCursor = ref<string | null>(null)
  const loading = ref(false)
  const loadingMore = ref(false)
  const initialized = ref(false)
  const error = ref<string | null>(null)
  const windowTruncated = ref(false)

  // === 降级 ===
  const available = ref(true)
  const degradedReason = ref<TdDegradedReason | null>(null)

  // === 新数据气泡 ===
  const atTop = ref(true)
  const newCount = ref(0)

  // === 内部状态（非响应式）===
  /** 每个账户已见过的最大 seq（同账户 seq 回退 = 数据重置） */
  let maxSeqByAccount = new Map<string, number>()
  /** 是否由用户改过筛选（未改过时账户变化自动全选） */
  let filterTouched = false
  /** 待批量合并的 WS 增量 */
  let pendingRows: Row[] = []
  let pendingKeys = new Set<string | number>()
  let batchHandle: unknown = null
  let retryHandle: ReturnType<typeof setTimeout> | null = null
  let retryAttempt = 0
  let disposed = false

  // === 派生状态 ===
  /** 筛选后实际参与查询的账户（与在线集合求交；空数组 = 全选 → 不传 accounts） */
  const effectiveAccounts = computed<string[]>(() => {
    const online = onlineAccounts.value
    const selected = accountFilter.value
    if (online.length === 0) return []
    // 未改过筛选时视为全选（信封里的在线集合即全集）
    const chosen = filterTouched ? [...selected].filter(a => online.includes(a)) : online
    return chosen.sort()
  })

  /**
   * 是否全选（决定请求是否省略 accounts 参数）。
   * 空筛选等价于全选：契约 rest §2.9.2 缺省 = 全部在线账户，两者不能出现「UI 说筛了、
   * 请求却没带」的分裂状态。
   */
  const allAccountsSelected = computed<boolean>(() => {
    const online = onlineAccounts.value.length
    if (online === 0) return true
    const effective = effectiveAccounts.value.length
    return effective === 0 || effective === online
  })

  /**
   * 是否「无数据」空态：**成功**读到基线且列表为空。
   * 降级与请求失败都不是「无数据」——它们是不同的结论，不能让用户误以为当天没有记录
   * （契约 webui-ws §5 第 13 条：不用假数据/假空态冒充）。
   */
  const isEmpty = computed<boolean>(
    () => available.value && error.value === null && rows.value.length === 0)

  /** 降级提示文案（可用时为空串；统一降级提示条的唯一文案来源） */
  const degradedText = computed<string>(() => {
    if (available.value) return ''
    const reason = degradedReason.value
    const detail = reason ? DEGRADED_REASON_TEXT[reason] : '未知原因'
    return `交易数据暂不可用（${detail}），正在自动重试`
  })

  /** 请求用的 accounts 参数：全选时 undefined（契约 rest §2.9.2 缺省 = 全部在线账户） */
  function accountsParam(): string[] | undefined {
    if (allAccountsSelected.value) return undefined
    return effectiveAccounts.value
  }

  /** 新一天的交易日是否比当前更新（日切判定：date 为 "YYYYMMDD" 字符串，可直接比较） */
  function isNewTradingDay(date: string): boolean {
    return tradingDay.value !== null && date > tradingDay.value
  }

  /** 上一交易日的迟到行（日切后旧日数据不得混入，契约 webui-ws §5 第 10 条） */
  function isStaleTradingDay(date: string): boolean {
    return tradingDay.value !== null && date < tradingDay.value
  }

  function rebuildMaxSeq(): void {
    const map = new Map<string, number>()
    for (const row of rows.value) {
      const current = map.get(row.account_id)
      if (current === undefined || row.seq > current) map.set(row.account_id, row.seq)
    }
    maxSeqByAccount = map
  }

  /** 清空列表与游标（重置触发共用；不发起请求） */
  function clearList(): void {
    rows.value = []
    nextCursor.value = null
    windowTruncated.value = false
    newCount.value = 0
    pendingRows = []
    pendingKeys = new Set()
    maxSeqByAccount = new Map()
    if (batchHandle !== null) {
      batchScheduler.clear(batchHandle)
      batchHandle = null
    }
  }

  function clearRetry(): void {
    if (retryHandle !== null) {
      clearTimeout(retryHandle)
      retryHandle = null
    }
  }

  function scheduleRetry(): void {
    if (disposed) return
    clearRetry()
    const delay = Math.min(TD_RETRY_MAX_DELAY_MS, TD_RETRY_BASE_DELAY_MS * 2 ** retryAttempt)
    retryAttempt += 1
    retryHandle = setTimeout(() => {
      retryHandle = null
      void loadFirstPage(false)
    }, delay)
  }

  /**
   * 拉首页（`reset=true` 时先清空：日切 / data_changed / 重连 / 筛选变化的统一入口）。
   *
   * 响应判别：`available:false` 是**降级**（契约 rest §2.9.8），不是错误页——进降级态并退避重试；
   * HTTP 失败只记 error、保留已有行（不把列表清成假空态）。
   */
  async function loadFirstPage(reset: boolean, seed: Row[] = []): Promise<void> {
    if (disposed) return
    if (reset) clearList()
    loading.value = true
    try {
      const response = await fetch({ limit: TD_LIST_PAGE_SIZE, accounts: accountsParam() })
      // 请求在途时页面被卸载（dispose）：丢弃响应，别在无视图时写状态
      if (disposed) return
      if (response.available === false) {
        available.value = false
        degradedReason.value = response.reason
        scheduleRetry()
        return
      }
      available.value = true
      degradedReason.value = null
      retryAttempt = 0
      clearRetry()
      error.value = null
      tradingDay.value = response.trading_day
      onlineAccounts.value = response.online_accounts
      if (!filterTouched) accountFilter.value = new Set(response.online_accounts)
      nextCursor.value = response.next_cursor
      windowTruncated.value = false
      // REST 基线与已有行合并：同键 seq 大者胜（异步持久化窗口内 WS 行可能更新）。
      // seed = 建连补发事件（td_recent）：与基线走同一合并规则，同键 seq 大者胜——
      // 补发的旧行若基线已有更新版本，会被基线的 seq 覆盖，不会写回旧状态。
      rows.value = mergeTdRows(
        rows.value,
        seed.length > 0 ? [...response.items, ...seed] : response.items,
        capacity,
        keyOf,
      )
      rebuildMaxSeq()
    } catch (e) {
      if (disposed) return
      error.value = e instanceof Error ? e.message : String(e)
      // 交易库不可读之外的失败同样按退避重试（dzweb 重启/网络抖动）
      scheduleRetry()
    } finally {
      loading.value = false
      initialized.value = true
    }
  }

  /** 拉下一页（游标分页，禁 OFFSET；契约 rest §2.9.2） */
  async function loadMore(): Promise<void> {
    if (disposed || loading.value || loadingMore.value || !nextCursor.value) return
    loadingMore.value = true
    try {
      const response = await fetch({
        limit: TD_LIST_PAGE_SIZE,
        cursor: nextCursor.value,
        accounts: accountsParam(),
      })
      if (disposed) return
      if (response.available === false) {
        available.value = false
        degradedReason.value = response.reason
        scheduleRetry()
        return
      }
      available.value = true
      degradedReason.value = null
      error.value = null
      tradingDay.value = response.trading_day
      onlineAccounts.value = response.online_accounts
      if (!filterTouched) accountFilter.value = new Set(response.online_accounts)
      nextCursor.value = response.next_cursor
      rows.value = mergeTdRows(rows.value, response.items, capacity, keyOf)
      rebuildMaxSeq()
    } catch (e) {
      if (disposed) return
      error.value = e instanceof Error ? e.message : String(e)
    } finally {
      loadingMore.value = false
    }
  }

  /**
   * 页面挂载：拉首屏基线 + 初始化筛选。
   *
   * 每次挂载都重拉基线：离开页面期间 WS 增量仍在到达（store 是单例），且可能跨过日切，
   * 旧窗口不可信——重置重拉比"复用上一轮窗口"安全。首次挂载与再次挂载走同一路径。
   */
  async function init(): Promise<void> {
    // 重新挂载时解除 dispose（dispose 只停定时器，不终结 store 生命周期）
    disposed = false
    await resetAndReload()
  }

  /**
   * 重置并重拉：清空 + 丢游标（日切 / data_changed / 重连 / seq 回退）。
   * @param seed 重拉前先并入的补发事件（`td_recent`）：作为基线种子参与同一合并规则，
   *   基线到达后同键 seq 大者胜，不会把旧行写回。
   */
  async function resetAndReload(seed: Row[] = []): Promise<void> {
    await loadFirstPage(true, seed)
  }

  /**
   * 把一批增量合并进窗口（批量刷新的唯一写入点）。
   *
   * 重置判定（契约 webui-ws §5 第 8 条）：
   * - **日切**：新交易日的行 → 本地窗口混了两个交易日的风险，清空重拉；
   * - **seq 回退**：仅对**新业务键**判定。同一业务键的迟到/重复事件属于「重复/乱序无害」，
   *   由合并规则（seq 大者胜）吸收——否则一次重传就会误判成数据重置。
   *
   * 命中重置就整批丢弃并重拉基线：重置意味着本地窗口已不可信。
   */
  function ingest(incoming: Row[], countNew: boolean): boolean {
    if (incoming.length === 0) return false
    // 已知业务键 = 窗口内 + 本轮已并入但尚未刷新的（flushPending 会先清空 pendingRows，
    // 因此这里两个来源即完整）
    const knownKeys = new Set(rows.value.map(row => keyOf(row)))
    for (const key of pendingKeys) knownKeys.add(key)
    let resetNeeded = false
    const accepted: Row[] = []
    for (const row of incoming) {
      if (isNewTradingDay(row.date)) {
        resetNeeded = true
        break
      }
      if (isStaleTradingDay(row.date)) continue
      const seen = maxSeqByAccount.get(row.account_id)
      if (!knownKeys.has(keyOf(row)) && seen !== undefined && row.seq < seen) {
        // 同账户 seq 回退（新业务键却比水位旧）= 数据重置（进程重启/库重置），本地窗口不可信
        resetNeeded = true
        break
      }
      accepted.push(row)
    }
    if (resetNeeded) {
      pendingRows = []
      pendingKeys = new Set()
      void resetAndReload()
      return true
    }
    if (accepted.length === 0) return false

    const existingKeys = new Set(rows.value.map(row => keyOf(row)))
    let added = 0
    for (const row of accepted) {
      const key = keyOf(row)
      if (!existingKeys.has(key) && !pendingKeys.has(key)) {
        pendingKeys.add(key)
        added += 1
      }
      const seen = maxSeqByAccount.get(row.account_id)
      if (seen === undefined || row.seq > seen) maxSeqByAccount.set(row.account_id, row.seq)
    }

    const merged = mergeTdRows(rows.value, accepted, capacity, keyOf)
    // 容量判据：原有行 + 新增业务键 > 合并后行数 = 有行被淘汰（老数据滑出窗口，向上滚动需重拉）
    if (rows.value.length + added > merged.length) windowTruncated.value = true
    rows.value = merged
    if (countNew && !atTop.value && added > 0) newCount.value += added
    return false
  }

  function flushPending(): void {
    if (batchHandle !== null) {
      batchScheduler.clear(batchHandle)
      batchHandle = null
    }
    const batch = pendingRows
    pendingRows = []
    pendingKeys = new Set()
    if (!disposed) ingest(batch, true)
  }

  /** 事件行是否可消费（协议载荷可能被中间层污染；坏行丢弃而不是让 store 抛错） */
  function isValidEventRow(row: Row | undefined): row is Row {
    if (!row || typeof row !== 'object') return false
    const key = keyOf(row)
    return typeof key === 'number' || (typeof key === 'string' && key !== '')
  }

  /**
   * 收到一条 WS 增量事件（契约 webui-ws §2.5）：只入队，约 10Hz 批量合并后才写列表，
   * 避免逐条事件触发重渲染。
   */
  function applyRowEvent(row: Row): void {
    if (disposed || !isValidEventRow(row)) return
    pendingRows.push(row)
    if (batchHandle === null) {
      batchHandle = batchScheduler.set(() => {
        batchHandle = null
        flushPending()
      })
    }
  }

  /**
   * 收到建连补发 `td_recent`（契约 webui-ws §2.5）。
   *
   * 契约要求补发事件**与随后到达的实时事件使用同一合并规则**（重复/乱序无害，同键 seq
   * 大者胜），因此这里先把本 store 认领的事件按常规批量合并入队——不判 seq 回退、不判日切：
   * 补发环从旧到新，其 seq 本就可能低于断连前水位，判回退会把一次正常重连误判成数据重置。
   * 同时按 §5 第 7 条与 §6 重拉 REST 基线（补发有界，不能替代基线；基线到达后同键 seq
   * 大者胜，补发的旧行不会被写回）。
   *
   * 页面未打开时不做任何请求（store 是单例，无视图时不该产生流量）。
   */
  function applyRecent(payload: TdRecentLike | undefined): void {
    if (disposed || !initialized.value) return
    if (!Array.isArray(payload?.events)) return
    // 先合并补发事件（同一合并规则），再重拉基线——基线到达后按同键 seq 大者胜，补发的
    // 旧行不会被写回。clearList 会清空待合并队列，故补发行作为基线种子交给 loadFirstPage。
    const seeded: Row[] = []
    for (const event of payload.events) {
      if (event?.type !== eventType) continue
      const row = event.data as Row | undefined
      if (isValidEventRow(row)) seeded.push(row)
    }
    void resetAndReload(seeded)
  }

  /** `data_changed` 分发（契约 webui-ws §2.3）：scope=td 才清空重拉 */
  async function handleDataChanged(scope: string): Promise<void> {
    if (scope !== 'td') return
    if (!initialized.value) return
    await resetAndReload()
  }

  /** 账户筛选变化：重置游标从首页重拉（契约 webui-ws §5 第 11 条） */
  async function setAccountFilter(accounts: string[]): Promise<void> {
    filterTouched = true
    accountFilter.value = new Set(accounts)
    await resetAndReload()
  }

  /** 恢复默认全选（筛选面板「全选」） */
  async function selectAllAccounts(): Promise<void> {
    filterTouched = false
    accountFilter.value = new Set(onlineAccounts.value)
    await resetAndReload()
  }

  /**
   * 滚动接近底部时预取下一页（虚拟滚动可视区变化调用）。
   * `visibleEndIndex` 是当前最后一个可见行的索引（0 基）；与列表尾部距离小于阈值时拉取。
   */
  function prefetchIfNeeded(visibleEndIndex: number): void {
    if (!nextCursor.value || loading.value || loadingMore.value) return
    if (visibleEndIndex < rows.value.length - TD_LIST_PREFETCH_THRESHOLD) return
    void loadMore()
  }

  /** 窗口淘汰后向上滚动：从首页重拉基线（ADR 0043 决策 7） */
  function prefetchNearTop(firstVisibleIndex: number): void {
    if (!windowTruncated.value) return
    if (firstVisibleIndex > TD_LIST_PREFETCH_THRESHOLD) return
    void resetAndReload()
  }

  /** 视口是否在顶部（决定新行是进气泡计数还是直接展示） */
  function setAtTop(value: boolean): void {
    atTop.value = value
    if (value) newCount.value = 0
  }

  function clearNewCount(): void {
    newCount.value = 0
  }

  /** 页面卸载：清定时器，避免离开页面后继续请求 */
  function dispose(): void {
    disposed = true
    clearRetry()
    if (batchHandle !== null) {
      batchScheduler.clear(batchHandle)
      batchHandle = null
    }
    pendingRows = []
    pendingKeys = new Set()
  }

  return {
    rows,
    tradingDay,
    onlineAccounts,
    accountFilter,
    nextCursor,
    loading,
    loadingMore,
    initialized,
    error,
    available,
    degradedReason,
    degradedText,
    windowTruncated,
    atTop,
    newCount,
    allAccountsSelected,
    effectiveAccounts,
    isEmpty,
    init,
    resetAndReload,
    applyRowEvent,
    applyRecent,
    handleDataChanged,
    setAccountFilter,
    selectAllAccounts,
    prefetchIfNeeded,
    prefetchNearTop,
    setAtTop,
    clearNewCount,
    dispose,
  }
}
