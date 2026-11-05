import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProgressStore } from '@/stores/progress'
import { useTdPositionsStore } from '@/stores/tdPositions'
import { marketSourcesApi } from '@/api/marketSources'
import { tdApi } from '@/api/td'
import DashboardView from '../DashboardView.vue'
import type { TdPositionRow } from '@/types/api'

// Dashboard 引用真实 StatusIndicator 与真实持仓 store；断言页面展示的内容，
// 而不是测试自己刚写进 store 的值。
//
// #249：持仓面板已接**真实数据**（REST 基线 + WS 绝对态增量），不再是演示数据；
// 账户 / 委托 / 策略 / 消息四个面板仍是演示数据，必须继续永久可见地标注。
vi.mock('@/api/marketSources', () => ({
  marketSourcesApi: { list: vi.fn() },
}))

vi.mock('@/api/td', () => ({
  tdApi: { getPositions: vi.fn() },
}))

const ctpSource = {
  id: 1,
  source_type: 'CTP',
  source_name: 'dzmd_ctp',
  display_name: 'CTP主行情',
  ui_card: 'ctp',
  is_added: true,
  auto_login: false,
  created_at: '2026-08-01T00:00:00Z',
  updated_at: '2026-08-01T00:00:00Z',
}

function position(overrides: Partial<TdPositionRow> = {}): TdPositionRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    volume: 2,
    frozen_volume: 0,
    today_volume: 1,
    yd_volume: 1,
    price: 3512.35,
    price_decimals: 1,
    seq: 10,
    ...overrides,
  }
}

function positionListResponse(items: TdPositionRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
  ...overrides,
  }
}

/** 挂载仪表盘并等待持仓基线到达 */
async function mountDashboard() {
  const wrapper = mount(DashboardView, { attachTo: document.body })
  await flushPromises()
  return wrapper
}

