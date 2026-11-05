/**
 * 持仓的展示与合并纯函数（契约 rest §2.9.2/§2.9.4/§2.9.6，契约 webui-ws §5 第 8/11/12 条）。
 *
 * 为什么**没有**复用 `tdListStore.mergeTdRows`：列表工厂的合并语义建立在委托/成交的
 * 「追加流」前提上，持仓是**绝对态**，两者有三处硬冲突（不是风格偏好）：
 * 1. 工厂要求行带 `sort_key` 并按它字符串降序插入；持仓行**没有** `sort_key`（契约 rest
 *    §2.9.4 字段表），列表顺序是服务端 `(account_id, instrument_id, direction)` 升序；
 * 2. 工厂按 `capacity` 截断「最旧」行（有界窗口）；持仓没有任何行可丢——截断一行等于
 *    向用户宣告一间并不存在的空仓（契约 webui-ws §5 第 13 条禁止用假数据/假空态冒充）；
 * 3. 工厂的账户筛选选项来源是信封 `online_accounts`；持仓的账户全集含离线（§5 第 11 条）。
 *
 * 因此持仓的合并是**更小的一份**：业务键 `(account_id, instrument_id, direction)` 去重 +
 * 同键 `seq` 大者胜 + 按服务端顺序整表排序。它不是委托 store 的复制——没有有界窗口、
 * 没有 sort_key 插入、没有分页游标语义上的排序依赖。精度格式化直接复用 `tdOrderView`
 * （契约 webui-ws §5 第 12 条：口径只能有一个）。
 */
import { TD_UNAVAILABLE_TEXT, formatTdPrice } from './tdOrderView'
import type { TdPositionRow } from '@/types/api'

/** 方向在游标坐标里的全序（契约 rest §2.9.2）：Short(-1) < Net(0) < Long(1) */
const DIRECTION_ORDER: Record<TdPositionRow['direction'], number> = {
  Short: -1,
  Net: 0,
  Long: 1,
}

/**
 * 持仓业务键（契约 rest §2.9.6）：`account_id|instrument_id|direction`。
 *
 * 同一合约的多空**是两条独立记录**（方向入键），不得按合约单独合并——多空锁仓是真实现象，
 * 轧差成一条会丢掉方向明细与今昨拆分。
 */
export function tdPositionBusinessKey(row: TdPositionRow): string {
  return `${row.account_id}|${row.instrument_id}|${row.direction}`
}

/**
 * 服务端列表顺序（契约 rest §2.9.2）：`(account_id, instrument_id, direction)` 升序，
 * direction 全序 Short < Net < Long。字符串用原生比较（服务端 SQLite TEXT 为字节序；
 * 账户/合约 ID 为 ASCII，与 UTF-16 码元序一致），不做 locale 排序。
 */
export function comparePositionRows(a: TdPositionRow, b: TdPositionRow): number {
  if (a.account_id !== b.account_id) return a.account_id < b.account_id ? -1 : 1
  if (a.instrument_id !== b.instrument_id) return a.instrument_id < b.instrument_id ? -1 : 1
  return DIRECTION_ORDER[a.direction] - DIRECTION_ORDER[b.direction]
}

/**
 * 合并一批持仓行到现有集合（契约 webui-ws §5 第 8 条 + 契约 rest §2.9.6）。
 *
 * - 业务键去重，同键 **`seq` 大者胜**（与到达顺序无关；重复/乱序无害）；
 * - 结果按服务端顺序排序（不是 sort_key，也不是到达顺序），保证 REST 分页与 WS 增量
 *   拼出的列表顺序一致；
 * - **不做容量截断**：持仓是绝对态，丢行就是假空仓（与列表工厂的有界窗口刻意不同）。
 *
 * 纯函数：不修改 `existing` / `incoming`。
 */
export function mergePositionRows(
  existing: readonly TdPositionRow[],
  incoming: readonly TdPositionRow[],
): TdPositionRow[] {
  if (incoming.length === 0) return existing.slice()
  const byKey = new Map<string, TdPositionRow>()
  for (const row of existing) byKey.set(tdPositionBusinessKey(row), row)
  for (const row of incoming) {
    const key = tdPositionBusinessKey(row)
    const current = byKey.get(key)
    // 同键 seq 大者胜；相等视为重复消息（保留先到的，两者等价）
    if (!current || row.seq > current.seq) byKey.set(key, row)
  }
  return Array.from(byKey.values()).sort(comparePositionRows)
}

