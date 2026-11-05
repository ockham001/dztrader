<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, type Ref } from 'vue'
import StatusIndicator from '@/components/shared/StatusIndicator.vue'
import Icon from '@/components/shared/Icon.vue'
import { processStateText, processStateColor } from '@/composables/useProcessState'
import { formatSignedVolume, formatTdDate, positionDetailTexts } from '@/composables/tdPositionView'
import { TD_POSITION_DETAIL_COLUMNS } from '@/composables/tdPositionView'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useTdPositionsStore } from '@/stores/tdPositions'

const marketSourcesStore = useMarketSourcesStore()
const positionsStore = useTdPositionsStore()

// 行情面板：复用 store 的 sources，与行情源页面同步
const marketOnlineCount = computed(
  () => marketSourcesStore.sources.filter(s => s.loginState === 'online').length,
)
const marketTotalCount = computed(() => marketSourcesStore.sources.length)

// 行情进程状态标签：仅在非 running 状态显示（running 时由 StatusIndicator 表达）
// running 时不显示标签，避免与 StatusIndicator 的"已登录"冗余；文案复用共享 processStateText
function marketProcessStateText(state: string | null): string {
  if (!state || state === 'running') return ''
  return processStateText(state)
}

// === Types ===
interface AccountRow {
  name: string
  sub: string
  status: 'logged-in' | 'logged-out' | 'loading'
  loadingText: string
  equity: string
  risk: string
  riskColor: string
  available: string
  margin: string
  deposit: string
  profit: string
  profitColor: string
  fee: string
}

interface OrderRow {
  contract: string
  icon: 'pending' | 'filled' | 'cancelled' | 'rejected'
  source: string
  account: string
  direction: 'buy' | 'sell'
  filled: string
  price: string
  avgPrice: string
  time: string
  tagText: string
  tagClass: string
  rowClass: string
}

interface StrategyRow {
  name: string
  id: string
  lang: string
  running: boolean
}

interface MessageRow {
  level: 'info' | 'warning' | 'error'
  text: string
  time: string
  source: string
}

// === 持仓面板：真实数据（#249，契约 rest §2.9.4 / 契约 webui-ws §2.5/§5）===
// 面板接的是 `stores/tdPositions`（REST 基线 + WS 绝对态增量 + 降级退避）。
// 一行 = (account_id, instrument_id)（用户确认口径）：契约行是 (account, instrument,
// direction) 三元组，面板按合约分组，默认显示轧差后的账户净仓，多/空明细进可展开详情列。
//
// 策略列（策略净仓 / 逐策略净仓 / 持仓差值）**保留列位置显示「—」**：策略维度的持仓归属
// 尚未建立（策略侧记账只在策略进程内，契约 strategy §「策略逻辑持仓」），不能拿账户净仓
// 冒充策略净仓。列位置保留是为了布局稳定 + 让读者看到「这里将来会有数据」。

/**
 * 策略名列表（**演示数据**的一部分）：策略维度持仓归属尚未建立，这些列只保留位置显示「—」。
 * 与账户/委托/策略/消息面板同属 issue #200 的演示数据块。
 */
const strategyNames = ['长线', '中线', '短线1号', '短线2号', '多因子一号']

/** 详情列（多总/多今/多昨/多冻结/多均价 × 空方）默认隐藏，勾选后展开 */
const showPositionDetail = ref(false)

/** 详情列表头（顺序与 `positionDetailTexts` 的返回值一一对应） */
const positionDetailColumns = TD_POSITION_DETAIL_COLUMNS

/** 面板行数（聚合数用）：一行 = 一个 (账户, 合约) */
const positionAggregate = computed(() => {
  const total = positionsStore.groups.length
  if (total === 0) return '0/0'
  const netted = positionsStore.groups.filter(group => group.netVolume !== 0).length
  // 匹配数 = 净仓非零的行（与「隐藏匹配行」勾选框的口径一致：勾上后隐藏净仓为零的行）
  return `${netted}/${total}`
})

/**
 * 「隐藏匹配行」勾选框：原先是无逻辑的假交互（issue #200 遗留）。
 * 现在接真实语义：勾上后隐藏**账户净仓为零**的行（多空锁仓 / 已全平但仍残留今昨明细的行）——
 * 这类行对"当前风险敞口"没有贡献，但仍在列表里占位。聚合数始终按全部行计算（匹配数/总数），
 * 因此勾选不会让分母跳动。
 */
const hideMatchedPositions = ref(false)

const visiblePositionGroups = computed(() => {
  if (!hideMatchedPositions.value) return positionsStore.groups
  return positionsStore.groups.filter(group => group.netVolume !== 0)
})

/** 面板级数据日期标签：各行锚点日可能不同（多账户），只标一个日期会说谎 */
const positionDateLabel = computed<string>(() => {
  const dates = positionsStore.anchorDates
  if (dates.length === 0) return ''
  const shown = dates.map(formatTdDate).join(' / ')
  return `数据日期 ${shown}`
})

/** 净仓文案：正数带 +、负数原样、零显示 0（零是合法真实值，不是不可用） */
function netVolumeText(volume: number): string {
  return formatSignedVolume(volume)
}

/** 净仓单元格类名（红涨绿跌，复用 dashboard.css 既有口径） */
function netVolumeClass(volume: number): string {
  return volume > 0 ? 'diff-pos' : volume < 0 ? 'diff-neg' : ''
}

