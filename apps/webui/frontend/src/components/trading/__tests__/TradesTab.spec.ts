import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import TradesTab from '../TradesTab.vue'
import {
  TD_BATCH_INTERVAL_MS,
  __resetTdBatchSchedulerForTests,
  useTdTradesStore,
} from '@/stores/tdTrades'
import { tdApi } from '@/api/td'
import type { TdTradeRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn(), getTrades: vi.fn() },
}))

// 虚拟滚动容器：真实 RecycleScroller 依赖 DOM 尺寸/虚拟化计算，测试以「全量渲染 + 暴露
// scrollToItem」的 stub 替代（口径同 OrdersTab.spec.ts）。keyField 允许函数（成交用
// 业务键函数而非行内数值列，见设计 §4.3）。
const scrollToItem = vi.fn()
vi.mock('vue-virtual-scroller', () => ({
  RecycleScroller: {
    name: 'RecycleScroller',
    props: {
      items: { type: Array, default: () => [] },
      itemSize: { type: Number, default: 36 },
      keyField: { type: [String, Function], default: 'order_id' },
      emitUpdate: { type: Boolean, default: false },
    },
    emits: ['update'],
    setup(_props: unknown, { expose }: { expose: (api: Record<string, unknown>) => void }) {
      expose({ scrollToItem, scrollToPosition: vi.fn() })
    },
    template: `
      <div class="stub-scroller">
        <button
          type="button"
          data-test="stub-scroll-end"
          @click="$emit('update', 0, items.length - 1, items.length - 1, items.length - 1)"
        />
        <button
          type="button"
          data-test="stub-scroll-top"
          @click="$emit('update', 0, 9, 0, 9)"
        />
        <div v-for="(item, index) in items" :key="index" class="stub-row" :data-trade-id="item.trade_id">
          <slot :item="item" :index="index" />
        </div>
      </div>`,
  },
}))

function trade(overrides: Partial<TdTradeRow> = {}): TdTradeRow {
  const tradeId = overrides.trade_id ?? 't1'
  return {
    account_id: 'acc_a',
    date: '20260808',
    trade_id: tradeId,
    order_id: 10231,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price: 3512.3,
    volume: 1,
    time: 32405,
    price_decimals: 1,
    seq: 1,
    sort_key: `k-${tradeId}`,
    ...overrides,
  }
}