/**
 * 合并 REST 基线与本地已有行（绝对态语义下的**替换式**合并）。
 *
 * REST 是权威快照：基线没有返回的业务键 = 该持仓已消失（查询快照整组原子替换，
 * 契约 td-data-sync §3），**必须丢掉本地对应行**，否则会留下幽灵持仓。
 * 唯一的例外是查询与推送的竞态：交易库异步持久化（ADR 0042），WS 增量可能比基线更新却
 * 不在基线里（基线查询先于它提交）。因此：
 * - 两侧都有的业务键 → 同键 `seq` 大者胜（契约 webui-ws §5 第 8 条）；
 * - 只在本地有的业务键 → 仅当它的 `seq` **高于该账户基线水位**时保留（说明它比快照更新），
 *   否则丢弃（旧快照，基线已覆盖）。
 *
 * `seq` 是账户级、跨类型单调的（契约 td-data-sync §1），所以「本地 seq > 基线最大 seq」
 * 可以安全地表达「本地行比这次快照更新」，不需要时间戳。
 */
export function reconcilePositionBaseline(
  baseline: readonly TdPositionRow[],
  live: readonly TdPositionRow[],
): TdPositionRow[] {
  // 即使没有本地行也要排序：服务端顺序是契约承诺，但前端不依赖它（分页拼装可能乱序）
  if (live.length === 0) return baseline.slice().sort(comparePositionRows)
  const baselineByKey = new Map<string, TdPositionRow>()
  const baselineSeqByAccount = new Map<string, number>()
  for (const row of baseline) {
    baselineByKey.set(tdPositionBusinessKey(row), row)
    const current = baselineSeqByAccount.get(row.account_id)
    if (current === undefined || row.seq > current) baselineSeqByAccount.set(row.account_id, row.seq)
  }
  const survivors: TdPositionRow[] = []
  for (const row of live) {
    const key = tdPositionBusinessKey(row)
    const fromBaseline = baselineByKey.get(key)
    if (fromBaseline !== undefined) {
      // 两侧都有：seq 大者胜；相等视为同一版本（保留基线，来自权威快照）
      if (row.seq > fromBaseline.seq) survivors.push(row)
      continue
    }
    const watermark = baselineSeqByAccount.get(row.account_id)
    // 本地新键但 seq 不高于快照水位 = 基线查询时它还不存在或已被覆盖 → 丢弃
    if (watermark !== undefined && row.seq > watermark) survivors.push(row)
  }
  const merged = [...baseline]
  for (const row of survivors) {
    const key = tdPositionBusinessKey(row)
    const index = merged.findIndex(item => tdPositionBusinessKey(item) === key)
    if (index >= 0) merged[index] = row
    else merged.push(row)
  }
  return merged.sort(comparePositionRows)
}

/** 方向文案（契约 rest §2.9.3/§2.9.4：Long / Short / Net） */
export function tdPositionDirectionText(direction: TdPositionRow['direction']): string {
  switch (direction) {
    case 'Long': return '多'
    case 'Short': return '空'
    case 'Net': return '净'
    default: return String(direction)
  }
}

/**
 * 数据日期显示：契约的 `date` 是 `"YYYYMMDD"` 字符串，展示成 `YYYY-MM-DD` 便于阅读；
 * 非法 / 缺失返回占位（不猜测）。该日期是**账户锚点日**，可能早于信封 `trading_day`。
 */
export function formatTdDate(date: string): string {
  if (!/^\d{8}$/.test(date)) return TD_UNAVAILABLE_TEXT
  return `${date.slice(0, 4)}-${date.slice(4, 6)}-${date.slice(6, 8)}`
}

/** 带符号的手数：正数显式 `+`（与账户净仓的既有展示口径一致），其余原样 */
export function formatSignedVolume(volume: number): string {
  return volume > 0 ? `+${volume}` : String(volume)
}

/** 单方向（多 / 空）的明细：持仓行的量/今昨/冻结/均价 */
export interface TdPositionSide {
  volume: number
  today_volume: number
  yd_volume: number
  frozen_volume: number
  price: number
  price_decimals: number
}

/**
 * 面板展示行：**一行 = (account_id, instrument_id)**（用户确认的界面口径）。
 *
 * 契约的持仓行是 `(account, instrument, direction)` 三元组；面板把同一合约的多空两条
 * 合成一行，默认只显示轧差后的账户净仓，多/空明细放进可展开详情列。
 */
export interface TdPositionGroup {
  /** 展示行 key = `account_id|instrument_id` */
  key: string
  account_id: string
  instrument_id: string
  exchange_id: string
  /** 行内数据日期：多空两条的锚点日取较大者（同账户通常相同） */
  date: string
  /** 账户净仓 = 多总量 − 空总量（正为多、负为空、零为平） */
  netVolume: number
  /** 多方向明细；无多仓 = null（详情列显示占位） */
  long: TdPositionSide | null
  /** 空方向明细；无空仓 = null */
  short: TdPositionSide | null
  /**
   * 存在 direction=Net 的行（多柜台兼容口径；本仓 CTP 网关把净持仓映射为 Long，
   * 因此正常不会出现）。`Net` 行按其 volume 计入净仓，展示时不做多空拆分。
   */
  hasNetDirection: boolean
}