// === 持仓账户筛选（契约 webui-ws §5 第 11 条）===
// 选项来源 = 返回的账户全集（含离线），**不是** online_accounts；筛选变化丢弃游标从首页重拉
// （由 store 的 setAccountFilter 负责）。
const positionFilterOpen = ref(false)

/** 筛选按钮文案：空筛选 / 全选 = 「全部账户（N）」 */
const positionFilterLabel = computed<string>(() => {
  const total = positionsStore.accounts.length
  if (total === 0) return '账户 0'
  if (positionsStore.allAccountsSelected || positionsStore.effectiveAccounts.length === 0) {
    return `全部账户（${total}）`
  }
  return `账户 ${positionsStore.effectiveAccounts.length}/${total}`
})

function isPositionAccountSelected(account: string): boolean {
  if (positionsStore.allAccountsSelected) return true
  return positionsStore.accountFilter.has(account)
}

/** 唯一生效账户时禁用取消：空筛选在契约里 = 全部已登记账户（rest §2.9.2），会显示更多数据 */
function isLastPositionAccount(account: string): boolean {
  const effective = positionsStore.effectiveAccounts
  return effective.length === 1 && effective[0] === account
}

/** 切换单个账户（不允许取消最后一个；重置重拉由 store 负责） */
function togglePositionAccount(account: string): void {
  const all = positionsStore.accounts
  const next = positionsStore.allAccountsSelected
    ? all.filter(item => item !== account)
    : all.filter(item => (
        item === account ? !positionsStore.accountFilter.has(item) : positionsStore.accountFilter.has(item)
      ))
  if (next.length === 0) return
  void positionsStore.setAccountFilter(next)
}

function selectAllPositionAccounts(): void {
  void positionsStore.selectAllAccounts()
}

/** 空态行的 colspan：固定列（合约/差值/净仓/策略净仓/账户/日期）+ 策略列 + 详情列 */
const positionColumnCount = computed(
  () => 6 + strategyNames.length + (showPositionDetail.value ? positionDetailColumns.length : 0),
)

// === 账户 / 委托 / 策略 / 消息（演示数据，issue #200）===
// 这四个面板的交易数据链路尚未建立（issue #6：TdDataService 未暴露 REST/WS），此处保留
// 原型布局，但满足三条约束：
//   1) 页面永久可见地标注「演示数据」（横幅 + 逐面板 ds-tag）；
//   2) 券商名 / 账户号 / 合约代码全部虚构，不得与真实期货公司或真实合约撞名；
//   3) 面板聚合数一律由下列行数据计算，禁止硬编码（历史版本存在 2/3、5/5 等矛盾数字）。
// 持仓面板已随 #249 接入真实链路，不再属于演示数据。
const accounts: AccountRow[] = [
  {
    name: '演示期货_A', sub: 'demo-0001 · ctp', status: 'logged-in', loadingText: '',
    equity: '1,890,887.80', risk: '62.3%', riskColor: 'var(--status-alert-default)',
    available: '715,340.50', margin: '1,175,547.30', deposit: '+50,000', profit: '+12,350',
    profitColor: '#2a9d5c', fee: '1,230.50',
  },
  {
    name: '演示期货_B', sub: 'demo-0002 · xtp', status: 'loading', loadingText: '查询持仓...',
    equity: '2,345,621.35', risk: '38.1%', riskColor: 'var(--status-success-default)',
    available: '1,451,230.80', margin: '894,390.55', deposit: '0', profit: '-8,560',
    profitColor: '#d4543e', fee: '3,560.20',
  },
  {
    name: '演示期货_C', sub: 'demo-0003 · ctptest', status: 'logged-out', loadingText: '',
    equity: '--', risk: '--', riskColor: 'var(--text-tertiary)',
    available: '--', margin: '--', deposit: '--', profit: '--',
    profitColor: 'var(--text-tertiary)', fee: '--',
  },
]

const orders: OrderRow[] = [
  { contract: 'zz2601', icon: 'pending', source: '长线', account: '演示期货_A', direction: 'buy', filled: '5/10', price: '3,680', avgPrice: '3,682', time: '14:05:23', tagText: '部分成交', tagClass: 'ds-tag--alert', rowClass: '' },
  { contract: 'yy2601', icon: 'filled', source: '中线', account: '演示期货_A', direction: 'sell', filled: '8/8', price: '3,915', avgPrice: '3,915', time: '14:03:12', tagText: '全部成交', tagClass: 'ds-tag--success', rowClass: '' },
  { contract: 'zz2601', icon: 'filled', source: '长线', account: '演示期货_A', direction: 'sell', filled: '3/3', price: '3,695', avgPrice: '3,694', time: '14:01:45', tagText: '全部成交', tagClass: 'ds-tag--success', rowClass: '' },
  { contract: 'xx2605', icon: 'pending', source: '短线1号', account: '演示期货_A', direction: 'buy', filled: '0/20', price: '785.5', avgPrice: '--', time: '13:58:30', tagText: '未成交', tagClass: '', rowClass: '' },
  { contract: 'ww2601', icon: 'cancelled', source: '短线2号', account: '演示期货_B', direction: 'buy', filled: '0/5', price: '3,720', avgPrice: '--', time: '13:50:22', tagText: '已撤单', tagClass: '', rowClass: 'order-row--cancelled' },
  { contract: 'ww2605', icon: 'rejected', source: '多因子一号', account: '演示期货_A', direction: 'sell', filled: '0/10', price: '2,350', avgPrice: '--', time: '13:45:10', tagText: '废单', tagClass: 'ds-tag--danger', rowClass: 'order-row--rejected' },
  { contract: 'yy2601', icon: 'filled', source: '多因子一号', account: '演示期货_B', direction: 'buy', filled: '10/10', price: '3,908', avgPrice: '3,907', time: '13:55:18', tagText: '全部成交', tagClass: 'ds-tag--success', rowClass: '' },
]

