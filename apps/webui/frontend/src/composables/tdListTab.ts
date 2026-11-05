/**
 * 交易列表 tab 的**共用交互逻辑**（委托 tab 与成交 tab 是同一交互的两份渲染配置）。
 *
 * 与 `createTdListStore` 同一条理由（#248 设计 §4.1 / 冻结取舍 5）：账户筛选、视口位置判定、
 * 底部预取、气泡回顶这些行为在委托与成交之间**逐字相同**，差别只有「哪一份 store」。复制
 * 两份实现会让一侧的修复悄悄漏掉另一侧。
 *
 * 本模块只依赖 store 的**公开面**（不 import 具体 store），因此两份配置共用它时不会互相耦合。
 */
import type { TdListStoreView } from './tdListStore'

/**
 * tab 渲染所需的 store 公开面：直接由工厂的公开 API 派生（Pinia setup store 会把 ref 解包
 * 成普通值，{@link TdListStoreView} 就表达这个形变）。不手写第二份字段清单——否则工厂
 * 加/改名时组件会静默漂移。
 *
 * 只取**与行类型无关**的字段（`rows` 由组件直接读 store，不在共用逻辑里）；`resetAndReload`
 * 收窄成无参形态：`seed` 是 store 内部的重连接口（`td_recent` 补发），tab 永远不传。
 */
export type TdListTabStore = Pick<
  TdListStoreView,
  | 'tradingDay'
  | 'onlineAccounts'
  | 'accountFilter'
  | 'loading'
  | 'allAccountsSelected'
  | 'effectiveAccounts'
  | 'setAccountFilter'
  | 'selectAllAccounts'
  | 'prefetchIfNeeded'
  | 'prefetchNearTop'
  | 'setAtTop'
  | 'clearNewCount'
> & {
  resetAndReload: () => Promise<void>
}

/**
 * 视口滚动（RecycleScroller 的 update 事件）：更新「是否在顶部」并触发预取。
 *
 * 顶部判定用**首个可见行索引**（visibleStartIndex === 0 才是真的在顶部），
 * 不能用 endIndex——首屏只有 10 行时 endIndex 也是 9，会把"用户在顶部"误判成"不在顶部"。
 */
export function handleListUpdate(
  store: TdListTabStore,
  _startIndex: number,
  _endIndex: number,
  visibleStartIndex: number,
  visibleEndIndex: number,
): void {
  store.setAtTop(visibleStartIndex <= 0)
  store.prefetchIfNeeded(visibleEndIndex)
  store.prefetchNearTop(visibleStartIndex)
}

/** 点击气泡：回到顶部并清零计数（新行已在窗口内，滚动即可见） */
export function jumpListToTop(store: TdListTabStore, scrollToTop: () => void): void {
  store.setAtTop(true)
  store.clearNewCount()
  scrollToTop()
}

/**
 * 账户筛选按钮文案：空筛选 = 契约 rest §2.9.2 缺省语义「全部在线账户」，
 * 此时显示「全部」而不是 0/N。
 */
export function tdListFilterLabel(store: TdListTabStore): string {
  const total = store.onlineAccounts.length
  if (total === 0) return '账户 0'
  if (store.allAccountsSelected || store.effectiveAccounts.length === 0) return `全部账户（${total}）`
  return `账户 ${store.effectiveAccounts.length}/${total}`
}

/** 账户选项：在线集合排序（已下线账户不列，契约 webui-ws §5 第 11 条） */
export function tdListAccountOptions(store: TdListTabStore): string[] {
  return [...store.onlineAccounts].sort()
}

/** 账户是否被选中（未改过筛选时视为全选） */
export function isTdListAccountSelected(store: TdListTabStore, account: string): boolean {
  if (store.allAccountsSelected) return true
  return store.accountFilter.has(account)
}

/**
 * 唯一生效账户时禁用取消——取消它会让筛选变成 0 个账户，而空筛选在契约里 = 全部在线账户
 * （契约 rest §2.9.2），用户会因此看到比原来更多的数据。判定用「当前生效筛选只剩这一个」，
 * 覆盖两种来源：只剩一个在线账户（默认全选），或用户已筛到只剩它。
 */
export function isTdListLastSelectedAccount(store: TdListTabStore, account: string): boolean {
  return store.effectiveAccounts.length === 1 && store.effectiveAccounts[0] === account
}

/**
 * 切换单个账户（重置游标重拉由 store 负责）。
 *
 * 不允许取消最后一个账户：空列表在契约里的语义是「缺省 = 全部在线账户」（rest §2.9.2），
 * 用户取消到 0 个却看到全部数据会非常反直觉。要回到全部请用「全选」。
 */
export function toggleTdListAccount(store: TdListTabStore, account: string): void {
  const next = store.allAccountsSelected
    ? store.onlineAccounts.filter(item => item !== account)
    : store.onlineAccounts.filter(item => (
        item === account ? !store.accountFilter.has(item) : store.accountFilter.has(item)
      ))
  if (next.length === 0) return
  void store.setAccountFilter(next)
}

/** 手动刷新：丢弃游标从首页重拉（与日切/data_changed 同一路径） */
export function refreshTdList(store: TdListTabStore): void {
  void store.resetAndReload()
}

/** 表格列描述（表头与行共用同一组类；列宽类必须一并给上，否则该列不参与 flex 定宽） */
export interface TdListColumn {
  key: string
  label: string
  className: string
}
