import { defineStore } from 'pinia'
import { tdApi } from '@/api/td'
import {
  TD_LIST_PAGE_SIZE,
  TD_LIST_PREFETCH_THRESHOLD,
  createTdListStore,
} from '@/composables/tdListStore'
import type { TdListQuery, TdListResponse } from '@/composables/tdListStore'
import { TD_ORDERS_WINDOW_CAPACITY } from '@/composables/tdOrderView'
import type { TdOrderRow } from '@/types/api'

/**
 * 交易数据 · 委托列表 store（契约 rest §2.9、契约 webui-ws §2.5/§5，ADR 0043 决策 7）。
 *
 * 行为实现**只有一份**：{@link createTdListStore}（委托与成交是同一能力的两份配置，
 * #248 设计 §4.1 / 冻结取舍 5）。本文件只提供委托的差异面：
 * - 端点 `GET /api/td/orders`（契约 rest §2.9）；
 * - WS 事件名 `td_order_report`（契约 webui-ws §2.5）；
 * - 业务键 `order_id`（契约 rest §2.9.6）——合并算法本身在 `mergeTdRows`，不在这里。
 *
 * 分页/合并/重置/降级/气泡等语义全部由工厂负责（实现契约见 `composables/tdListStore.ts`
 * 顶部注释），这里**不得**重复实现——两份列表一旦各写一份，合并语义必然漂移。
 */

/** 首屏/每页行数（契约 rest §2.9.2 limit 默认 200） */
export const TD_ORDERS_PAGE_SIZE = TD_LIST_PAGE_SIZE

/** 有界窗口容量（ADR 0043 决策 7 建议 2000；组件与测试从 store 取，单一常量来源） */
export { TD_ORDERS_WINDOW_CAPACITY }

/** 距列表底部多少行开始预取下一页 */
export const TD_ORDERS_PREFETCH_THRESHOLD = TD_LIST_PREFETCH_THRESHOLD

/**
 * 批量合并间隔 / 降级退避参数 / 测试注入点：均为工厂机制（委托与成交共用同一份），
 * 从本模块转出以保持既有导入面不变。
 */
export {
  TD_BATCH_INTERVAL_MS,
  TD_RETRY_BASE_DELAY_MS,
  TD_RETRY_MAX_DELAY_MS,
  __setTdBatchSchedulerForTests,
  __resetTdBatchSchedulerForTests,
} from '@/composables/tdListStore'
export type { TdBatchScheduler } from '@/composables/tdListStore'

/**
 * 委托列表 store。
 * 方法名 `applyOrderEvent` 是工厂 `applyRowEvent` 的别名（与成交 store 的 `applyTradeEvent`
 * 对称：调用点按业务命名，实现只有工厂一份）。
 */
export const useTdOrdersStore = defineStore('tdOrders', () => {
  const store = createTdListStore<TdOrderRow>({
    fetch: (query: TdListQuery) =>
      tdApi.getOrders(query) as Promise<TdListResponse<TdOrderRow>>,
    eventType: 'td_order_report',
    keyOf: row => row.order_id,
    capacity: TD_ORDERS_WINDOW_CAPACITY,
  })

  /**
   * 收到一条 `td_order_report`（契约 webui-ws §2.5）：只入队，约 10Hz 批量合并后才写列表，
   * 避免逐条事件触发重渲染（ADR 0043 决策 7）。
   */
  function applyOrderEvent(row: TdOrderRow): void {
    store.applyRowEvent(row)
  }

  return { ...store, applyOrderEvent }
})
