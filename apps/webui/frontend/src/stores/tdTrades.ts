import { defineStore } from 'pinia'
import { tdApi } from '@/api/td'
import {
  TD_LIST_PAGE_SIZE,
  TD_LIST_WINDOW_CAPACITY,
  createTdListStore,
} from '@/composables/tdListStore'
import type { TdListQuery, TdListResponse } from '@/composables/tdListStore'
import {
  TD_TRADES_WINDOW_CAPACITY,
  mergeTradeRows,
  tdTradeBusinessKey,
} from '@/composables/tdTradeView'
import type { TdTradeRow } from '@/types/api'

/**
 * 交易数据 · 成交列表 store（契约 rest §2.9、契约 webui-ws §2.5/§5，ADR 0043 决策 7）。
 *
 * 行为实现**只有一份**：{@link createTdListStore}（与委托 store 是同一工厂的两份配置，
 * #248 设计 §4.1 / 冻结取舍 5）。本文件只提供成交的差异面：
 * - 端点 `GET /api/td/trades`（契约 rest §2.9）；
 * - WS 事件名 `td_trade_report`（契约 webui-ws §2.5）；
 * - 业务键 `account_id|date|exchange_id|trade_id` 与 {@link mergeTradeRows}
 *   （契约 rest §2.9.6，唯一实现在 `composables/tdTradeView`）。
 *
 * 分页/合并/重置/降级/气泡等语义全部由工厂负责，这里**不得**重复实现。
 */

/** 首屏/每页行数（契约 rest §2.9.2 limit 默认 200，与委托同一口径） */
export const TD_TRADES_PAGE_SIZE = TD_LIST_PAGE_SIZE

/** 有界窗口容量（ADR 0043 决策 7；组件与测试从 store 取，单一常量来源） */
export { TD_TRADES_WINDOW_CAPACITY, TD_LIST_WINDOW_CAPACITY }

/**
 * 测试注入点与批量间隔：委托与成交**共用同一注入点**（批次调度是工厂的机制，不是某一份
 * 配置的属性），从本模块转出是为了让成交侧用例只 import 自己那一个 store 模块。
 */
export { TD_BATCH_INTERVAL_MS, TD_RETRY_BASE_DELAY_MS, TD_RETRY_MAX_DELAY_MS } from '@/composables/tdListStore'
export {
  __setTdBatchSchedulerForTests,
  __resetTdBatchSchedulerForTests,
} from '@/composables/tdListStore'

/**
 * 成交列表 store。
 * 方法名 `applyTradeEvent` 是工厂 `applyRowEvent` 的别名（与委托 store 的
 * `applyOrderEvent` 对称：调用点按业务命名，实现只有工厂一份）。
 */
export const useTdTradesStore = defineStore('tdTrades', () => {
  const store = createTdListStore<TdTradeRow>({
    fetch: (query: TdListQuery) =>
      tdApi.getTrades(query) as Promise<TdListResponse<TdTradeRow>>,
    eventType: 'td_trade_report',
    keyOf: tdTradeBusinessKey,
    merge: mergeTradeRows,
    capacity: TD_TRADES_WINDOW_CAPACITY,
  })

  /**
   * 收到一条 `td_trade_report`（契约 webui-ws §2.5）：只入队，约 10Hz 批量合并后才写列表，
   * 避免逐条事件触发重渲染（ADR 0043 决策 7）。
   */
  function applyTradeEvent(row: TdTradeRow): void {
    store.applyRowEvent(row)
  }

  return { ...store, applyTradeEvent }
})
