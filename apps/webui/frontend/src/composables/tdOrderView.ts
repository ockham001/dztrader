/**
 * 委托列表的展示与合并纯函数（契约 rest §2.9.3 / §2.9.6，契约 webui-ws §5 第 8/9/12 条）。
 *
 * 为什么单独成模块：精度格式化、sort_key 插入、业务键合并都是**纯函数**，store 与组件共用同一
 * 实现，单测可直接覆盖。口径全部来自契约，不在组件里另写一份：
 * - 精度：只用行内 `price_decimals` / `avg_price_decimals` 做 `toFixed`，不推断小数位（§2.9.6）；
 * - 排序：只做 `sort_key` 字符串比较，**不解析其结构**（§2.9.6 性质 3）；
 * - 合并：业务键 `order_id` 去重 + 同键 `seq` 大者胜，与到达顺序无关（webui-ws §5 第 8 条）。
 */
import { TD_LIST_WINDOW_CAPACITY, mergeTdRows } from './tdListStore'
import type { TdOrderRow } from '@/types/api'

/** 数值不可用 / 无数据时的占位（不用 0 冒充，契约 webui-ws §5 第 13 条） */
export const TD_UNAVAILABLE_TEXT = '—'

/** 委托列表有界窗口容量（ADR 0043 决策 7 建议 2000；数值单源在 tdListStore，成交同口径） */
export const TD_ORDERS_WINDOW_CAPACITY = TD_LIST_WINDOW_CAPACITY

/** 报单时间 0–86399（距午夜秒数，契约 rest §2.9.3）的上界 */
const SECONDS_PER_DAY = 86_400

/**
 * 委托价显示：`price.toFixed(price_decimals)`（契约 webui-ws §5 第 12 条）。
 * 非有限值显示占位——不把 NaN / Infinity 渲染成价格。
 */
export function formatTdPrice(price: number, decimals: number): string {
  if (!Number.isFinite(price) || !Number.isFinite(decimals)) return TD_UNAVAILABLE_TEXT
  return price.toFixed(clampDecimals(decimals))
}

/**
 * 成交均价显示：`avg_price.toFixed(avg_price_decimals)`；
 * `avg_price === null`（volume_traded=0，无成交）显示占位，**不显示 0**。
 */
export function formatTdAvgPrice(avgPrice: number | null, decimals: number): string {
  if (avgPrice === null || !Number.isFinite(avgPrice) || !Number.isFinite(decimals)) {
    return TD_UNAVAILABLE_TEXT
  }
  return avgPrice.toFixed(clampDecimals(decimals))
}

/** toFixed 的参数必须在 [0, 100]；服务端给的精度落在 [0, 8]，越界值按不可用处理而非抛错 */
function clampDecimals(decimals: number): number {
  return Math.min(100, Math.max(0, Math.trunc(decimals)))
}

/**
 * 报单时间格式化：距午夜秒数 → `HH:MM:SS`。
 * 契约明文「不做时区转换」（rest §2.9.3）；越界 / 非整数视为不可用，不猜测。
 */
export function formatTdTime(seconds: number): string {
  if (!Number.isInteger(seconds) || seconds < 0 || seconds >= SECONDS_PER_DAY) {
    return TD_UNAVAILABLE_TEXT
  }
  const hh = String(Math.floor(seconds / 3600)).padStart(2, '0')
  const mm = String(Math.floor((seconds % 3600) / 60)).padStart(2, '0')
  const ss = String(seconds % 60).padStart(2, '0')
  return `${hh}:${mm}:${ss}`
}

/** 方向文案（契约 rest §2.9.3：Long / Short / Net） */
export function tdDirectionText(direction: TdOrderRow['direction']): string {
  switch (direction) {
    case 'Long': return '买'
    case 'Short': return '卖'
    case 'Net': return '净'
    default: return String(direction)
  }
}

/** 开平仓文案（契约 rest §2.9.3） */
export function tdPositionEffectText(effect: TdOrderRow['position_effect']): string {
  switch (effect) {
    case 'Auto': return '自动'
    case 'Open': return '开仓'
    case 'Close': return '平仓'
    case 'CloseToday': return '平今'
    case 'CloseYesterday': return '平昨'
    default: return String(effect)
  }
}

/** 委托状态文案（契约 rest §2.9.3） */
export function tdOrderStatusText(status: TdOrderRow['status']): string {
  switch (status) {
    case 'Submitting': return '提交中'
    case 'NotTraded': return '未成交'
    case 'PartTraded': return '部分成交'
    case 'AllTraded': return '全部成交'
    case 'Cancelled': return '已撤单'
    case 'Rejected': return '已拒绝'
    default: return String(status)
  }
}

/** 价格类型文案（契约 rest §2.9.3；FAK/FOK 为行业通用缩写，保留原文） */
export function tdPriceTypeText(priceType: TdOrderRow['price_type']): string {
  switch (priceType) {
    case 'Limit': return '限价'
    case 'Market': return '市价'
    case 'Stop': return '止损'
    case 'Fak': return 'FAK'
    case 'Fok': return 'FOK'
    case 'Rfq': return '询价'
    default: return String(priceType)
  }
}

/** 业务键：委托 = order_id（契约 rest §2.9.6） */
function businessKey(row: TdOrderRow): number {
  return row.order_id
}

/**
 * 合并一批增量行到现有窗口（契约 webui-ws §5 第 8/9 条 + ADR 0043 决策 7 有界窗口）。
 *
 * **薄委托**：算法本身只有一份，在 `tdListStore.mergeTdRows`（委托与成交共用）。本函数保留
 * 既有导出与签名（调用方与测试不动），只绑定委托的业务键 `order_id` 与默认容量。
 */
export function mergeOrderRows(
  existing: readonly TdOrderRow[],
  incoming: readonly TdOrderRow[],
  capacity: number = TD_ORDERS_WINDOW_CAPACITY,
): TdOrderRow[] {
  return mergeTdRows(existing, incoming, capacity, businessKey)
}
