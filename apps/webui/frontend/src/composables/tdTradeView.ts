/**
 * 成交列表的展示与合并纯函数（契约 rest §2.9.3 / §2.9.6，契约 webui-ws §5 第 8/9 条）。
 *
 * 与委托列表是**同一能力的两份配置**（ADR 0043 决策 7 / #248 设计 §4.1）：业务键只是本模块的
 * 差异面，合并算法本身唯一实现在 `tdListStore.mergeTdRows`；精度格式化、时间格式化、
 * 方向/开平文案直接复用 `tdOrderView`（契约 webui-ws §5 第 12 条：口径只能有一个）。
 */
import { TD_LIST_WINDOW_CAPACITY, mergeTdRows } from './tdListStore'
import type { TdTradeRow } from '@/types/api'

/** 成交列表有界窗口容量（ADR 0043 决策 7：与委托同一数值口径） */
export const TD_TRADES_WINDOW_CAPACITY = TD_LIST_WINDOW_CAPACITY

/**
 * 成交业务键（契约 rest §2.9.6）：`account_id|date|exchange_id|trade_id`。
 *
 * `trade_id` 只在交易所内唯一，因此必须与 account / date / exchange 组合才能全局唯一
 * （多交易所同号不合并、跨账户不合并——这也是 #248 验收里明文要求的两条）。
 */
export function tdTradeBusinessKey(row: TdTradeRow): string {
  return `${row.account_id}|${row.date}|${row.exchange_id}|${row.trade_id}`
}

/**
 * 合并一批成交增量到现有窗口（契约 webui-ws §5 第 8/9 条 + ADR 0043 决策 7 有界窗口）。
 *
 * **薄委托**：算法本身只有一份，在 `tdListStore.mergeTdRows`（委托与成交共用）。本函数保留
 * 既有导出与签名（调用方与测试不动），只绑定成交的业务键 {@link tdTradeBusinessKey} 与默认容量。
 */
export function mergeTradeRows(
  existing: readonly TdTradeRow[],
  incoming: readonly TdTradeRow[],
  capacity: number = TD_TRADES_WINDOW_CAPACITY,
): TdTradeRow[] {
  return mergeTdRows(existing, incoming, capacity, tdTradeBusinessKey)
}
