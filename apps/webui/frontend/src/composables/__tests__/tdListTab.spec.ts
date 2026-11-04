import { describe, it, expect, vi } from 'vitest'
import {
  handleListUpdate,
  isTdListAccountSelected,
  isTdListLastSelectedAccount,
  jumpListToTop,
  refreshTdList,
  tdListAccountOptions,
  tdListFilterLabel,
  toggleTdListAccount,
} from '../tdListTab'
import type { TdListTabStore } from '../tdListTab'

/**
 * 构造 tab store 的最小替身：只实现 `TdListTabStore` 的公开面（委托与成交共用同一套交互
 * 逻辑，本套直接测纯函数，不经 Vue 组件与 Pinia）。
 *
 * `rows` 不在此类型内（共用逻辑不读行内容），因此这里也不需要构造真实行。
 */
function tabStore(overrides: Partial<TdListTabStore> = {}): TdListTabStore & {
  setAtTop: ReturnType<typeof vi.fn>
  prefetchIfNeeded: ReturnType<typeof vi.fn>
  prefetchNearTop: ReturnType<typeof vi.fn>
  clearNewCount: ReturnType<typeof vi.fn>
  setAccountFilter: ReturnType<typeof vi.fn>
  selectAllAccounts: ReturnType<typeof vi.fn>
  resetAndReload: ReturnType<typeof vi.fn>
} {
  const store = {
    tradingDay: '20260808',
    onlineAccounts: ['acc_a', 'acc_b'],
    accountFilter: new Set(['acc_a', 'acc_b']),
    loading: false,
    allAccountsSelected: true,
    effectiveAccounts: ['acc_a', 'acc_b'],
    setAccountFilter: vi.fn(async () => {}),
    selectAllAccounts: vi.fn(async () => {}),
    resetAndReload: vi.fn(async () => {}),
    setAtTop: vi.fn(),
    prefetchIfNeeded: vi.fn(),
    prefetchNearTop: vi.fn(),
    clearNewCount: vi.fn(),
    ...overrides,
  }
  return store as unknown as ReturnType<typeof tabStore>
}

describe('tdListTab · 账户筛选文案（契约 rest §2.9.2：空筛选 = 全部在线账户）', () => {
  it('无在线账户 → 账户 0（不说「全部」以免暗示有数据）', () => {
    expect(tdListFilterLabel(tabStore({
      onlineAccounts: [], effectiveAccounts: [], accountFilter: new Set(), allAccountsSelected: true,
    }))).toBe('账户 0')
  })

  it('全选态 → 全部账户（N）', () => {
    expect(tdListFilterLabel(tabStore())).toBe('全部账户（2）')
  })

  it('部分筛选 → 账户 M/N', () => {
    expect(tdListFilterLabel(tabStore({
      allAccountsSelected: false,
      accountFilter: new Set(['acc_a']),
      effectiveAccounts: ['acc_a'],
    }))).toBe('账户 1/2')
  })

  it('空筛选集合（等价缺省）按全选文案显示，不显示 0/N 的分裂状态', () => {
    expect(tdListFilterLabel(tabStore({
      allAccountsSelected: true,
      accountFilter: new Set(),
      effectiveAccounts: [],
    }))).toBe('全部账户（2）')
  })
})

describe('tdListTab · 账户选项与选中态（契约 webui-ws §5 第 11 条）', () => {
  it('选项 = 在线集合排序（未排序的信封顺序不外泄到 UI）', () => {
    expect(tdListAccountOptions(tabStore({ onlineAccounts: ['acc_c', 'acc_a', 'acc_b'] })))
      .toEqual(['acc_a', 'acc_b', 'acc_c'])
  })

  it('已下线账户不列：选项只取在线集合，不看 accountFilter', () => {
    // accountFilter 里残留 acc_gone，但在线集合已无它 → 不出现在选项里
    expect(tdListAccountOptions(tabStore({
      onlineAccounts: ['acc_a'],
      accountFilter: new Set(['acc_a', 'acc_gone']),
      allAccountsSelected: false,
      effectiveAccounts: ['acc_a'],
    }))).toEqual(['acc_a'])
  })

  it('全选态下任何在线账户都显示为选中', () => {
    const store = tabStore()
    expect(isTdListAccountSelected(store, 'acc_a')).toBe(true)
    expect(isTdListAccountSelected(store, 'acc_b')).toBe(true)
  })

  it('筛选态下只认 accountFilter 里的账户', () => {
    const store = tabStore({
      allAccountsSelected: false,
      accountFilter: new Set(['acc_a']),
      effectiveAccounts: ['acc_a'],
    })
    expect(isTdListAccountSelected(store, 'acc_a')).toBe(true)
    expect(isTdListAccountSelected(store, 'acc_b')).toBe(false)
  })
})

