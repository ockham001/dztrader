/**
 * 成交列表的展示与合并纯函数（契约 rest §2.9.3 / §2.9.6，契约 webui-ws §5 第 8/9 条）。
 *
 * 与委托列表是**同一能力的两份配置**（ADR 0043 决策 7 / #248 设计 §4.1）：
 * - 合并口径与 `tdOrderView.mergeOrderRows` 完全一致（业务键去重 + 同键 seq 大者胜 +
 *   sort_key 字符串降序 + 有界截断），差别只在业务键的组成；
 * - 精度格式化、时间格式化、方向/开平文案直接复用 `tdOrderView`，
 *   **不为成交另写一份**（契约 webui-ws §5 第 12 条：口径只能有一个）。
 */
import { TD_LIST_WINDOW_CAPACITY } from './tdListStore'
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
 * 1. 业务键（{@link tdTradeBusinessKey}）去重：同键只在 `seq` **更大**时覆盖（重复/乱序无害）；
 * 2. 新行按 `sort_key` 降序插入——一次排序而非逐行 splice（10Hz 批量合并下更省）；
 * 3. 结果按 `capacity` 截断，丢弃 `sort_key` 最小的行（= 最旧，列表尾部）。
 *
 * `sort_key` 相同时按业务键降序，仅是**稳定排序的兜底**（契约禁止解析 sort_key 结构，
 * 这里只做字符串比较）；同一作用域内 sort_key 已含业务键，正常不会进入该分支。
 *
 * 纯函数：不修改 `existing` / `incoming`。
 */
export function mergeTradeRows(
  existing: readonly TdTradeRow[],
  incoming: readonly TdTradeRow[],
  capacity: number = TD_TRADES_WINDOW_CAPACITY,
): TdTradeRow[] {
  if (incoming.length === 0) return existing.slice()
  const byKey = new Map<string, TdTradeRow>()
  for (const row of existing) byKey.set(tdTradeBusinessKey(row), row)
  for (const row of incoming) {
    const key = tdTradeBusinessKey(row)
    const current = byKey.get(key)
    // 同键 seq 大者胜；相等视为重复消息（保留先到的，两者等价）
    if (!current || row.seq > current.seq) byKey.set(key, row)
  }
  const merged = Array.from(byKey.values())
  merged.sort((a, b) => {
    if (a.sort_key !== b.sort_key) return a.sort_key > b.sort_key ? -1 : 1
    const keyA = tdTradeBusinessKey(a)
    const keyB = tdTradeBusinessKey(b)
    return keyA > keyB ? -1 : keyA < keyB ? 1 : 0
  })
  return merged.length > capacity ? merged.slice(0, capacity) : merged
}
