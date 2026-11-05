import { api } from './client'
import type { TdListQuery } from '@/composables/tdListStore'
import type {
  TdAccountsResponse,
  TdAddAccountBody,
  TdOrdersResponse,
  TdPositionsResponse,
  TdStatsResponse,
  TdTradesResponse,
  TdTradingAccountsResponse,
  TdUpdateAccountBody,
  TdWriteOkResponse,
} from '@/types/api'

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

/**
 * 资金端点查询参数（契约 rest §2.9.2）：端点**不分页**（`next_cursor` 恒 null，
 * `limit`/`cursor` 被忽略），因此只承载账户过滤；缺省 / 空串 = 全部已登记账户。
 */
export interface TdTradingAccountsQuery {
  accounts?: string[]
}

/**
 * 账户过滤参数编码（契约 rest §2.9.2：逗号分隔、每项去首尾空白、空项跳过、重复忽略）。
 * 委托/成交/持仓与资金端点共用同一份编码，避免两端各自 trim/join 出细微差异。
 */
function appendAccountsParam(search: URLSearchParams, accounts: string[] | undefined): void {
  const normalized = (accounts ?? [])
    .map(account => account.trim())
    .filter(account => account !== '')
  if (normalized.length > 0) search.set('accounts', normalized.join(','))
}

/** 把查询参数拼成 query string（省略 undefined 与空串，后端按「未给」处理） */
function toQueryString(query: TdListQuery): string {
  const search = new URLSearchParams()
  if (query.limit !== undefined) search.set('limit', String(query.limit))
  if (query.cursor) search.set('cursor', query.cursor)
  appendAccountsParam(search, query.accounts)
  const text = search.toString()
  return text ? `?${text}` : ''
}

/** 资金端点 query string：只编码 accounts（端点不分页，limit/cursor 被契约忽略） */
function toTradingAccountsQueryString(query: TdTradingAccountsQuery): string {
  const search = new URLSearchParams()
  appendAccountsParam(search, query.accounts)
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
   * 持仓列表（契约 rest §2.9.1/§2.9.2/§2.9.4）：信封与委托/成交**同形**，分页参数编码也与
   * 它们同源（共用 `toQueryString`）。差异在口径（不是签名）：
   * - 覆盖**全部已登记账户（含离线）**，`accounts` 过滤作用于该全集（缺省 = 全部）；
   * - 行 `date` 是各账户**锚点日**，不是信封 `trading_day`；
   * - 行没有 `sort_key`，列表顺序 = 服务端 `(account_id, instrument_id, direction)` 升序。
   */
  getPositions: (query: TdListQuery = {}) =>
    api.get<TdPositionsResponse>(`/api/td/positions${toQueryString(query)}`),
  /**
   * 资金列表（契约 rest §2.9.1/§2.9.2/§2.9.4）：**不分页**——`next_cursor` 恒 null，
   * `limit`/`cursor` 被忽略，因此只接受 `accounts` 过滤（缺省 = 全部已登记账户）。
   * 每个已登记账户一行（含离线与无资金记录账户）：无记录行 `has_data:false` 且
   * `date`/`seq`/资金字段全 null；行 `date` 是各账户**锚点日**，不是信封 `trading_day`。
   */
  getTradingAccounts: (query: TdTradingAccountsQuery = {}) =>
    api.get<TdTradingAccountsResponse>(
      `/api/td/trading_accounts${toTradingAccountsQueryString(query)}`),
  /**
   * 统计计数器（契约 rest §2.9.5）：**无参数**——stats 是全局口径（全部在线账户 + 当前交易日），
   * 不接受 accounts 过滤，也不是列表（无 limit/cursor）。降级同样是 200 + available:false
   * （reason 只有 not_ready），调用方必须判断 `available`。
   */
  getStats: () => api.get<TdStatsResponse>('/api/td/stats'),

  // ===== 账户配置（契约 rest §2.9.7/§2.9.9）=====

  /**
   * 账户注册表（契约 rest §2.9.9）：账户配置页的**唯一账户列表来源**（跨网关全集）。
   * 任意已认证用户可读（不是 admin 专属，与其它读端点权限一致）。
   *
   * 降级语义：注册表尚不可用（dzweb 启动后未收到任何 `TD_RTN_CONFIG` 且平台库也无条目）时回
   * **200 + available:false**（reason 只有 `no_gateway_snapshot`），此时"无账户"不可信——
   * 调用方必须判断 `available`，不得把空 accounts 当作"没有账户"（契约 rest §2.9.9）。
   */
  listAccounts: () => api.get<TdAccountsResponse>('/api/td/accounts'),

  /**
   * 添加账户（契约 rest §2.9.7，admin）：`{"gateway": <网关进程名>, "account": {...}}`。
   * 200 `{ok:true}` 只表示已写入事件通道，**不代表业务已生效**——生效信号是 WS `td_rtn_config`
   *（成功推新值、失败推回滚旧值），前端 pending 以该回报清除。
   */
  addAccount: (body: TdAddAccountBody) =>
    api.post<TdWriteOkResponse>('/api/td/accounts', body),

  /**
   * 修改账户（契约 rest §2.9.7，admin）：body 为 `UpdateAccount` 的 params 子集（字段级 merge，
   * 缺失字段保留旧值，允许空对象）。**不得包含 account_id**（路径是唯一来源，出现即 400）。
   * 网关由 dzweb 按注册表从 account_id 解析，前端不传、也不写进 body。
   */
  updateAccount: (accountId: string, patch: TdUpdateAccountBody) =>
    api.put<TdWriteOkResponse>(`/api/td/accounts/${encodeURIComponent(accountId)}`, patch),

  /**
   * 删除账户（契约 rest §2.9.7，admin）：无 body（dzweb 忽略请求体）。
   * **不删除交易数据**（ADR 0045），离线账户的持仓/资金仍按锚点日可查。
   */
  removeAccount: (accountId: string) =>
    api.del<TdWriteOkResponse>(`/api/td/accounts/${encodeURIComponent(accountId)}`),
}