describe('tdListTab · 唯一生效账户禁用取消（契约 rest §2.9.2：空筛选 = 全部）', () => {
  it('唯一生效账户 → 禁用取消（true）', () => {
    const store = tabStore({
      onlineAccounts: ['acc_a'],
      accountFilter: new Set(['acc_a']),
      allAccountsSelected: true,
      effectiveAccounts: ['acc_a'],
    })
    expect(isTdListLastSelectedAccount(store, 'acc_a')).toBe(true)
  })

  it('多账户全选态 → 不禁用（false）', () => {
    expect(isTdListLastSelectedAccount(tabStore(), 'acc_a')).toBe(false)
  })

  it('多账户部分筛选（生效 2 个）→ 不禁用', () => {
    const store = tabStore({
      allAccountsSelected: false,
      accountFilter: new Set(['acc_a', 'acc_b']),
      effectiveAccounts: ['acc_a', 'acc_b'],
    })
    expect(isTdListLastSelectedAccount(store, 'acc_a')).toBe(false)
  })

  it('多在线账户但已筛到只剩一个 → 该账户禁用取消，其他不禁用', () => {
    const store = tabStore({
      allAccountsSelected: false,
      accountFilter: new Set(['acc_a']),
      effectiveAccounts: ['acc_a'],
    })
    expect(isTdListLastSelectedAccount(store, 'acc_a')).toBe(true)
    expect(isTdListLastSelectedAccount(store, 'acc_b')).toBe(false)
  })
})

describe('tdListTab · 切换账户（不允许取消最后一个）', () => {
  it('全选态取消一个 → setAccountFilter(其余账户)，重置游标由 store 负责', () => {
    const store = tabStore()
    toggleTdListAccount(store, 'acc_b')
    expect(store.setAccountFilter).toHaveBeenCalledWith(['acc_a'])
  })

  it('筛选态再取消一个 → 提交剩余已选账户', () => {
    const store = tabStore({
      allAccountsSelected: false,
      accountFilter: new Set(['acc_a', 'acc_b']),
      effectiveAccounts: ['acc_a', 'acc_b'],
    })
    toggleTdListAccount(store, 'acc_a')
    expect(store.setAccountFilter).toHaveBeenCalledWith(['acc_b'])
  })

  it('取消最后一个账户 → 不发请求（空筛选在契约里 = 全部，会看到更多数据）', () => {
    const store = tabStore({
      onlineAccounts: ['acc_a'],
      accountFilter: new Set(['acc_a']),
      allAccountsSelected: false,
      effectiveAccounts: ['acc_a'],
    })
    toggleTdListAccount(store, 'acc_a')
    expect(store.setAccountFilter).not.toHaveBeenCalled()
  })
})

describe('tdListTab · 视口判定与预取（顶部判定用可见起始索引）', () => {
  it('visibleStartIndex === 0 才算在顶部（首屏 10 行时 endIndex 也是 9，不能据此误判）', () => {
    const store = tabStore()
    handleListUpdate(store, 0, 9, 0, 9)
    expect(store.setAtTop).toHaveBeenCalledWith(true)
  })

  it('visibleStartIndex > 0 → 不在顶部（即使用户只滚了一行）', () => {
    const store = tabStore()
    handleListUpdate(store, 1, 10, 1, 10)
    expect(store.setAtTop).toHaveBeenCalledWith(false)
  })

  it('同一次 update 同时触发底部预取（visibleEndIndex）与窗口淘汰后的回顶重拉', () => {
    const store = tabStore()
    handleListUpdate(store, 0, 1999, 5, 1999)
    expect(store.prefetchIfNeeded).toHaveBeenCalledWith(1999)
    expect(store.prefetchNearTop).toHaveBeenCalledWith(5)
  })
})

describe('tdListTab · 气泡回顶与刷新', () => {
  it('点击气泡：先置顶 + 清零计数，再滚动到首项（顺序固定）', () => {
    const store = tabStore()
    const scrollToTop = vi.fn()
    jumpListToTop(store, scrollToTop)

    expect(store.setAtTop).toHaveBeenCalledWith(true)
    expect(store.clearNewCount).toHaveBeenCalledTimes(1)
    expect(scrollToTop).toHaveBeenCalledTimes(1)
  })

  it('手动刷新 = 丢弃游标从首页重拉（与日切/data_changed 同一路径）', () => {
    const store = tabStore()
    refreshTdList(store)
    expect(store.resetAndReload).toHaveBeenCalledTimes(1)
  })
})