const strategyList: StrategyRow[] = [
  { name: '长线', id: 'ocm_001', lang: 'C/C++', running: true },
  { name: '中线', id: 'ocm_015', lang: 'Python', running: true },
  { name: '短线1号', id: 'ddb_nb', lang: 'C/C++', running: true },
  { name: '短线2号', id: 'ocm_013', lang: 'Python', running: false },
  { name: '多因子一号', id: 'ocm_101', lang: 'Python', running: true },
]

const messages: MessageRow[] = [
  { level: 'info', text: '多因子一号 zz2601 平仓信号触发', time: '1分钟前', source: '多因子一号' },
  { level: 'info', text: '中线 xx2601 止损触发', time: '5分钟前', source: '中线' },
  { level: 'info', text: '长线 yy2601 加仓执行完成，当前持仓 10 手，均价 38520', time: '18分钟前', source: '长线' },
  { level: 'info', text: '短线2号 参数热更新完成', time: '32分钟前', source: '短线2号' },
  { level: 'warning', text: '行情源 CTP 断线重连成功，已补发 12 笔行情快照', time: '1小时前', source: 'md_ctp' },
  { level: 'error', text: '演示品种套利 zz2601 订单被拒：超出可开仓手数限制', time: '2小时前', source: '短线1号' },
  { level: 'info', text: '账户权益更新', time: '3小时前', source: 'account_ctp' },
  { level: 'warning', text: '期货备用登录超时，30秒后重试', time: '5小时前', source: 'account_ctp' },
  { level: 'info', text: '短线1号 ww2605 开仓，方向：空，手数：5，委托价 1580', time: '6小时前', source: '短线1号' },
]

// === 面板聚合数：一律由行数据计算（issue #200）===
// 历史版本硬编码 2/3、7/8、2/12、5/5 与展示行互相矛盾；此处改为派生，
// 行数据变化时聚合数自动跟随，不可能再漂移。
const accountAggregate = computed(
  () => `${accounts.filter(a => a.status === 'logged-in').length}/${accounts.length}`,
)
const ordersAggregate = computed(
  () => `${orders.filter(o => o.icon === 'pending').length}/${orders.length} 笔`,
)
const strategyAggregate = computed(
  () => `${strategyList.filter(s => s.running).length}/${strategyList.length}`,
)
const strategyFooter = computed(
  () => `总计: ${strategyList.length} 已加载, ${strategyList.filter(s => !s.running).length} 已停止`,
)

// === Splitter state (flex-grow values) ===
const leftFlex = ref(1)
const rightFlex = ref(0.382)
const positionFlex = ref(1.618)
const bottomFlex = ref(1)
const accountFlex = ref(1)
const orderFlex = ref(1.618)
const marketFlex = ref(1)
const strategyFlex = ref(1.618)
const msgFlex = ref(1.618)

// === Template refs for container measurement ===
const dashboardContent = ref<HTMLElement | null>(null)
const leftPanelEl = ref<HTMLElement | null>(null)
const bottomPanelEl = ref<HTMLElement | null>(null)
const rightPanelEl = ref<HTMLElement | null>(null)

// === Splitter drag logic ===
interface DragInfo {
  axis: 'x' | 'y'
  startPos: number
  containerSize: number
  total: number
  apply: (deltaRatio: number) => void
}

const dragInfo = ref<DragInfo | null>(null)

function startTwoPanelDrag(
  e: MouseEvent,
  axis: 'x' | 'y',
  container: HTMLElement,
  flexA: Ref<number>,
  flexB: Ref<number>,
): void {
  e.preventDefault()
  const containerSize = axis === 'x' ? container.clientWidth : container.clientHeight
  const total = flexA.value + flexB.value
  const startA = flexA.value
  dragInfo.value = {
    axis,
    startPos: axis === 'x' ? e.clientX : e.clientY,
    containerSize,
    total,
    apply: (deltaRatio: number) => {
      const newA = Math.max(0.1, Math.min(total - 0.1, startA + deltaRatio))
      flexA.value = newA
      flexB.value = total - newA
    },
  }
  window.addEventListener('mousemove', onSplitterMouseMove)
  window.addEventListener('mouseup', onSplitterMouseUp)
}

function startMarketDrag(e: MouseEvent): void {
  if (!rightPanelEl.value) return
  e.preventDefault()
  const containerSize = rightPanelEl.value.clientHeight
  const total = marketFlex.value + strategyFlex.value + msgFlex.value
  const startMarket = marketFlex.value
  const startStrategy = strategyFlex.value
  const startMsg = msgFlex.value
  dragInfo.value = {
    axis: 'y',
    startPos: e.clientY,
    containerSize,
    total,
    apply: (deltaRatio: number) => {
      const newMarket = Math.max(0.1, Math.min(total - 0.2, startMarket + deltaRatio))
      const actualDelta = newMarket - startMarket
      const restTotal = startStrategy + startMsg
      if (restTotal > 0.001) {
        strategyFlex.value = startStrategy - actualDelta * (startStrategy / restTotal)
        msgFlex.value = startMsg - actualDelta * (startMsg / restTotal)
      }
      marketFlex.value = newMarket
    },
  }
  window.addEventListener('mousemove', onSplitterMouseMove)
  window.addEventListener('mouseup', onSplitterMouseUp)
}

