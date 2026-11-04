import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import OrdersTab from '../OrdersTab.vue'
import {
  TD_BATCH_INTERVAL_MS,
  __resetTdBatchSchedulerForTests,
  useTdOrdersStore,
} from '@/stores/tdOrders'
import { tdApi } from '@/api/td'
import type { TdOrderRow } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn() },
}))

// 虚拟滚动容器：真实 RecycleScroller 依赖 DOM 尺寸/虚拟化计算，测试以「全量渲染 + 暴露
// scrollToItem」的 stub 替代（口径同 LogViewerTab.spec.ts）。items 数量仍可断言——
// 组件只把窗口行交给 RecycleScroller，DOM 行数与数据窗口一致。
const scrollToItem = vi.fn()
vi.mock('vue-virtual-scroller', () => ({
  RecycleScroller: {
    name: 'RecycleScroller',
    // Boolean 类型声明才能让 emit-update（无值属性）被 Vue 布尔转换，否则拿到空串 ''
    props: {
      items: { type: Array, default: () => [] },
      itemSize: { type: Number, default: 36 },
      keyField: { type: String, default: 'order_id' },
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
        <div v-for="(item, index) in items" :key="item.order_id" class="stub-row" :data-order-id="item.order_id">
          <slot :item="item" :index="index" />
        </div>
      </div>`,
  },
}))

function row(overrides: Partial<TdOrderRow> = {}): TdOrderRow {
  const orderId = overrides.order_id ?? 1
  return {
    account_id: 'acc_a',
    date: '20260808',
    order_id: orderId,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price_type: 'Limit',
    status: 'PartTraded',
    price: 3512.5,
    volume: 2,
    volume_traded: 1,
    remark: '',
    time: 32400,
    avg_price: 3512.3,
    price_decimals: 1,
    avg_price_decimals: 3,
    seq: orderId,
    sort_key: `k-${String(orderId).padStart(6, '0')}`,
    ...overrides,
  }
}

function listResponse(items: TdOrderRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

describe('OrdersTab 渲染与交互（#246 契约 webui-ws §5）', () => {
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

  it('挂载时拉基线并渲染行（列含时间/账户/合约/价格/状态/委托号）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([
      row({ order_id: 2, price: 3512.5, price_decimals: 1, avg_price: 3512.3, avg_price_decimals: 3 }),
      row({ order_id: 1, avg_price: null, volume_traded: 0, status: 'NotTraded' }),
    ]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
    expect(wrapper.findAll('.stub-row')).toHaveLength(2)
    const text = wrapper.text()
    expect(text).toContain('09:00:00')
    expect(text).toContain('acc_a')
    expect(text).toContain('rb2601')
    expect(text).toContain('买')
    expect(text).toContain('开仓')
    expect(text).toContain('3512.5')
    expect(text).toContain('3512.300')
    expect(text).toContain('部分成交')
    expect(text).toContain('2')
    wrapper.unmount()
  })

  it('avg_price 为 null（无成交）显示破折号，不显示 0', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([
      row({ order_id: 1, avg_price: null, volume_traded: 0, status: 'NotTraded' }),
    ]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    const cells = wrapper.findAll('.stub-row .td-order__col').map(cell => cell.text())
    expect(cells).toContain('—')
    expect(cells).not.toContain('0.000')
    wrapper.unmount()
  })

  it('首屏加载中显示加载态，不抢先显示「当日暂无委托」', async () => {
    let resolveRequest: (value: unknown) => void = () => {}
    vi.mocked(tdApi.getOrders).mockReturnValue(new Promise((resolve) => {
      resolveRequest = resolve as (value: unknown) => void
    }) as never)
    const wrapper = mount(OrdersTab)
    await nextTick()

    expect(wrapper.text()).toContain('加载中')
    expect(wrapper.find('[data-test="td-orders-empty"]').exists()).toBe(false)

    resolveRequest(listResponse([]))
    await flushPromises()
    expect(wrapper.find('[data-test="td-orders-empty"]').exists()).toBe(true)
    wrapper.unmount()
  })

  it('降级响应展示统一降级提示条（不白屏、不显示 0 冒充数据）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'not_found' })
    const wrapper = mount(OrdersTab)
    await flushPromises()

    const banner = wrapper.find('[data-test="td-degraded-banner"]')
    expect(banner.exists()).toBe(true)
    expect(banner.text()).toContain('交易数据')
    expect(wrapper.findAll('.stub-row')).toHaveLength(0)
    // 降级不是"无数据"空态：不显示承诺"无委托"的空态文案
    expect(wrapper.find('[data-test="td-orders-empty"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('可用但无行 → 空态文案（区分「无在线账户」与「当日无委托」）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([], { trading_day: null, online_accounts: [] }))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    const empty = wrapper.find('[data-test="td-orders-empty"]')
    expect(empty.exists()).toBe(true)
    expect(empty.text()).toContain('无在线交易账户')
    wrapper.unmount()
  })

  it('有交易日但无行 → 当日无委托空态', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    expect(wrapper.find('[data-test="td-orders-empty"]').text()).toContain('当日暂无委托')
    wrapper.unmount()
  })

  it('「N 条新数据」气泡：不在顶部时显示计数，点击回顶部并清零', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1, seq: 10 })]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    const store = useTdOrdersStore()
    store.setAtTop(false)
    store.applyOrderEvent(row({ order_id: 2, seq: 11, sort_key: 'k-000002' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    await nextTick()

    const bubble = wrapper.find('[data-test="td-new-rows-bubble"]')
    expect(bubble.exists()).toBe(true)
    expect(bubble.text()).toContain('1 条新数据')

    await bubble.trigger('click')
    await flushPromises()

    expect(scrollToItem).toHaveBeenCalledWith(0)
    expect(useTdOrdersStore().newCount).toBe(0)
    expect(wrapper.find('[data-test="td-new-rows-bubble"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('账户筛选：默认全选，取消一个账户后按 accounts 重拉', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a', 'acc_b'] }))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    await wrapper.find('[aria-controls="td-order-account-filter"]').trigger('click')
    const boxes = wrapper.findAll('[data-test="td-account-filter"] input[type="checkbox"]')
    expect(boxes).toHaveLength(2)
    expect(boxes.every(box => (box.element as HTMLInputElement).checked)).toBe(true)

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2 })], { online_accounts: ['acc_a', 'acc_b'] }))
    await boxes[1].trigger('change')
    await flushPromises()

    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: 200, accounts: ['acc_a'] })
    // 计数文案：从「全部」变成 1/2
    expect(wrapper.text()).toContain('账户 1/2')
    wrapper.unmount()
  })

  it('不允许取消最后一个账户（空筛选在契约里等价于「全部」，会造成歧义）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a'] }))
    const wrapper = mount(OrdersTab)
    await flushPromises()
    await wrapper.find('[aria-controls="td-order-account-filter"]').trigger('click')

    const box = wrapper.find('[data-test="td-account-filter"] input[type="checkbox"]')
    expect((box.element as HTMLInputElement).disabled).toBe(true)
    await box.trigger('change')
    await flushPromises()

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('筛选面板「全选」恢复全选（请求不再带 accounts）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(
      listResponse([row({ order_id: 1 })], { online_accounts: ['acc_a', 'acc_b'] }))
    const wrapper = mount(OrdersTab)
    await flushPromises()
    await wrapper.find('[aria-controls="td-order-account-filter"]').trigger('click')
    const boxes = wrapper.findAll('[data-test="td-account-filter"] input[type="checkbox"]')
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 2 })], { online_accounts: ['acc_a', 'acc_b'] }))
    await boxes[1].trigger('change')
    await flushPromises()
    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: 200, accounts: ['acc_a'] })

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 3 })], { online_accounts: ['acc_a', 'acc_b'] }))
    const selectAllBtn = wrapper.findAll('button').find(btn => btn.text() === '全选')
    expect(selectAllBtn).toBeTruthy()
    await selectAllBtn!.trigger('click')
    await flushPromises()

    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: 200, accounts: undefined })
    wrapper.unmount()
  })

  it('虚拟滚动开启 update 事件（RecycleScroller emitUpdate，否则预取永不触发）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1 })]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    expect(wrapper.findComponent({ name: 'RecycleScroller' }).props('emitUpdate')).toBe(true)
    wrapper.unmount()
  })

  it('滚动接近底部时按游标预取下一页', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 1, seq: 1 })], { next_cursor: 'c1' }))
    const wrapper = mount(OrdersTab)
    await flushPromises()
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)

    vi.mocked(tdApi.getOrders).mockResolvedValueOnce(
      listResponse([row({ order_id: 0, seq: 0, sort_key: 'k-000000' })]))
    await wrapper.find('[data-test="stub-scroll-end"]').trigger('click')
    await flushPromises()

    expect(tdApi.getOrders).toHaveBeenLastCalledWith({ limit: 200, cursor: 'c1', accounts: undefined })
    expect(wrapper.findAll('.stub-row')).toHaveLength(2)
    wrapper.unmount()
  })

  it('滚动回到顶部时「N 条新数据」计数清零（用户已看到新行）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue(listResponse([row({ order_id: 1, seq: 10 })]))
    const wrapper = mount(OrdersTab)
    await flushPromises()

    const store = useTdOrdersStore()
    store.setAtTop(false)
    store.applyOrderEvent(row({ order_id: 2, seq: 11, sort_key: 'k-000002' }))
    await vi.advanceTimersByTimeAsync(TD_BATCH_INTERVAL_MS)
    await nextTick()
    expect(wrapper.find('[data-test="td-new-rows-bubble"]').exists()).toBe(true)

    await wrapper.find('[data-test="stub-scroll-top"]').trigger('click')
    await nextTick()

    expect(store.newCount).toBe(0)
    expect(wrapper.find('[data-test="td-new-rows-bubble"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('卸载时清理 store 定时器（离开页面后不再请求）', async () => {
    vi.mocked(tdApi.getOrders).mockResolvedValue({ available: false, reason: 'busy' })
    const wrapper = mount(OrdersTab)
    await flushPromises()
    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)

    wrapper.unmount()
    await vi.advanceTimersByTimeAsync(60_000)

    expect(tdApi.getOrders).toHaveBeenCalledTimes(1)
  })
})
