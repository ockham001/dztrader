import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProgressStore } from '@/stores/progress'
import { useTdPositionsStore } from '@/stores/tdPositions'
import { useTdTradingAccountsStore } from '@/stores/tdTradingAccounts'
import { useTdAccountsStore } from '@/stores/tdAccounts'
import { useProcessStore } from '@/stores/process'
import { marketSourcesApi } from '@/api/marketSources'
import { tdApi } from '@/api/td'
import DashboardView from '../DashboardView.vue'
import type { TdPositionRow, TdTradingAccountRow, TdTradingAccountsResponse } from '@/types/api'

// Dashboard 引用真实 StatusIndicator 与真实持仓/资金 store；断言页面展示的内容，
// 而不是测试自己刚写进 store 的值。
//
// #249：持仓面板已接**真实数据**（REST 基线 + WS 绝对态增量）；
// #250：账户演示面板改为「资金」面板并接真实数据（REST 基线 + WS 绝对态增量）；
// 委托 / 策略两个面板仍是演示数据，必须继续永久可见地标注。
// 消息面板已随 ADR 0060 接入真实通知链路（REST 历史基线 + WS notify_ui 增量）。
vi.mock('@/api/marketSources', () => ({
  marketSourcesApi: { list: vi.fn() },
}))

vi.mock('@/api/td', () => ({
  tdApi: {
    getPositions: vi.fn(),
    getTradingAccounts: vi.fn(),
    refreshTradingAccount: vi.fn(),
    // 仪表盘挂载时重拉账户注册表（展示名来源之一，ADR 0056 决策 9）
    listAccounts: vi.fn(),
  },
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

function account(overrides: Partial<TdTradingAccountRow> = {}): TdTradingAccountRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    has_data: true,
    balance: 1_234_567.89,
    available: 850_000,
    frozen: 0,
    commission: 12.5,
    margin: 150_000,
    withdraw_quota: 800_000,
    deposit: 50_000,
    withdraw: 1234.5,
    seq: 10,
    ...overrides,
  }
}

/** 无资金记录的账户行（has_data:false；date/seq/资金字段全 null，契约 rest §2.9.4） */
function emptyAccount(overrides: Partial<TdTradingAccountRow> = {}): TdTradingAccountRow {
  return account({
    date: null,
    has_data: false,
    balance: null,
    available: null,
    frozen: null,
    commission: null,
    margin: null,
    withdraw_quota: null,
    deposit: null,
    withdraw: null,
    seq: null,
    ...overrides,
  })
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

/** 资金可用信封（契约 rest §2.9.2；资金端点不分页，next_cursor 恒 null） */
function fundListResponse(items: TdTradingAccountRow[], overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    items,
    next_cursor: null,
    ...overrides,
  }
}

/** 挂载仪表盘并等待持仓/资金基线到达 */
async function mountDashboard() {
  const wrapper = mount(DashboardView, { attachTo: document.body })
  await flushPromises()
  return wrapper
}

/**
 * 按账户 ID 找资金行（`data-test="fund-row"` 的 `data-account-id` 钩子）。
 *
 * `account_id` 可能被展示名替代（ADR 0056），因此不能按可见文本找；钩子保留 id 是
 * 「测试选择器稳定」的既定口径（#470）。
 */
function fundRow(wrapper: Awaited<ReturnType<typeof mountDashboard>>, accountId: string) {
  const row = wrapper.findAll('[data-test="fund-row"]')
    .find(item => item.attributes('data-account-id') === accountId)
  if (!row) throw new Error(`fund row not found: ${accountId}`)
  return row
}

/**
 * 登记一个交易账户并给进度（#509：资金面板状态列读 `tdAccounts` 行镜像，
 * 与交易账户页同一个 StatusIndicator；不再由 `online_accounts` 派生「在线/离线」）。
 *
 * 必须先于挂载调用：仪表盘挂载时 `tdAccountsStore.init()` 拉注册表，进度镜像随 Pinia
 * 单例一起生效。
 */
function seedTdAccountStatus(
  accountId: string,
  progress: { min: number; max: number; current: number; desc?: string },
): void {
  vi.mocked(tdApi.listAccounts).mockResolvedValue({
    available: true,
    accounts: [{ account_id: accountId, account_name: '', gateway: 'dztd_ctp', enabled: true }],
  })
  useProgressStore().applyProgress(accountId, progress)
}

describe('DashboardView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    vi.mocked(marketSourcesApi.list).mockResolvedValue([])
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([]))
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] })
    // #509 资金状态列读 tdAccounts 行镜像：网关进程按 Running 造镜像，账户才有「会话」
    // （进程没跑时状态列一律「未启动」，与真实页面一致）
    useProcessStore().applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 1 })
  })

  afterEach(() => {
    // 持仓/资金/账户注册表 store 的降级退避定时器：用例结束时统一释放
    useTdPositionsStore().dispose()
    useTdTradingAccountsStore().dispose()
    useTdAccountsStore().dispose()
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

  it('未知方向的持仓行在净仓列显示「未知」，不显示 0（#367 用户故事 9）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', direction: 'Unknown', volume: 4, seq: 10 }),
    ]))
    const w = await mountDashboard()

    const cell = w.get('[data-test="position-row-net"]')
    expect(cell.text()).toBe('未知')
    // 0 会被读成「已平仓」——正是要消灭的「看起来正常的错误值」。
    expect(cell.text()).not.toBe('0')
    // 不染涨跌色（它不是敞口），并给出可解释的 tooltip。
    expect(cell.classes()).toContain('cell-muted')
    expect(cell.attributes('title')).toContain('未知')

    w.unmount()
  })

  it('勾选「隐藏匹配行」不得隐藏未知方向行（它不是净仓为零）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', direction: 'Unknown', volume: 4, seq: 10 }),
    ]))
    const w = await mountDashboard()
    await w.get('[data-test="position-hide-matched"]').setValue(true)

    // 未知行净仓恒为 0；若被当成「匹配行」隐藏，用户就看不到「这里有读不懂的持仓」。
    expect(w.get('[data-test="position-row-net"]').text()).toBe('未知')

    w.unmount()
  })

  it('未知方向与已知多空并存时净仓照常轧差，但仍标「未知」（未知量不计入）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', direction: 'Long', volume: 5, seq: 10 }),
      position({ instrument_id: 'rb2601', direction: 'Short', volume: 2, seq: 11 }),
      position({ instrument_id: 'rb2601', direction: 'Unknown', volume: 9, seq: 12 }),
    ]))
    const w = await mountDashboard()

    const cell = w.get('[data-test="position-row-net"]')
    expect(cell.text()).toBe('未知')
    // 详情列仍能看到真实的多空明细（未知行不参与，但也没被丢掉）：
    // 明细 10 列 = 多 5 列 + 空 5 列，首列（多总）= 5、第六列（空总）= 2。
    await w.get('[data-test="position-detail-toggle"]').setValue(true)
    const metrics = w.findAll('td.position-detail-col').map(item => item.text())
    expect(metrics[0]).toBe('5')
    expect(metrics[5]).toBe('2')

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

  it('持仓/消息面板不再标注「演示」，其余两个演示面板仍标注（演示标签 2 个）', async () => {
    const w = await mountDashboard()
    const tags = w.findAll('[data-test="dashboard-demo-tag"]')
    expect(tags).toHaveLength(2)
    expect(tags.every(tag => tag.text() === '演示')).toBe(true)
    w.unmount()
  })

  it('横幅点名仍为演示数据的面板，并说明已接真实链路的面板', async () => {
    const w = await mountDashboard()
    const banner = w.get('[data-test="dashboard-demo-banner"]')
    const text = banner.text()
    expect(text).toContain('演示数据')
    for (const panel of ['委托', '策略']) {
      expect(text).toContain(panel)
    }
    // 消息面板已随 ADR 0060 接入真实通知链路：不得再出现在「尚未接入」的列举里
    expect(text).not.toContain('委托 / 策略 / 消息')
    // 账户面板已改「资金」并接真实链路：不得再被列为演示面板
    expect(text).not.toContain('账户')
    // 持仓（#249）/ 资金（#250）/ 消息（ADR 0060）已接入真实链路
    expect(text).toContain('「行情」「持仓」「资金」「消息」为实时数据')
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

  it('持仓分组与资金行的账户列显示展示名，id 保留在 title（ADR 0056/#470 AC3）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [
        { account_id: 'acc_a', account_name: '主账户', gateway: 'dztd_ctp', enabled: true },
        { account_id: 'acc_offline', account_name: '', gateway: 'dztd_ctp_2', enabled: false },
      ],
    })
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
    ]))
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([
      account(), emptyAccount({ account_id: 'acc_offline' }),
    ]))
    const w = await mountDashboard()

    // 持仓分组行：账户列=展示名、title=id（第 5 列，列序见模板）
    const positionRow = w.get('.dashboard-panel--position tr[data-account-id="acc_a"]')
    const posAccountCell = positionRow.findAll('td')[4]
    expect(posAccountCell.text()).toBe('主账户')
    expect(posAccountCell.attributes('title')).toBe('acc_a')

    // 资金行：同上；无名字的账户回落 id
    const fundCell = fundRow(w, 'acc_a').findAll('td')[0]
    expect(fundCell.text()).toBe('主账户')
    expect(fundCell.attributes('title')).toBe('acc_a')
    expect(fundRow(w, 'acc_offline').findAll('td')[0].text()).toBe('acc_offline')

    w.unmount()
  })

  it('持仓账户筛选面板选项显示展示名（仍按 id 过滤）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'acc_a', account_name: '主账户', gateway: 'dztd_ctp', enabled: true }],
    })
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse(
      [position({ account_id: 'acc_a' })], { online_accounts: ['acc_a'] }))
    const w = await mountDashboard()

    await w.get('[data-test="position-account-filter-trigger"]').trigger('click')
    const label = w.get('[data-test="position-account-filter"] .dashboard-pos-filter__label')
    expect(label.text()).toBe('主账户')
    expect(label.attributes('title')).toBe('acc_a')
    expect(w.get('[data-test="position-account-filter"] input[type="checkbox"]').attributes('aria-label'))
      .toBe('筛选持仓账户 主账户')

    w.unmount()
  })

  it('改名经 td_rtn_config 到达后历史行（REST 基线里的持仓/资金行）立刻显示新名字（ADR 0056 决策 9）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'acc_a', account_name: '旧名字', gateway: 'dztd_ctp', enabled: true }],
    })
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([position({ account_id: 'acc_a' })]))
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([account()]))
    const w = await mountDashboard()
    expect(w.get('.dashboard-panel--position tr[data-account-id="acc_a"]').findAll('td')[4].text()).toBe('旧名字')
    expect(fundRow(w, 'acc_a').findAll('td')[0].text()).toBe('旧名字')

    // 改名不广播 data_changed、历史行也不会重推：解析层按 id 现查，因此帧到达即全站生效
    useTdAccountsStore().applyTdConfig('dztd_ctp', {
      qry_min_interval_ms: 1100,
      accounts: [{ account_id: 'acc_a', account_name: '新名字', enabled: true, ext: { broker_id: '9999', password: '****' } }],
      ext: {},
    })
    await flushPromises()

    expect(w.get('.dashboard-panel--position tr[data-account-id="acc_a"]').findAll('td')[4].text()).toBe('新名字')
    expect(fundRow(w, 'acc_a').findAll('td')[0].text()).toBe('新名字')

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

  it('聚合数与展示行自洽（委托/策略仍是演示行，持仓/资金来自真实基线）', async () => {
    const w = await mountDashboard()
    // 持仓：空基线 → 0/0（不再是写死的演示行数）
    expect(w.get('[data-test="position-aggregate"]').text()).toBe('0/0')
    // 资金：空基线 → 在线 0 / 已登记 0
    expect(w.get('[data-test="fund-aggregate"]').text()).toBe('0/0')
    // 委托：7 笔中 2 笔未成交（旧值 2/12 与 7 行矛盾）
    expect(w.get('[data-test="orders-aggregate"]').text()).toBe('2/7 笔')
    // 策略：5 个策略中 4 个运行中（旧值 5/5 与「1 已停止」矛盾）
    expect(w.get('[data-test="strategy-aggregate"]').text()).toBe('4/5')
    expect(w.get('[data-test="strategy-footer"]').text()).toBe('总计: 5 已加载, 1 已停止')
    w.unmount()
  })

  // ---- #250：资金面板接真实数据 ----

  it('资金面板表头为契约资金字段，金额按 2 位小数 + 千分位格式化', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([
      account({
        account_id: 'acc_a',
        balance: 1_234_567.89,
        available: 850_000,
        frozen: 0,
        margin: 150_000,
        commission: 12.5,
        withdraw_quota: 800_000,
        deposit: 50_000,
        withdraw: 1234.5,
      }),
    ]))
    // 状态列读 tdAccounts 行镜像：acc_a 已登录（progress 到 max）
    seedTdAccountStatus('acc_a', { min: 0, max: 10, current: 10 })
    const w = await mountDashboard()

    for (const header of ['账户', '状态', '权益', '可用', '冻结', '保证金', '手续费', '可取', '出入金', '数据日期']) {
      expect(w.text()).toContain(header)
    }

    const row = fundRow(w, 'acc_a')
    const cells = row.findAll('td').map(td => td.text())
    // 列序：账户 | 状态 | 权益 | 可用 | 冻结 | 保证金 | 手续费 | 可取 | 出入金 | 数据日期 | 操作
    // 状态列 = StatusIndicator（#509）：徽章文案取自 `.ds-status__text`（单元格 text 会把
    // 隐藏的进度文案也算进来，不能直接断言整格 text）
    const status = row.findAll('td')[1].find('[data-test="fund-status"]')
    expect(status.classes()).toContain('ds-status--done')
    expect(status.find('.ds-status__text').text()).toBe('已登录')
    expect(cells.slice(2)).toEqual([
      '1,234,567.89',
      '850,000.00',
      '0.00',
      '150,000.00',
      '12.50',
      '800,000.00',
      '入 +50,000.00 / 出 -1,234.50',
      '2026-08-08',
      '',  // 操作列：刷新按钮无文本
    ])

    w.unmount()
  })

  it('状态列 = 登录态（已登录/未启动），界面不再出现「在线/离线」；多账户锚点日不同时全部列出', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse(
      [
        account({ account_id: 'acc_a', date: '20260808' }),
        account({ account_id: 'acc_offline', date: '20260807', seq: 11 }),
      ],
      { online_accounts: ['acc_a'] }))
    // 只有 acc_a 有进度镜像 → 已登录；acc_offline 没有行镜像 → 「未启动」（无数据 ≠ 未登录）
    seedTdAccountStatus('acc_a', { min: 0, max: 10, current: 10 })
    const w = await mountDashboard()

    const online = fundRow(w, 'acc_a').find('[data-test="fund-status"]')
    expect(online.classes()).toContain('ds-status--done')
    expect(online.find('.ds-status__text').text()).toBe('已登录')
    const noData = fundRow(w, 'acc_offline').find('[data-test="fund-status"]')
    expect(noData.classes()).toContain('ds-status--idle')
    expect(noData.find('.ds-status__text').text()).toBe('未启动')
    // 「在线/离线」这层词不再出现在界面上（它与登录态是同一件事的两种说法，#509）
    expect(w.text()).not.toContain('在线')
    expect(w.text()).not.toContain('离线')
    expect(w.get('[data-test="fund-date-label"]').text())
      .toBe('数据日期 2026-08-07 / 2026-08-08')
    // 行内数据日期是该账户锚点日，不是信封 trading_day
    expect(fundRow(w, 'acc_offline').find('[data-test="fund-row-date"]').text()).toBe('2026-08-07')

    w.unmount()
  })

  it('资金行失败态（四个登录链失败态）显示 ds-status--failed，不把失败说成「未登录」', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse(
      [account({ account_id: 'acc_a' })],
      { online_accounts: [] }))
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'acc_a', account_name: '', gateway: 'dztd_ctp', enabled: true }],
    })
    // 失败态停在中间值（ADR 0059）：进度数值只会说「进行中」，失败结论来自细状态（四个失败态）
    useTdAccountsStore().applyTdStatus('acc_a', {
      account_id: 'acc_a',
      state: 'LoginFailed',
      date: '',
      login_time: '',
      progress_desc: '登录失败',
      progress_min: 0,
      progress_max: 10,
      progress_current: 5,
      last_error: { code: 'login_rejected', message: '', error_id: 3, timestamp: 1_700_000_000 },
      ext: {},
    })
    useProgressStore().applyProgress('acc_a', { min: 0, max: 10, current: 5, desc: '登录失败' })

    const w = await mountDashboard()

    const status = fundRow(w, 'acc_a').find('[data-test="fund-status"]')
    expect(status.classes()).toContain('ds-status--failed')
    expect(status.find('.ds-status__text').text()).toBe('失败')

    w.unmount()
  })

  it('has_data:false 的账户整行金额/日期显示占位「—」，不用 0 冒充', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse(
      [emptyAccount({ account_id: 'acc_offline' })],
      { online_accounts: [] }))
    const w = await mountDashboard()

    const row = fundRow(w, 'acc_offline')
    const cells = row.findAll('td').map(td => td.text())
    // 状态列无行镜像 → 「未启动」（不是「离线」：那是刷新按钮的数据新鲜度语义）
    const status = row.findAll('td')[1].find('[data-test="fund-status"]')
    expect(status.find('.ds-status__text').text()).toBe('未启动')
    // 列序：账户 | 状态 | 权益 | 可用 | 冻结 | 保证金 | 手续费 | 可取 | 出入金 | 数据日期 | 操作
    expect(cells.slice(2)).toEqual([
      '—', '—', '—', '—', '—', '—', '入 — / 出 —', '—', '',
    ])
    // title 说明「该账户无资金记录」
    expect(row.attributes('title')).toBe('该账户无资金记录')

    w.unmount()
  })

  it('资金聚合数为 在线账户/已登记账户（含离线与无记录账户）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse(
      [
        account({ account_id: 'acc_a' }),
        account({ account_id: 'acc_b', seq: 11 }),
        emptyAccount({ account_id: 'acc_offline' }),
      ],
      { online_accounts: ['acc_a', 'acc_b'] }))
    const w = await mountDashboard()

    expect(w.get('[data-test="fund-aggregate"]').text()).toBe('2/3')
    w.unmount()
  })

  it('资金降级 available:false 时展示统一提示，空态行同样是降级文案', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue({ available: false, reason: 'not_found' })
    const w = await mountDashboard()

    const banner = w.get('[data-test="fund-degraded-banner"]')
    expect(banner.text()).toContain('交易数据暂不可用')
    expect(w.get('[data-test="fund-empty"]').text()).toBe('交易数据暂不可用')

    w.unmount()
  })

  it('资金请求失败：显示错误行与「加载失败」空态，不冒充「暂无资金数据」', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockRejectedValue(new Error('HTTP 500'))
    const w = await mountDashboard()

    expect(w.get('[data-test="fund-error"]').text()).toBe('HTTP 500')
    expect(w.get('[data-test="fund-empty"]').text()).toBe('加载失败')
    expect(w.text()).not.toContain('暂无资金数据')
    expect(w.find('[data-test="fund-degraded-banner"]').exists()).toBe(false)
    w.unmount()
  })

  it('成功读到空资金基线时显示「暂无资金数据」空态（与降级/失败区分）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const w = await mountDashboard()

    expect(w.find('[data-test="fund-degraded-banner"]').exists()).toBe(false)
    expect(w.find('[data-test="fund-error"]').exists()).toBe(false)
    expect(w.get('[data-test="fund-empty"]').text()).toBe('暂无资金数据')
    w.unmount()
  })

  it('资金基线请求在途时显示「加载中...」（未初始化 ≠ 无数据）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockImplementation(
      () => new Promise<TdTradingAccountsResponse>(() => {}))
    const w = await mountDashboard()

    expect(w.get('[data-test="fund-empty"]').text()).toBe('加载中...')
    w.unmount()
  })

  it('data_changed 重拉在途时显示「加载中...」，不把刷新中冒充「暂无资金数据」', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const w = await mountDashboard()
    // 已初始化空基线：此刻确为「暂无资金数据」
    expect(w.get('[data-test="fund-empty"]').text()).toBe('暂无资金数据')

    // data_changed 会先清空 rows 再重拉；请求在途期间不得显示「暂无资金数据」
    vi.mocked(tdApi.getTradingAccounts).mockImplementation(
      () => new Promise<TdTradingAccountsResponse>(() => {}))
    void useTdTradingAccountsStore().handleDataChanged('td')
    await w.vm.$nextTick()

    expect(useTdTradingAccountsStore().loading).toBe(true)
    expect(w.get('[data-test="fund-empty"]').text()).toBe('加载中...')
    expect(w.text()).not.toContain('暂无资金数据')

    w.unmount()
  })

  it('data_changed 重拉失败时显示「加载失败」与错误行，不冒充「暂无资金数据」', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([]))
    const w = await mountDashboard()
    expect(w.get('[data-test="fund-empty"]').text()).toBe('暂无资金数据')

    // 重拉（清空后）HTTP 失败：error 非空但 available 仍 true，不得落回「暂无资金数据」
    vi.mocked(tdApi.getTradingAccounts).mockRejectedValue(new Error('HTTP 500'))
    await useTdTradingAccountsStore().handleDataChanged('td')

    expect(w.get('[data-test="fund-empty"]').text()).toBe('加载失败')
    expect(w.get('[data-test="fund-error"]').text()).toBe('HTTP 500')
    expect(w.text()).not.toContain('暂无资金数据')
    w.unmount()
  })

  // ---- #358：无法确认数据时不得渲染成「暂无」（五态互斥）----

  it('无已登记账户（持仓）时渲染「持仓未知」+ 面板级提示条，不渲染「暂无持仓」', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([], { online_accounts: [] }))
    const w = await mountDashboard()

    expect(w.get('[data-test="position-empty"]').text()).toBe('持仓未知')
    const banner = w.get('[data-test="position-unknown-banner"]')
    expect(banner.text()).toContain('持仓未知')
    expect(banner.text()).toContain('交易网关')
    expect(banner.text()).toContain('账户')
    // 假空态与假降级都不得出现
    expect(w.text()).not.toContain('暂无持仓')
    expect(w.find('[data-test="position-degraded-banner"]').exists()).toBe(false)

    w.unmount()
  })

  it('无已登记账户（资金）时渲染「资金未知」+ 面板级提示条，不渲染「暂无资金数据」', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse([], { online_accounts: [] }))
    const w = await mountDashboard()

    expect(w.get('[data-test="fund-empty"]').text()).toBe('资金未知')
    const banner = w.get('[data-test="fund-unknown-banner"]')
    expect(banner.text()).toContain('资金未知')
    expect(banner.text()).toContain('交易网关')
    expect(w.text()).not.toContain('暂无资金数据')
    expect(w.find('[data-test="fund-degraded-banner"]').exists()).toBe(false)

    w.unmount()
  })

  it('持仓请求失败时渲染「加载失败」，不渲染「暂无持仓」（initialized 置 true）', async () => {
    vi.mocked(tdApi.getPositions).mockRejectedValue(new Error('HTTP 500'))
    const w = await mountDashboard()

    expect(w.get('[data-test="position-empty"]').text()).toBe('加载失败')
    expect(w.text()).not.toContain('暂无持仓')
    expect(w.find('[data-test="position-unknown-banner"]').exists()).toBe(false)

    w.unmount()
  })

  it('持仓首次加载在途时渲染「加载中...」，不渲染空白 tbody 或「暂无持仓」', async () => {
    vi.mocked(tdApi.getPositions).mockImplementation(
      () => new Promise<never>(() => {}))
    const w = await mountDashboard()

    // 原先 `v-if="!loading"` 让加载态分支成为死代码：tbody 完全空白
    expect(w.get('[data-test="position-empty"]').text()).toBe('加载中...')
    expect(w.text()).not.toContain('暂无持仓')

    w.unmount()
  })

  it('已知账户但无持仓行时仍显示「暂无持仓」（online_accounts 非空即视为已知账户）', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([], { online_accounts: ['acc_a'] }))
    const w = await mountDashboard()

    expect(w.get('[data-test="position-empty"]').text()).toBe('暂无持仓')
    expect(w.find('[data-test="position-unknown-banner"]').exists()).toBe(false)

    w.unmount()
  })

  it('0/0 聚合数不使用成功色，有持仓组时才应用 positive 修饰', async () => {
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([], { online_accounts: ['acc_a'] }))
    const w = await mountDashboard()

    const aggregate = w.get('[data-test="position-aggregate"]')
    expect(aggregate.text()).toBe('0/0')
    expect(aggregate.classes()).not.toContain('dashboard-panel__aggregate--positive')

    // 有持仓组时恢复成功色（原口径不变）
    vi.mocked(tdApi.getPositions).mockResolvedValue(positionListResponse([
      position({ instrument_id: 'rb2601', volume: 2 }),
    ]))
    await useTdPositionsStore().resetAndReload()
    await w.vm.$nextTick()

    expect(w.get('[data-test="position-aggregate"]').classes())
      .toContain('dashboard-panel__aggregate--positive')

    w.unmount()
  })

  it('资金行手动刷新：在线行可点并发起请求、pending 生效；离线行禁用（契约 webui-ws §5 第 15 条）', async () => {
    vi.mocked(tdApi.getTradingAccounts).mockResolvedValue(fundListResponse(
      [account(), account({ account_id: 'acc_offline' })],
      { online_accounts: ['acc_a'] },
    ))
    vi.mocked(tdApi.refreshTradingAccount).mockResolvedValue({ ok: true })

    const w = await mountDashboard()
    const onlineButton = fundRow(w, 'acc_a').find('[data-test="fund-refresh"]')
    const offlineButton = fundRow(w, 'acc_offline').find('[data-test="fund-refresh"]')
    expect(onlineButton.attributes('disabled')).toBeUndefined()
    expect(offlineButton.attributes('disabled')).toBeDefined()
    expect(offlineButton.attributes('title')).toBe('账户未在线，不可刷新')

    await onlineButton.trigger('click')
    await flushPromises()
    expect(tdApi.refreshTradingAccount).toHaveBeenCalledWith('acc_a')
    // 成功请求保持 pending（清 pending 的唯一信号 = 同账户新 seq），按钮进入刷新中态。
    expect(useTdTradingAccountsStore().refreshPending.acc_a).toBe(true)
    expect(fundRow(w, 'acc_a').find('[data-test="fund-refresh"]').attributes('data-pending')).toBe('true')
    w.unmount()
  })

})
