import { api } from './client'
import type { TdListQuery } from '@/composables/tdListStore'
import type { TdOrdersResponse, TdStatsResponse, TdTradesResponse } from '@/types/api'

/**
 * 交易数据列表查询参数（契约 rest §2.9.2）：委托与成交**同形**，类型单源在
 * `composables/tdListStore`（列表 store 工厂的 TdListQuery），此处只转出便于调用方引用。
 *
 * - `limit`：默认 200，允许 1–1000；越界或非整数由服务端回 400，前端不做静默钳制。
 * - `cursor`：上一页的 `next_cursor`；与查询作用域（端点 + accounts + 交易日）绑定，
 *   作用域变化必须从首页开始（本 store 丢弃游标重拉，契约 webui-ws §5 第 9 条）。
 * - `accounts`：逗号分隔账户过滤；缺省 / 空串 = 全部在线账户。
 *
 * 列表**不接受交易日参数**：当前交易日由 dzweb 解析（契约 rest §2.9.1）。
 */
export type TdOrdersQuery = TdListQuery

/** 把查询参数拼成 query string（省略 undefined 与空串，后端按「未给」处理） */
function toQueryString(query: TdListQuery): string {
  const search = new URLSearchParams()
  if (query.limit !== undefined) search.set('limit', String(query.limit))
  if (query.cursor) search.set('cursor', query.cursor)
  // 逗号分隔 + 去空白/空项（契约 rest §2.9.2：每项去首尾空白、空项跳过、重复忽略）
  const accounts = (query.accounts ?? [])
    .map(account => account.trim())
    .filter(account => account !== '')
  if (accounts.length > 0) search.set('accounts', accounts.join(','))
  const text = search.toString()
  return text ? `?${text}` : ''
}

/**
 * 交易数据读端点（契约 rest §2.9）。
 *
 * 降级语义：交易库不可读时回 **200 + available:false**（不是 5xx），调用方必须判断
 * `available` 而不是只看 HTTP 是否成功（契约 rest §2.9.8）。
 */
export const tdApi = {
  getOrders: (query: TdOrdersQuery = {}) =>
    api.get<TdOrdersResponse>(`/api/td/orders${toQueryString(query)}`),
  /**
   * 当日成交列表（契约 rest §2.9）：与委托**同信封同分页语义**，仅路径不同
   * （query string 拼接共用 `toQueryString`，不另写一份）。
   */
  getTrades: (query: TdListQuery = {}) =>
    api.get<TdTradesResponse>(`/api/td/trades${toQueryString(query)}`),
  /**
   * 统计计数器（契约 rest §2.9.5）：**无参数**——stats 是全局口径（全部在线账户 + 当前交易日），
   * 不接受 accounts 过滤，也不是列表（无 limit/cursor）。降级同样是 200 + available:false
   * （reason 只有 not_ready），调用方必须判断 `available`。
   */
  getStats: () => api.get<TdStatsResponse>('/api/td/stats'),
}