function listResponse(items: TdTradeRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('TradesTab 渲染与交互（#248 契约 rest §2.9 / webui-ws §5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
    scrollToItem.mockClear()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdBatchSchedulerForTests()
  })

  it('挂载时用成交端点拉基线并渲染列（时间/账户/合约/方向/开平/成交价/成交量/委托号/策略）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade()]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
    // 成交 tab 绝不拉委托端点
    expect(tdApi.getOrders).not.toHaveBeenCalled()
    expect(wrapper.findAll('.stub-row')).toHaveLength(1)

    const header = wrapper.find('[data-test="td-trades-header"]').text()
    for (const label of ['时间', '账户', '合约', '方向', '开平', '成交价', '成交量', '委托号', '策略']) {
      expect(header).toContain(label)
    }

    const text = wrapper.text()
    expect(text).toContain('09:00:05')  // 32405s
    expect(text).toContain('acc_a')
    expect(text).toContain('rb2601')
    expect(text).toContain('买')
    expect(text).toContain('开仓')
    expect(text).toContain('3512.3')    // price_decimals=1
    expect(text).toContain('10231')     // order_id
    expect(text).toContain('stg_demo')
    wrapper.unmount()
  })

  it('成交价按行内 price_decimals 做 toFixed（不自行推断小数位）', async () => {
    // 同一成交价、三种行内精度：输出必须跟着 price_decimals 走（契约 rest §2.9.6）
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([
      trade({ trade_id: 't1', price: 3512.5, price_decimals: 1, sort_key: 'k3' }),
      trade({ trade_id: 't2', price: 3512.5, price_decimals: 3, sort_key: 'k2' }),
      trade({ trade_id: 't3', price: 3512.5, price_decimals: 0, sort_key: 'k1' }),
    ]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    const cells = wrapper.findAll('.stub-row .td-trade__col').map(cell => cell.text())
    expect(cells).toContain('3512.5')
    expect(cells).toContain('3512.500')
    expect(cells).toContain('3513')
    wrapper.unmount()
  })

  it('行 key 用成交业务键（account|date|exchange|trade_id），不是 trade_id 单键', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([
      trade({ trade_id: '1', exchange_id: 'SHFE' }),
      trade({ trade_id: '1', exchange_id: 'CZCE', sort_key: 'k2' }),
    ]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    const keyField = wrapper.findComponent({ name: 'RecycleScroller' }).props('keyField') as
      (row: TdTradeRow) => string
    expect(keyField(trade({ trade_id: '9', exchange_id: 'DCE' }))).toBe('acc_a|20260808|DCE|9')
    expect(wrapper.findAll('.stub-row')).toHaveLength(2)
    wrapper.unmount()
  })

  it('首屏加载中显示加载态，不抢先显示「当日暂无成交」', async () => {
    let resolveRequest: (value: unknown) => void = () => {}
    vi.mocked(tdApi.getTrades).mockReturnValue(new Promise((resolve) => {
      resolveRequest = resolve as (value: unknown) => void
    }) as never)
    const wrapper = mount(TradesTab)
    await nextTick()

    expect(wrapper.text()).toContain('加载中')
    expect(wrapper.find('[data-test="td-trades-empty"]').exists()).toBe(false)

    resolveRequest(listResponse([]))
    await flushPromises()
    expect(wrapper.find('[data-test="td-trades-empty"]').exists()).toBe(true)
    wrapper.unmount()
  })

  it('可用但无行 → 空态两文案（区分「无在线账户」与「当日无成交」）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([], { trading_day: null, online_accounts: [] }))
    const wrapper = mount(TradesTab)
    await flushPromises()

    const empty = wrapper.find('[data-test="td-trades-empty"]')
    expect(empty.text()).toContain('无在线交易账户')
    expect(empty.text()).toContain('交易账户登录后')
    wrapper.unmount()
  })

  it('有交易日但无行 → 当日无成交空态', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    const empty = wrapper.find('[data-test="td-trades-empty"]')
    expect(empty.text()).toContain('当日暂无成交')
    expect(empty.text()).toContain('当前交易日还没有成交记录')
    wrapper.unmount()
  })

  it('降级响应展示统一降级提示条（不白屏、不显示 0 冒充数据、不显示空态）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'not_found' })
    const wrapper = mount(TradesTab)
    await flushPromises()

    const banner = wrapper.find('[data-test="td-degraded-banner"]')
    expect(banner.exists()).toBe(true)
    expect(banner.text()).toContain('交易数据')
    expect(wrapper.findAll('.stub-row')).toHaveLength(0)
    expect(wrapper.find('[data-test="td-trades-empty"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('虚拟滚动开启 update 事件（否则预取永不触发）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade()]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    expect(wrapper.findComponent({ name: 'RecycleScroller' }).props('emitUpdate')).toBe(true)
    wrapper.unmount()
  })

  it('滚动接近底部时按游标预取下一页', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't1', sort_key: 'k2' })], { next_cursor: 'c1' }))
    const wrapper = mount(TradesTab)
    await flushPromises()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)

    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't0', sort_key: 'k1', seq: 0 })]))
    await wrapper.find('[data-test="stub-scroll-end"]').trigger('click')
    await flushPromises()

    expect(tdApi.getTrades).toHaveBeenLastCalledWith({ limit: 200, cursor: 'c1', accounts: undefined })
    expect(wrapper.findAll('.stub-row')).toHaveLength(2)
    wrapper.unmount()
  })

  it('「N 条新数据」气泡：不在顶部时显示计数，点击回顶部并清零', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(listResponse([trade({ trade_id: 't1', seq: 10 })]))
    const wrapper = mount(TradesTab)
    await flushPromises()

    const store = useTdTradesStore()
    store.setAtTop(false)
    store.applyTradeEvent(trade({ trade_id: 't2', seq: 11, sort_key: 'k2' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    await nextTick()

    const bubble = wrapper.find('[data-test="td-new-rows-bubble"]')
    expect(bubble.exists()).toBe(true)
    expect(bubble.text()).toContain('1 条新数据')

    await bubble.trigger('click')
    await flushPromises()

    expect(scrollToItem).toHaveBeenCalledWith(0)
    expect(useTdTradesStore().newCount).toBe(0)
    expect(wrapper.find('[data-test="td-new-rows-bubble"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('账户筛选：默认全选，取消一个账户后按 accounts 重拉（契约 webui-ws §5 第 11 条）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade()], { online_accounts: ['acc_a', 'acc_b'] }))
    const wrapper = mount(TradesTab)
    await flushPromises()

    await wrapper.find('[aria-controls="td-trade-account-filter"]').trigger('click')
    const boxes = wrapper.findAll('[data-test="td-trade-account-filter"] input[type="checkbox"]')
    expect(boxes).toHaveLength(2)
    expect(boxes.every(box => (box.element as HTMLInputElement).checked)).toBe(true)

    vi.mocked(tdApi.getTrades).mockResolvedValueOnce(
      listResponse([trade({ trade_id: 't2' })], { online_accounts: ['acc_a', 'acc_b'] }))
    await boxes[1].trigger('change')
    await flushPromises()

    expect(tdApi.getTrades).toHaveBeenLastCalledWith({ limit: 200, accounts: ['acc_a'] })
    expect(wrapper.text()).toContain('账户 1/2')
    wrapper.unmount()
  })

  it('不允许取消最后一个账户（空筛选在契约里等价于「全部」，会造成歧义）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade()], { online_accounts: ['acc_a'] }))
    const wrapper = mount(TradesTab)
    await flushPromises()
    await wrapper.find('[aria-controls="td-trade-account-filter"]').trigger('click')

    const box = wrapper.find('[data-test="td-trade-account-filter"] input[type="checkbox"]')
    expect((box.element as HTMLInputElement).disabled).toBe(true)
    await box.trigger('change')
    await flushPromises()

    // 唯一账户被禁用 → 点击不触发重拉（没有新的请求）
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('多账户时允许取消（禁用规则只作用于「唯一生效账户」）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue(
      listResponse([trade()], { online_accounts: ['acc_a', 'acc_b'] }))
    const wrapper = mount(TradesTab)
    await flushPromises()
    await wrapper.find('[aria-controls="td-trade-account-filter"]').trigger('click')

    const boxes = wrapper.findAll('[data-test="td-trade-account-filter"] input[type="checkbox"]')
    expect(boxes.every(box => (box.element as HTMLInputElement).disabled)).toBe(false)
    wrapper.unmount()
  })

  it('卸载时清理 store 定时器（离开页面后不再请求）', async () => {
    vi.mocked(tdApi.getTrades).mockResolvedValue({ available: false, reason: 'busy' })
    const wrapper = mount(TradesTab)
    await flushPromises()
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)

    wrapper.unmount()
    await vi.advanceTimersByTimeAsync(60_000)

    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
  })
})