describe('DashboardView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    vi.mocked(marketSourcesApi.list).mockResolvedValue([])
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
  })

  afterEach(() => {
    // 持仓 store 的降级退避定时器：用例结束时统一释放
    useTdPositionsStore().dispose()
  })

  it('无行情源时展示在线/总数 0/0', async () => {
    const w = await mountDashboard()
    expect(w.text()).toContain('0/0')
    w.unmount()
  })

  it('行情源在线时在线数/总数随聚合视图更新', async () => {
    const store = useMarketSourcesStore()
    const progress = useProgressStore()
    vi.mocked(marketSourcesApi.list).mockResolvedValue([ctpSource])
    await store.loadSources()
    // 契约 progress: min=0,max=1,current=1 → 登录完成
    progress.progress.dzmd_ctp = { min: 0, max: 1, current: 1 }

    const w = await mountDashboard()
    await w.vm.$nextTick()

    expect(w.text()).toContain('1/1')
    expect(w.text()).toContain('CTP主行情')
    expect(w.text()).toContain('已登录')

    w.unmount()
  })

  // ---- #249：持仓面板接真实数据 ----

  it('持仓面板默认只显示账户净仓，详情列（10 列）默认隐藏', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', direction: 'Long', volume: 5, seq: 10 }),
      position({ instrument_id: 'rb2601', direction: 'Short', volume: 2, seq: 11 }),
    ]))
    const w = await mountDashboard()

    // 表格里能看到真实合约与净仓（+3 = 多 5 − 空 2）
    expect(w.text()).toContain('rb2601')
    expect(w.text()).toContain('+3')
    // 详情列默认隐藏：多总/空总等表头不在渲染结果里
    expect(w.text()).not.toContain('多总')
    expect(w.text()).not.toContain('空冻结')

    w.unmount()
  })

  it('勾选「详情」后展开 10 个明细列，数值按 price_decimals 格式化', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({
        instrument_id: 'rb2601', direction: 'Long', volume: 5, today_volume: 2, yd_volume: 3,
        frozen_volume: 1, price: 3512.35, price_decimals: 1, seq: 10,
      }),
      position({
        instrument_id: 'rb2601', direction: 'Short', volume: 2, today_volume: 1, yd_volume: 1,
        frozen_volume: 0, price: 3500, price_decimals: 0, seq: 11,
      }),
    ]))
    const w = await mountDashboard()

    await w.get('[data-test="position-detail-toggle"]').setValue(true)

    for (const header of ['多总', '多今', '多昨', '多冻结', '多均价',
      '空总', '空今', '空昨', '空冻结', '空均价']) {
      expect(w.text()).toContain(header)
    }
    // 均价按行内 price_decimals：3512.35 → 3512.3（1 位）
    expect(w.text()).toContain('3512.3')

    w.unmount()
  })

  it('策略列保留列位置但值显示「—」（策略维度持仓归属尚未建立）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', volume: 2 }),
    ]))
    const w = await mountDashboard()

    // 策略净仓 / 持仓差值 / 逐策略列：表头仍在（列位置保留）
    expect(w.text()).toContain('策略净仓')
    expect(w.text()).toContain('持仓差值')
    expect(w.text()).toContain('长线')
    // 值显示占位而不是账户净仓的拷贝
    const cells = w.findAll('td').map(td => td.text())
    const placeholders = cells.filter(text => text === '—')
    // 持仓差值 + 策略净仓 + 5 个策略列 = 7 个占位单元格
    expect(placeholders.length).toBe(7)

    w.unmount()
  })

  it('持仓面板不再标注「演示」，其余四个面板仍标注（演示标签 4 个）', async () => {
    const w = await mountDashboard()
    const tags = w.findAll('[data-test="dashboard-demo-tag"]')
    expect(tags).toHaveLength(4)
    expect(tags.every(tag => tag.text() === '演示')).toBe(true)
    w.unmount()
  })

  it('横幅点名仍为演示数据的面板，并说明持仓已接真实链路', async () => {
    const w = await mountDashboard()
    const banner = w.get('[data-test="dashboard-demo-banner"]')
    const text = banner.text()
    expect(text).toContain('演示数据')
    for (const panel of ['账户', '委托', '策略', '消息']) {
      expect(text).toContain(panel)
    }
    // 持仓已接入真实链路：不得再被列为演示面板
    expect(text).toContain('「行情」与「持仓」面板为实时数据')
    w.unmount()
  })

  it('available:false 时展示统一降级提示，不显示 0 冒充数据', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue({ available: false, reason: 'not_found' })
    const w = await mountDashboard()

    const banner = w.find('[data-test="position-degraded-banner"]')
    expect(banner.exists()).toBe(true)
    expect(banner.text()).toContain('交易数据暂不可用')
    // 不出现「暂无持仓」这种假空态
    expect(w.get('[data-test="position-empty"]').text()).toContain('交易数据暂不可用')

    w.unmount()
  })

  it('成功读到空基线时显示「暂无持仓」空态（与降级区分）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    const w = await mountDashboard()

    expect(w.find('[data-test="position-degraded-banner"]').exists()).toBe(false)
    expect(w.get('[data-test="position-empty"]').text()).toBe('暂无持仓')
    w.unmount()
  })

  it('行内显示数据日期（锚点日），面板级标签同步展示', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', date: '20260807' }),
    ]))
    const w = await mountDashboard()

    expect(w.get('[data-test="position-row-date"]').text()).toBe('2026-08-07')
    expect(w.get('[data-test="position-date-label"]').text()).toBe('数据日期 2026-08-07')
    w.unmount()
  })

  it('「隐藏匹配行」勾选框现在有真实语义：隐藏净仓为零的行，聚合数不变公式', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', direction: 'Long', volume: 3, seq: 10 }),
      // 多空锁仓 → 净仓 0，属"匹配行"
      position({ instrument_id: 'ag2601', direction: 'Long', volume: 2, seq: 11 }),
      position({ instrument_id: 'ag2601', direction: 'Short', volume: 2, seq: 12 }),
    ]))
    const w = await mountDashboard()

    // 净仓非零 1 行 / 总 2 行
    expect(w.get('[data-test="position-aggregate"]').text()).toBe('1/2')
    expect(w.text()).toContain('ag2601')

    await w.get('[data-test="position-hide-matched"]').setValue(true)

    // 净仓为零的行被隐藏；聚合数（匹配/总）不因隐藏而变
    expect(w.text()).not.toContain('ag2601')
    expect(w.text()).toContain('rb2601')
    expect(w.get('[data-test="position-aggregate"]').text()).toBe('1/2')

    w.unmount()
  })

  it('账户筛选选项来自账户全集（含离线账户），不是 online_accounts', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse(
      [
        position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
        position({ account_id: 'acc_offline', instrument_id: 'ag2601' }),
      ],
      { online_accounts: ['acc_a'] }))
    const w = await mountDashboard()

    await w.get('[data-test="position-account-filter-trigger"]').trigger('click')

    const filter = w.get('[data-test="position-account-filter"]')
    expect(filter.text()).toContain('acc_a')
    // 离线账户也必须在筛选项里（契约 webui-ws §5 第 11 条）
    expect(filter.text()).toContain('acc_offline')

    w.unmount()
  })

  it('不出现真实券商名与真实合约代码', async () => {
    const w = await mountDashboard()
    const text = w.text()
    const realWorldValues = [
      '华鑫期货', '山金期货',
      'rb2510', 'hc2510', 'i2509', 'j2509', 'JM2510', 'rb2511', 'hc2511', 'i2510',
      'i2609', 'hc2501', 'j2601',
    ]
    for (const real of realWorldValues) {
      expect(text).not.toContain(real)
    }
    w.unmount()
  })

  it('聚合数与展示行自洽（账户/委托/策略仍是演示行）', async () => {
    const w = await mountDashboard()
    // 账户：3 个账户中仅 1 个已登录（旧值 2/3 与展示行矛盾）
    expect(w.get('[data-test="account-aggregate"]').text()).toBe('1/3')
    // 持仓：空基线 → 0/0（不再是写死的演示行数）
    expect(w.get('[data-test="position-aggregate"]').text()).toBe('0/0')
    // 委托：7 笔中 2 笔未成交（旧值 2/12 与 7 行矛盾）
    expect(w.get('[data-test="orders-aggregate"]').text()).toBe('2/7 笔')
    // 策略：5 个策略中 4 个运行中（旧值 5/5 与「1 已停止」矛盾）
    expect(w.get('[data-test="strategy-aggregate"]').text()).toBe('4/5')
    expect(w.get('[data-test="strategy-footer"]').text()).toBe('总计: 5 已加载, 1 已停止')
    w.unmount()
  })
})