/** 取一条持仓行的方向明细 */
function toSide(row: TdPositionRow): TdPositionSide {
  return {
    volume: row.volume,
    today_volume: row.today_volume,
    yd_volume: row.yd_volume,
    frozen_volume: row.frozen_volume,
    price: row.price,
    price_decimals: row.price_decimals,
  }
}

/**
 * 把 `(account, instrument, direction)` 持仓行按 `(account, instrument)` 分组（纯函数）。
 *
 * - 净仓 = Σ(Long: +volume) + Σ(Short: −volume) + Σ(Net: +volume)；
 *   `Net` 行按正值计入（该方向在本仓网关不会产生，仅为契约完整性保留）；
 * - 多/空明细取该方向的行；「两条同方向记录」不会出现（业务键含 direction），
 *   若服务端异常返回两条，取 `seq` 较大者（与合并规则一致）；
 * - 输出顺序沿用输入的持仓排序（`mergePositionRows` 已按服务端顺序排好），保证面板稳定。
 */
export function groupPositionRows(rows: readonly TdPositionRow[]): TdPositionGroup[] {
  const byPair = new Map<string, TdPositionGroup>()
  const seqBySide = new Map<string, number>()
  /** 当前分组来自 Net 方向行的量（仅在该组没有多/空明细时使用） */
  let netDirectionVolume: number | null = null
  for (const row of rows) {
    const key = `${row.account_id}|${row.instrument_id}`
    const current = byPair.get(key)
    const group: TdPositionGroup = current ?? {
      key,
      account_id: row.account_id,
      instrument_id: row.instrument_id,
      exchange_id: row.exchange_id,
      date: row.date,
      netVolume: 0,
      long: null,
      short: null,
      hasNetDirection: false,
    }
    if (!current) {
      byPair.set(key, group)
      netDirectionVolume = null
    }
    // 数据日期取较大者（"YYYYMMDD" 字符串可直接比较）
    if (row.date > group.date) group.date = row.date
    // exchange_id 以首条为准（同一合约的交易所不会不同；不做猜测性合并）
    // 净仓在循环结束后由**选中的**多/空行重算：先累加再判重会把同方向重复行计两次
    const sideKey = `${key}|${row.direction}`
    const seenSeq = seqBySide.get(sideKey)
    if (seenSeq !== undefined && row.seq <= seenSeq) continue
    seqBySide.set(sideKey, row.seq)
    if (row.direction === 'Long') group.long = toSide(row)
    else if (row.direction === 'Short') group.short = toSide(row)
    else {
      // Net 方向：按正值记为净仓。本仓 CTP 网关把净持仓映射成 Long，正常不会出现；
      // 保留它是为了契约完整性（其它柜台可能用 Net 口径）。
      group.hasNetDirection = true
      netDirectionVolume = row.volume
    }
    // 净仓口径：有多/空明细时用明细轧差（明细是权威）；只有 Net 行时用它的值。
    // 两种口径同时出现属柜台/数据异常——明细优先，且不让两者相加（相加会凭空放大净仓）。
    if (group.long !== null || group.short !== null) {
      group.netVolume = (group.long?.volume ?? 0) - (group.short?.volume ?? 0)
    } else if (netDirectionVolume !== null) {
      group.netVolume = netDirectionVolume
    }
  }
  return Array.from(byPair.values())
}

/** 详情列（默认隐藏；用户勾选后显示）：多方 5 列 + 空方 5 列，共 10 列（用户确认口径） */
export const TD_POSITION_DETAIL_COLUMNS = [
  '多总', '多今', '多昨', '多冻结', '多均价',
  '空总', '空今', '空昨', '空冻结', '空均价',
] as const

/** 单方向明细 → 详情列 5 个单元格值；该方向无持仓时全部显示占位（不显示 0） */
function sideDetailTexts(side: TdPositionSide | null): string[] {
  if (side === null) {
    return Array.from({ length: 5 }, () => TD_UNAVAILABLE_TEXT)
  }
  return [
    String(side.volume),
    String(side.today_volume),
    String(side.yd_volume),
    String(side.frozen_volume),
    formatTdPrice(side.price, side.price_decimals),
  ]
}

/** 详情列单元格值（顺序 = {@link TD_POSITION_DETAIL_COLUMNS}） */
export function positionDetailTexts(group: TdPositionGroup): string[] {
  return [...sideDetailTexts(group.long), ...sideDetailTexts(group.short)]
}