function startStrategyMsgDrag(e: MouseEvent): void {
  if (!rightPanelEl.value) return
  e.preventDefault()
  const containerSize = rightPanelEl.value.clientHeight
  const total = strategyFlex.value + msgFlex.value
  const startStrategy = strategyFlex.value
  dragInfo.value = {
    axis: 'y',
    startPos: e.clientY,
    containerSize,
    total,
    apply: (deltaRatio: number) => {
      const newStrategy = Math.max(0.1, Math.min(total - 0.1, startStrategy + deltaRatio))
      strategyFlex.value = newStrategy
      msgFlex.value = total - newStrategy
    },
  }
  window.addEventListener('mousemove', onSplitterMouseMove)
  window.addEventListener('mouseup', onSplitterMouseUp)
}

function onSplitterMouseMove(e: MouseEvent): void {
  const di = dragInfo.value
  if (!di) return
  const currentPos = di.axis === 'x' ? e.clientX : e.clientY
  const delta = currentPos - di.startPos
  const deltaRatio = (delta / di.containerSize) * di.total
  di.apply(deltaRatio)
}

function onSplitterMouseUp(): void {
  dragInfo.value = null
  window.removeEventListener('mousemove', onSplitterMouseMove)
  window.removeEventListener('mouseup', onSplitterMouseUp)
}

// === Splitter mousedown handlers ===
function onVSplitter2Down(e: MouseEvent): void {
  if (dashboardContent.value) startTwoPanelDrag(e, 'x', dashboardContent.value, leftFlex, rightFlex)
}
function onVSplitter1Down(e: MouseEvent): void {
  if (leftPanelEl.value) startTwoPanelDrag(e, 'y', leftPanelEl.value, positionFlex, bottomFlex)
}
function onHSplitterDown(e: MouseEvent): void {
  if (bottomPanelEl.value) startTwoPanelDrag(e, 'x', bottomPanelEl.value, accountFlex, orderFlex)
}

// === WebSocket lifecycle ===
// WS 连接/断开由 App.vue 全局管理，DashboardView 不再负责。
// 持仓面板是唯一在本页消费交易数据的面板：挂载即拉 REST 基线，卸载即停降级重试
// （store 是单例，无视图时不该产生流量）。
onMounted(() => {
  void positionsStore.init()
})

onUnmounted(() => {
  onSplitterMouseUp()
  positionsStore.dispose()
})
</script>

