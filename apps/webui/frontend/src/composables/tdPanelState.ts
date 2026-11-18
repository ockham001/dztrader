/**
 * 仪表盘交易面板（持仓 / 资金）的空态状态机（#358）。
 *
 * 为什么单独成模块：两个面板的空态判定必须**同源**——「无法确认数据」和「确实没有数据」
 * 在下游不可区分时会渲染成同一句「暂无」，而两者对盯盘读者的含义完全相反
 * （一个要排查网关，一个是正常的空仓）。把判定抽成纯函数后，两个面板只负责渲染，
 * 判定逻辑只有一份、可被 spec 逐态固定（契约 webui-ws §5 第 13 条）。
 */

/** 面板种类：仅影响空态/提示条文案，不影响状态判定 */
export type TdPanelKind = 'position' | 'fund'

/**
 * 面板空态状态（互斥）。
 *
 * - `degraded`：`!available`，交易库不可读（空态槽位给统一降级文案，降级横幅另渲染）；
 * - `loading`：请求在途或尚未初始化（不得显示「暂无…」，也不得留空白 tbody）；
 * - `error`：请求失败（不得显示「暂无…」）；
 * - `no-accounts`：成功读到基线，但**账户全集为空**——无法判断持仓/资金，属未知态；
 * - `empty`：已知账户集合下的正常空数据（确实没有持仓行 / 资金行）。
 */
export type TdPanelState = 'degraded' | 'loading' | 'error' | 'no-accounts' | 'empty'

/** {@link tdPanelState} 的输入：全部来自持仓/资金 store 的公开字段 */
export interface TdPanelStateInput {
  /** store.available：交易库是否可读 */
  available: boolean
  /** store.loading：请求是否在途 */
  loading: boolean
  /** store.initialized：是否成功完成过一次基线 */
  initialized: boolean
  /** store.error：最近一次请求失败信息 */
  error: string | null
  /** 账户全集是否非空（持仓 = accounts；资金 = rows；两者都含 envelope 的 online_accounts） */
  hasAccounts: boolean
}

/**
 * 判定面板空态状态（互斥，优先级固定）。
 *
 * 优先级：**degraded（!available）→ loading（loading || !initialized）→ error →
 * no-accounts（无账户全集）→ empty**。
 *
 * 排序理由：
 * - 「交易库不可读」比「加载中 / 失败 / 无账户」更根本——它是降级而非普通失败；
 * - 请求在途 / 未初始化时还没有任何可判断的事实，必须先说「加载中」；
 * - 请求失败后账户全集是**不可信**的（可能只是没拉到），必须说「加载失败」而不是「未知」；
 * - 「无账户全集」= 没有可判断的对象，是未知态；只有账户集合已知且确实无行才是正常空态。
 *
 * 本函数只判定「没有行可渲染时该说什么」；**有行时视图不渲染空态槽位**（旧快照照常展示，
 * 降级提示条独立渲染），因此调用方需自行保证只在无行时消费返回值。
 */
export function tdPanelState(input: TdPanelStateInput): TdPanelState {
  if (!input.available) return 'degraded'
  if (input.loading || !input.initialized) return 'loading'
  if (input.error !== null) return 'error'
  if (!input.hasAccounts) return 'no-accounts'
  return 'empty'
}

/**
 * 空态槽位文案。
 *
 * `no-accounts` 必须说「未知」——账户全集为空意味着**无法确认**有没有持仓/资金，
 * 与成功读到基线的「暂无」是两条相反的事实（#358 AC2/AC5）。
 */
export function tdPanelEmptyText(state: TdPanelState, kind: TdPanelKind): string {
  switch (state) {
    case 'degraded':
      return '交易数据暂不可用'
    case 'loading':
      return '加载中...'
    case 'error':
      return '加载失败'
    case 'no-accounts':
      return kind === 'position' ? '持仓未知' : '资金未知'
    case 'empty':
      return kind === 'position' ? '暂无持仓' : '暂无资金数据'
  }
}

/**
 * 面板级未知态提示条文案（#358 AC2）：同时给出**原因**与**排查方向**。
 *
 * 单独一条提示条而不是只改空态文案：空态槽位在表格内、会被横向滚动带出视野；面板级
 * 提示条常驻在表头下方，读者扫一眼就知道「不是空仓，是没连上」。用中性色渲染（不用
 * status 色族）：尚未配置账户也可能是初始态，染成告警色会让常绿告警失去意义
 * （#358 拍板记录）。
 */
export function tdPanelUnknownBannerText(kind: TdPanelKind): string {
  const subject = kind === 'position' ? '持仓' : '资金'
  return `${subject}未知：交易账户未就绪（交易网关进程未运行或无已登记账户），无法确认当前${subject}；`
    + '请检查交易网关进程与账户配置'
}