<template>
  <div
    ref="dashboardContent"
    class="dashboard-content"
    :style="{
        flex: 1,
        display: 'grid',
        gridTemplateColumns: `minmax(300px, ${leftFlex}fr) 6px minmax(200px, ${rightFlex}fr)`,
        gridTemplateRows: 'auto minmax(0, 1fr)',
        overflow: 'hidden',
        padding: '6px',
        columnGap: '0',
        rowGap: 'var(--spacer-6)',
      }"
    >
      <!-- 演示数据横幅（issue #200）：首屏永久可见，与是否滚动 / 展开无关。
           账户 / 委托 / 策略 / 消息四个面板的交易数据链路尚未建立（issue #6），横幅是读者
           第一眼就应看到的事实声明，不得改为可关闭 banner 或仅靠图注。持仓面板已随 #249
           接入真实链路，已从本横幅（与逐面板 ds-tag）中移除。仪表盘锁一屏高度，故压成单行紧凑条。 -->
      <div
        class="ds-alert ds-alert--warning dashboard-demo-banner"
        data-test="dashboard-demo-banner"
        role="status"
      >
        <span class="ds-alert__icon"><Icon name="Tips" :size="16" /></span>
        <span class="dashboard-demo-banner__text">
          <strong>演示数据</strong>：账户 / 委托 / 策略 / 消息面板尚未接入真实链路，内容均为虚构；「行情」与「持仓」面板为实时数据。
        </span>
      </div>

      <!-- Left Panel -->
      <div ref="leftPanelEl" class="dashboard-left-panel" :style="{ display: 'flex', flexDirection: 'column', overflow: 'hidden', minWidth: 0 }">

        <!-- Position Monitor（#249：真实持仓；契约 rest §2.9.4 / 契约 webui-ws §2.5/§5） -->
        <div class="dashboard-panel dashboard-panel--position" :style="{ flex: `${positionFlex} 1 0%`, minHeight: '120px', display: 'flex', flexDirection: 'column', overflow: 'hidden' }">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--border-neutral-l1)', flexShrink: 0 }">
            <span :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)', minWidth: 0 }">
              <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">持仓</span>
              <!-- 数据日期（面板级）：行内「数据日期」列是权威来源；这里给整屏一眼可读的标记。
                   持仓行的 date 是各账户锚点日，可能早于当前交易日 —— 读者必须看得出这是旧快照。 -->
              <span
                v-if="positionDateLabel"
                data-test="position-date-label"
                :style="{ fontSize: '10px', color: 'var(--text-tertiary)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }"
              >{{ positionDateLabel }}</span>
            </span>
            <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-12)', fontSize: 'var(--body-xs-font-size)' }">
              <div class="dashboard-pos-filter">
                <button
                  class="filter-dropdown__trigger"
                  type="button"
                  :aria-expanded="positionFilterOpen"
                  aria-controls="dashboard-position-account-filter"
                  :style="{ cursor: 'pointer', border: 'none', background: 'transparent', padding: 0, color: 'inherit', font: 'inherit' }"
                  data-test="position-account-filter-trigger"
                  @click="positionFilterOpen = !positionFilterOpen"
                >{{ positionFilterLabel }}</button>
                <div v-if="positionFilterOpen" id="dashboard-position-account-filter" class="dashboard-pos-filter__panel">
                  <div class="dashboard-pos-filter__head">
                    <span class="dashboard-pos-filter__title">账户筛选</span>
                    <button
                      v-if="!positionsStore.allAccountsSelected"
                      class="dashboard-pos-filter__all"
                      type="button"
                      @click="selectAllPositionAccounts"
                    >全选</button>
                  </div>
                  <p v-if="positionsStore.accounts.length === 0" class="dashboard-pos-filter__empty">暂无已登记账户</p>
                  <div v-else data-test="position-account-filter" class="dashboard-pos-filter__list">
                    <label v-for="account in positionsStore.accounts" :key="account" class="ds-check">
                      <input
                        type="checkbox"
                        :checked="isPositionAccountSelected(account)"
                        :disabled="isLastPositionAccount(account)"
                        :aria-label="`筛选持仓账户 ${account}`"
                        :title="isLastPositionAccount(account) ? '至少保留一个账户；要显示全部请用「全选」' : ''"
                        @change="togglePositionAccount(account)"
                      >
                      <span class="ds-check__box"></span>
                      <span class="dashboard-pos-filter__label">{{ account }}</span>
                    </label>
                  </div>
                </div>
              </div>
              <label class="ds-check" :style="{ minHeight: 'auto', fontSize: 'var(--body-xs-font-size)', color: 'var(--text-secondary)' }">
                <input v-model="showPositionDetail" type="checkbox" data-test="position-detail-toggle">
                <span class="ds-check__box"></span> 详情
              </label>
              <label class="ds-check" :style="{ minHeight: 'auto', fontSize: 'var(--body-xs-font-size)', color: 'var(--text-secondary)' }">
                <input v-model="hideMatchedPositions" type="checkbox" data-test="position-hide-matched">
                <span class="ds-check__box"></span> 隐藏匹配行
              </label>
              <span data-test="position-aggregate" :style="{ fontSize: 'var(--body-xs-font-size)', fontVariantNumeric: 'tabular-nums', color: 'var(--status-success-default)', cursor: 'default' }" title="净仓非零行/全部行">{{ positionAggregate }}</span>
            </div>
          </div>
          <!-- 降级提示条（契约 webui-ws §5 第 13 条：统一提示 + 自动退避重试，不用 0 冒充数据） -->
          <div
            v-if="!positionsStore.available"
            class="ds-alert ds-alert--warning dashboard-pos-degraded"
            role="status"
            data-test="position-degraded-banner"
          >
            <span class="ds-alert__icon"><Icon name="Tips" :size="14" /></span>
            <span>{{ positionsStore.degradedText }}</span>
          </div>
          <div v-if="positionsStore.truncated" class="dashboard-pos-truncated" role="status" data-test="position-truncated">
            持仓行数超过单次拉取上限，列表不完整；请用账户筛选缩小范围
          </div>
          <div v-if="positionsStore.error" class="dashboard-pos-error" role="alert">{{ positionsStore.error }}</div>
          <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto', padding: 0 }">
            <table class="ds-table ds-table--dense" :style="{ minWidth: '100%', width: 'max-content', tableLayout: 'auto', whiteSpace: 'nowrap' }">
              <thead :style="{ position: 'sticky', top: 0, zIndex: 1 }">
                <tr>
                  <th class="frozen frozen-last" :style="{ left: 0 }">合约代码</th>
                  <th>持仓差值</th>
                  <th>账户净仓</th>
                  <th>策略净仓</th>
                  <th>账户</th>
                  <th>数据日期</th>
                  <th v-for="name in strategyNames" :key="name" class="strategy-col">{{ name }}</th>
                  <template v-if="showPositionDetail">
                    <th v-for="name in positionDetailColumns" :key="name" class="position-detail-col">{{ name }}</th>
                  </template>
                </tr>
              </thead>
              <tbody>
                <tr v-for="group in visiblePositionGroups" :key="group.key">
                  <td class="frozen frozen-last cell-text" :style="{ left: 0, fontWeight: 'var(--font-weight-medium)' }">{{ group.instrument_id }}</td>
                  <!-- 策略维度持仓归属尚未建立（策略记账在策略进程内）：保留列位置显示占位，
                       不拿账户净仓冒充策略净仓（契约 strategy §「策略逻辑持仓」）。 -->
                  <td class="cell-num" :style="{ color: 'var(--text-tertiary)' }" title="策略维度持仓归属尚未建立">—</td>
                  <td class="cell-num" :class="netVolumeClass(group.netVolume)">{{ netVolumeText(group.netVolume) }}</td>
                  <td class="cell-num" :style="{ color: 'var(--text-tertiary)' }" title="策略维度持仓归属尚未建立">—</td>
                  <td class="cell-text" :style="{ color: 'var(--text-secondary)' }">{{ group.account_id }}</td>
                  <td class="cell-num" :style="{ color: 'var(--text-tertiary)' }" data-test="position-row-date">{{ formatTdDate(group.date) }}</td>
                  <td v-for="name in strategyNames" :key="name" class="cell-num strategy-col" title="策略维度持仓归属尚未建立">—</td>
                  <template v-if="showPositionDetail">
                    <td
                      v-for="(text, index) in positionDetailTexts(group)"
                      :key="index"
                      class="cell-num position-detail-col"
                    >{{ text }}</td>
                  </template>
                </tr>
                <!-- 空态 / 加载态：降级与请求失败不是「无数据」（契约 webui-ws §5 第 13 条） -->
                <tr v-if="!positionsStore.loading && visiblePositionGroups.length === 0">
                  <td :colspan="positionColumnCount" class="dashboard-pos-empty" data-test="position-empty">
                    <template v-if="!positionsStore.available">交易数据暂不可用</template>
                    <template v-else-if="positionsStore.initialized">暂无持仓</template>
                    <template v-else>加载中...</template>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>

        <!-- Vertical Splitter 1 (Position / Bottom) -->
        <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 拖拽把手，键盘等效交互走后续票 -->
        <div class="splitter-v v-splitter-1" :style="{ height: '6px', cursor: 'row-resize', flexShrink: 0, margin: 0 }" @mousedown="onVSplitter1Down"></div>

        <!-- Bottom: Account + Orders -->
        <div ref="bottomPanelEl" class="dashboard-bottom-panel" :style="{ flex: `${bottomFlex} 1 0%`, minHeight: '100px', display: 'flex', flexDirection: 'row', overflow: 'hidden' }">

          <!-- Account Overview -->
          <div class="dashboard-panel dashboard-panel--account" :style="{ flex: `${accountFlex} 1 0%`, minWidth: '150px', minHeight: '100px', display: 'flex', flexDirection: 'column', overflow: 'hidden' }">
            <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--border-neutral-l1)', flexShrink: 0 }">
              <span :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)' }">
                <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">账户</span>
                <span class="ds-tag ds-tag--warning" data-test="dashboard-demo-tag" :style="{ fontSize: '10px' }">演示</span>
              </span>
              <span data-test="account-aggregate" :style="{ fontSize: 'var(--body-xs-font-size)', fontVariantNumeric: 'tabular-nums', color: 'var(--text-tertiary)' }" title="已登录账户/总账户">{{ accountAggregate }}</span>
            </div>
            <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto' }">
              <table class="ds-table ds-table--normal" :style="{ width: '100%', tableLayout: 'auto', whiteSpace: 'nowrap' }">
                <thead :style="{ position: 'sticky', top: 0, zIndex: 1 }">
                  <tr>
                    <th class="frozen frozen-last" :style="{ left: 0 }">账户</th>
                    <th>状态</th>
                    <th>动态权益</th>
                    <th>风险度</th>
                    <th>可用资金</th>
                    <th>占用保证金</th>
                    <th>出入金</th>
                    <th>净利润</th>
                    <th>手续费</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="acc in accounts" :key="acc.name">
                    <td class="frozen frozen-last cell-text" :style="{ left: 0 }">
                      <div class="cell-two-line"><span class="cell-main">{{ acc.name }}</span><span class="cell-sub">{{ acc.sub }}</span></div>
                    </td>
                    <td v-if="acc.status === 'logged-in'" :style="{ textAlign: 'center' }">
                      <span class="ds-tag ds-tag--success">已登录</span>
                    </td>
                    <td v-else-if="acc.status === 'logged-out'" :style="{ textAlign: 'center' }">
                      <span class="ds-tag ds-tag--danger">未登录</span>
                    </td>
                    <td v-else>
                      <StatusIndicator :current="0.5" :max="1" :min="0" :mini="true" idle-text="未登录" :loading-text="acc.loadingText" done-text="已登录" />
                    </td>
                    <td class="cell-num" :style="{ color: acc.equity === '--' ? 'var(--text-tertiary)' : 'inherit' }">{{ acc.equity }}</td>
                    <td class="cell-num" :style="{ color: acc.riskColor }">{{ acc.risk }}</td>
                    <td class="cell-num" :style="{ color: acc.available === '--' ? 'var(--text-tertiary)' : 'inherit' }">{{ acc.available }}</td>
                    <td class="cell-num" :style="{ color: acc.margin === '--' ? 'var(--text-tertiary)' : 'inherit' }">{{ acc.margin }}</td>
                    <td class="cell-num" :style="{ color: acc.deposit === '--' ? 'var(--text-tertiary)' : '#2a9d5c' }">{{ acc.deposit }}</td>
                    <td class="cell-num" :style="{ color: acc.profitColor }">{{ acc.profit }}</td>
                    <td class="cell-num" :style="{ color: acc.fee === '--' ? 'var(--text-tertiary)' : 'inherit' }">{{ acc.fee }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>

          <!-- Horizontal Splitter (Account / Orders) -->
          <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 拖拽把手，键盘等效交互走后续票 -->
          <div class="splitter-h h-splitter" :style="{ width: '6px', cursor: 'col-resize', flexShrink: 0, margin: 0 }" @mousedown="onHSplitterDown"></div>

          <!-- Orders -->
          <div class="dashboard-panel dashboard-panel--orders" :style="{ flex: `${orderFlex} 1 0%`, minWidth: '150px', minHeight: '100px', display: 'flex', flexDirection: 'column', overflow: 'hidden' }">
            <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--border-neutral-l1)', flexShrink: 0 }">
              <span :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)' }">
                <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">委托</span>
                <span class="ds-tag ds-tag--warning" data-test="dashboard-demo-tag" :style="{ fontSize: '10px' }">演示</span>
              </span>
              <div :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-12)', fontSize: 'var(--body-xs-font-size)' }">
                <label class="ds-check" :style="{ minHeight: 'auto', fontSize: 'var(--body-xs-font-size)', color: 'var(--text-secondary)' }">
                  <input type="checkbox"><span class="ds-check__box"></span> 仅挂单
                </label>
                <span data-test="orders-aggregate" :style="{ fontVariantNumeric: 'tabular-nums', color: 'var(--text-tertiary)', cursor: 'pointer', userSelect: 'none' }" title="点击切换 笔数/手数">{{ ordersAggregate }}</span>
              </div>
            </div>
            <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto' }">
              <table class="ds-table ds-table--dense" :style="{ width: '100%', tableLayout: 'auto', whiteSpace: 'nowrap' }">
                <thead :style="{ position: 'sticky', top: 0, zIndex: 1 }">
                  <tr>
                    <th class="frozen frozen-last" :style="{ left: 0 }">合约</th>
                    <th>来源</th>
                    <th>账户</th>
                    <th>买卖</th>
                    <th>已成/总</th>
                    <th>价格</th>
                    <th>均价</th>
                    <th>时间</th>
                    <th>状态</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="(ord, idx) in orders" :key="idx" :class="ord.rowClass">
                    <td class="frozen frozen-last cell-text" :style="{ left: 0, fontWeight: 'var(--font-weight-medium)' }">
                      <span class="order-status-icon" :class="`order-status-icon--${ord.icon}`">{{ ord.icon === 'filled' ? '✓' : ord.icon === 'cancelled' ? '✗' : ord.icon === 'rejected' ? '✗' : '◷' }}</span>
                      {{ ord.contract }}
                    </td>
                    <td class="cell-text" :style="{ color: 'var(--text-secondary)' }">{{ ord.source }}</td>
                    <td class="cell-text" :style="{ color: 'var(--text-secondary)' }">{{ ord.account }}</td>
                    <td class="cell-text" :class="ord.direction === 'buy' ? 'trade-buy' : 'trade-sell'">{{ ord.direction === 'buy' ? '买入' : '卖出' }}</td>
                    <td class="cell-num">{{ ord.filled }}</td>
                    <td class="cell-num">{{ ord.price }}</td>
                    <td class="cell-num" :style="{ color: ord.avgPrice === '--' ? 'var(--text-tertiary)' : 'inherit' }">{{ ord.avgPrice }}</td>
                    <td class="cell-num">{{ ord.time }}</td>
                    <td class="cell-text">
                      <span v-if="ord.tagClass" class="ds-tag" :class="ord.tagClass" :style="{ fontSize: '10px' }">{{ ord.tagText }}</span>
                      <span v-else class="ds-tag" :style="{ fontSize: '10px', color: 'var(--text-tertiary)' }">{{ ord.tagText }}</span>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </div>

      <!-- Vertical Splitter 2 (Left / Right) -->
      <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 拖拽把手，键盘等效交互走后续票 -->
      <div class="splitter-v v-splitter-main" :style="{ width: '6px', cursor: 'col-resize', flexShrink: 0 }" @mousedown="onVSplitter2Down"></div>

      <!-- Right Panel -->
      <div ref="rightPanelEl" class="dashboard-right-panel" :style="{ display: 'flex', flexDirection: 'column', overflow: 'hidden', minWidth: 0 }">

        <!-- Market Sources -->
        <div class="dashboard-panel" :style="{ flex: `${marketFlex} 1 0%`, minHeight: '80px', overflow: 'hidden', display: 'flex', flexDirection: 'column' }">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--bg-base-secondary)', flexShrink: 0 }">
            <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">行情</span>
            <span :style="{ fontSize: 'var(--body-xs-font-size)', fontVariantNumeric: 'tabular-nums', color: 'var(--text-tertiary)' }" title="已登录行情源/总行情源">{{ marketOnlineCount }}/{{ marketTotalCount }}</span>
          </div>
          <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto', display: 'flex', flexDirection: 'column' }">
            <div
              v-for="(src, idx) in marketSourcesStore.sources" :key="src.source_name"
              :style="{
                display: 'flex', alignItems: 'center', justifyContent: 'space-between',
                padding: 'var(--spacer-8) var(--spacer-16)',
                fontSize: 'var(--body-xs-font-size)', whiteSpace: 'nowrap',
                borderTop: idx > 0 ? '1px solid var(--bg-base-secondary)' : 'none',
              }"
            >
              <div :style="{ display: 'flex', flexDirection: 'column', gap: '1px', minWidth: 0, overflow: 'hidden' }">
                <div :style="{ fontWeight: 'var(--font-weight-medium)', color: 'var(--text-secondary)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }">{{ src.display_name }}</div>
                <div :style="{ fontSize: '10px', color: 'var(--text-tertiary)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }">
                  {{ src.source_name }} · 订阅 {{ src.subscribeCount }}/{{ src.subscribeTotal }}
                  <span
                    v-if="marketProcessStateText(src.process_state)"
                    :style="{ color: processStateColor(src.process_state), marginLeft: '4px' }"
                  >· {{ marketProcessStateText(src.process_state) }}</span>
                </div>
              </div>
              <StatusIndicator :current="src.progressView.current" :max="src.progressView.max" :min="src.progressView.min" :desc="src.progressView.desc" :mini="true" idle-text="未登录" loading-text="登录中" done-text="已登录" />
            </div>
          </div>
        </div>

        <!-- Splitter (Market / Strategy) -->
        <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 拖拽把手，键盘等效交互走后续票 -->
        <div class="splitter-h market-strategy-splitter" :style="{ height: '6px', cursor: 'row-resize', flexShrink: 0, margin: 0 }" @mousedown="startMarketDrag"></div>

        <!-- Strategy List -->
        <div class="dashboard-panel" :style="{ flex: `${strategyFlex} 1 0%`, minHeight: '80px', overflow: 'hidden', display: 'flex', flexDirection: 'column' }">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--bg-base-secondary)', flexShrink: 0 }">
            <span :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)', minWidth: 0 }">
              <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">策略</span>
              <span class="ds-tag ds-tag--warning" data-test="dashboard-demo-tag" :style="{ fontSize: '10px' }">演示</span>
            </span>
            <span data-test="strategy-aggregate" :style="{ fontSize: 'var(--body-xs-font-size)', fontVariantNumeric: 'tabular-nums', color: 'var(--text-tertiary)' }" title="正常运行策略/总策略数">{{ strategyAggregate }}</span>
          </div>
          <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto', fontSize: 'var(--body-xs-font-size)', fontVariantNumeric: 'tabular-nums' }">
            <div
              v-for="(strat, idx) in strategyList" :key="strat.id"
              :style="{
                display: 'flex', alignItems: 'center', justifyContent: 'space-between',
                padding: 'var(--spacer-8) var(--spacer-16)',
                borderBottom: idx < strategyList.length - 1 ? '1px solid var(--bg-base-secondary)' : 'none',
              }"
            >
              <div :style="{ display: 'flex', flexDirection: 'column', gap: '1px', minWidth: 0, overflow: 'hidden' }">
                <div :style="{ fontWeight: 'var(--font-weight-medium)', color: 'var(--text-secondary)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }">{{ strat.name }}</div>
                <div :style="{ fontSize: '10px', color: 'var(--text-tertiary)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }">{{ strat.id }} · {{ strat.lang }}</div>
              </div>
              <span class="ds-tag" :class="strat.running ? 'ds-tag--success' : 'ds-tag--danger'" :style="{ fontSize: '10px', flexShrink: 0 }">{{ strat.running ? '运行中' : '已停止' }}</span>
            </div>
            <div :style="{ padding: 'var(--spacer-8) var(--spacer-16)', fontSize: 'var(--body-xs-font-size)', color: 'var(--text-tertiary)', borderTop: '1px solid var(--bg-base-secondary)' }" data-test="strategy-footer">{{ strategyFooter }}</div>
          </div>
        </div>

        <!-- Splitter (Strategy / Messages) -->
        <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 拖拽把手，键盘等效交互走后续票 -->
        <div class="splitter-h strategy-msg-splitter" :style="{ height: '6px', cursor: 'row-resize', flexShrink: 0, margin: 0 }" @mousedown="startStrategyMsgDrag"></div>

        <!-- Messages -->
        <div class="dashboard-panel" :style="{ flex: `${msgFlex} 1 0%`, minHeight: '80px', overflow: 'hidden', display: 'flex', flexDirection: 'column' }">
          <div :style="{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: 'var(--spacer-8) var(--spacer-16)', background: 'var(--bg-card)', borderBottom: '1px solid var(--bg-base-secondary)', flexShrink: 0 }">
            <span :style="{ display: 'flex', alignItems: 'center', gap: 'var(--spacer-6)' }">
              <span :style="{ fontSize: 'var(--body-sm-font-size)', fontWeight: 'var(--font-weight-strong)', color: 'var(--text-default)' }">消息</span>
              <span class="ds-tag ds-tag--warning" data-test="dashboard-demo-tag" :style="{ fontSize: '10px' }">演示</span>
            </span>
            <button type="button" :style="{ border: 'none', background: 'transparent', cursor: 'pointer', color: 'var(--text-tertiary)', padding: '0 4px', fontSize: 'var(--body-xs-font-size)', lineHeight: '20px' }" title="切换时间显示">n分钟前</button>
          </div>
          <div class="auto-scroll" :style="{ flex: 1, overflow: 'auto', fontSize: 'var(--body-xs-font-size)' }">
            <div
              v-for="(msg, idx) in messages" :key="idx"
              :style="{
                padding: 'var(--spacer-8) var(--spacer-16)',
                borderBottom: idx < messages.length - 1 ? '1px solid var(--bg-base-secondary)' : 'none',
                display: 'flex', alignItems: 'flex-start', gap: 'var(--spacer-10)',
              }"
            >
              <span class="msg-level-icon" :class="`msg-level-icon--${msg.level}`"></span>
              <div :style="{ minWidth: 0, flex: 1 }">
                <div :style="{ color: 'var(--text-secondary)' }">{{ msg.text }}</div>
                <div :style="{ color: 'var(--text-tertiary)', fontSize: '10px' }">{{ msg.time }} · {{ msg.source }}</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
</template>
