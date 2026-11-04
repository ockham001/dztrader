<script setup lang="ts">
/**
 * 委托 tab（#246；契约 rest §2.9、契约 webui-ws §2.5/§5，ADR 0043 决策 7）。
 *
 * 展示职责全部在 `stores/tdOrders` 与 `composables/tdOrderView`（合并/精度/排序都是纯函数或
 * store 状态），本组件只做三件事：虚拟滚动渲染窗口、视口位置（顶部判定 + 底部预取）、
 * 筛选面板与「N 条新数据」气泡。
 *
 * 虚拟滚动：`RecycleScroller` 只渲染可视行（先例 LogViewerTab.vue），有界窗口 2000 行由 store 保证。
 * 精度：一律 `toFixed(price_decimals / avg_price_decimals)`，`avg_price === null` 显示占位破折号
 * （契约 webui-ws §5 第 12 条，禁止自行推断小数位）。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { RecycleScroller } from 'vue-virtual-scroller'
import type { RecycleScrollerExposed } from 'vue-virtual-scroller'
import 'vue-virtual-scroller/dist/vue-virtual-scroller.css'
import Icon from '@/components/shared/Icon.vue'
import { useTdOrdersStore } from '@/stores/tdOrders'
import {
  formatTdAvgPrice,
  formatTdPrice,
  formatTdTime,
  tdDirectionText,
  tdOrderStatusText,
  tdPositionEffectText,
} from '@/composables/tdOrderView'
import type { TdOrderRow } from '@/types/api'

const store = useTdOrdersStore()
const scroller = ref<RecycleScrollerExposed<TdOrderRow> | null>(null)

/** 行高固定 36px：RecycleScroller 据此算可视窗口（列多、单行不可换行） */
const ROW_HEIGHT = 36

/** 账户筛选面板开合（面板本身常驻 DOM 会挤压表格宽度，默认收起） */
const filterOpen = ref(false)

/** 筛选面板里的账户选项（在线集合 + 已选但已下线的账户不列：契约 webui-ws §5 第 11 条） */
const accountOptions = computed<string[]>(() => [...store.onlineAccounts].sort())

/** 按钮文案：空筛选 = 契约缺省语义「全部在线账户」，此时显示「全部」而不是 0/N */
const filterLabel = computed<string>(() => {
  const total = accountOptions.value.length
  if (total === 0) return '账户 0'
  if (store.allAccountsSelected || store.effectiveAccounts.length === 0) return `全部账户（${total}）`
  return `账户 ${store.effectiveAccounts.length}/${total}`
})

/** 账户是否被选中（未改过筛选时视为全选） */
function isAccountSelected(account: string): boolean {
  if (store.allAccountsSelected) return true
  return store.accountFilter.has(account)
}

/**
 * 切换单个账户（重置游标重拉由 store 负责）。
 *
 * 不允许取消最后一个账户：空列表在契约里的语义是「缺省 = 全部在线账户」（rest §2.9.2），
 * 用户取消到 0 个却看到全部数据会非常反直觉。要回到全部请用「全选」。
 */
function toggleAccount(account: string): void {
  const next = store.allAccountsSelected
    ? store.onlineAccounts.filter(item => item !== account)
    : store.onlineAccounts.filter(item => (
        item === account ? !store.accountFilter.has(item) : store.accountFilter.has(item)
      ))
  if (next.length === 0) return
  void store.setAccountFilter(next)
}

/**
 * 唯一生效账户时禁用取消——取消它会让筛选变成 0 个账户，而空筛选在契约里 = 全部在线账户
 * （rest §2.9.2），用户会因此看到比原来更多的数据。判定用「当前生效筛选只剩这一个」，
 * 覆盖两种来源：只剩一个在线账户（默认全选），或用户已筛到只剩它。
 */
function isLastSelectedAccount(account: string): boolean {
  return store.effectiveAccounts.length === 1 && store.effectiveAccounts[0] === account
}

/**
 * 全选 / 取消全选。取消全选在 UI 上没有意义（空筛选 = 全部在线账户），
 * 因此按钮只在「有未选中账户」时出现，点击 = 恢复全选（不传 accounts）。
 */
function selectAll(): void {
  void store.selectAllAccounts()
}

/** 状态标签样式（与设计系统状态色一致：成交/撤单/拒绝各有语义） */
function statusTagClass(status: TdOrderRow['status']): string {
  switch (status) {
    case 'AllTraded': return 'ds-tag ds-tag--success'
    case 'PartTraded': return 'ds-tag ds-tag--brand'
    case 'Rejected': return 'ds-tag ds-tag--danger'
    case 'Cancelled': return 'ds-tag'
    default: return 'ds-tag'
  }
}

/**
 * 方向单元格的类名（列宽类必须一并给上，否则该列不参与 flex 定宽、后面所有列都会错位）。
 * 买=成功色、卖=错误色，净/未知用中性色。
 */
function directionClass(direction: TdOrderRow['direction']): string {
  const base = 'td-order__col td-order__col--direction td-order__direction'
  if (direction === 'Long') return `${base} td-order__direction--long`
  if (direction === 'Short') return `${base} td-order__direction--short`
  return base
}

/**
 * 视口滚动（RecycleScroller 的 update 事件）：更新「是否在顶部」并触发预取。
 *
 * 顶部判定用**首个可见行索引**（visibleStartIndex === 0 才是真的在顶部），
 * 不能用 endIndex——首屏只有 10 行时 endIndex 也是 9，会把"用户在顶部"误判成"不在顶部"。
 */
function handleUpdate(
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
function jumpToTop(): void {
  store.setAtTop(true)
  store.clearNewCount()
  scroller.value?.scrollToItem(0)
}

/** 手动刷新：丢弃游标从首页重拉（与日切/data_changed 同一路径） */
function refresh(): void {
  void store.resetAndReload()
}

/** 表格列宽：委托号/策略固定，其余按内容分配（表头与行共用同一组类） */
const COLUMNS: { key: string; label: string; className: string }[] = [
  { key: 'time', label: '时间', className: 'td-order__col td-order__col--time' },
  { key: 'account', label: '账户', className: 'td-order__col td-order__col--account' },
  { key: 'instrument', label: '合约', className: 'td-order__col td-order__col--instrument' },
  { key: 'direction', label: '方向', className: 'td-order__col td-order__col--direction' },
  { key: 'effect', label: '开平', className: 'td-order__col td-order__col--effect' },
  { key: 'price', label: '价格', className: 'td-order__col td-order__col--price' },
  { key: 'volume', label: '数量', className: 'td-order__col td-order__col--volume' },
  { key: 'traded', label: '已成交', className: 'td-order__col td-order__col--volume' },
  { key: 'avg', label: '成交均价', className: 'td-order__col td-order__col--price' },
  { key: 'status', label: '状态', className: 'td-order__col td-order__col--status' },
  { key: 'orderId', label: '委托号', className: 'td-order__col td-order__col--order-id' },
  { key: 'strategy', label: '策略', className: 'td-order__col td-order__col--strategy' },
]

onMounted(() => {
  void store.init()
})

onUnmounted(() => {
  // 离开页面即停定时器：批量刷新与降级重试都不应在无视图时空转
  store.dispose()
})

// 筛选面板在无在线账户时没有内容可展示，自动收起（避免留一个空浮层）
watch(accountOptions, options => {
  if (options.length === 0) filterOpen.value = false
})
</script>

<template>
  <div class="td-orders">
    <div class="td-orders__toolbar">
      <div class="td-orders__toolbar-left">
        <span class="td-orders__meta">
          <template v-if="store.tradingDay">交易日 {{ store.tradingDay }}</template>
          <template v-else>无在线交易账户</template>
          <template v-if="store.rows.length > 0"> · 显示 {{ store.rows.length }} 条</template>
        </span>
        <span v-if="store.loading" class="td-orders__loading">加载中...</span>
      </div>
      <div class="td-orders__toolbar-right">
        <div class="td-orders__filter">
          <button
            class="ds-btn ds-btn--secondary ds-btn--sm"
            type="button"
            :aria-expanded="filterOpen"
            aria-controls="td-order-account-filter"
            @click="filterOpen = !filterOpen"
          >{{ filterLabel }}</button>
          <div v-if="filterOpen" id="td-order-account-filter" class="td-orders__filter-panel">
            <div class="td-orders__filter-head">
              <span class="td-orders__filter-title">账户筛选</span>
              <button
                v-if="!store.allAccountsSelected"
                class="ds-btn ds-btn--tertiary ds-btn--sm"
                type="button"
                @click="selectAll"
              >全选</button>
            </div>
            <p v-if="accountOptions.length === 0" class="td-orders__filter-empty">暂无在线账户</p>
            <div v-else data-test="td-account-filter" class="td-orders__filter-list">
              <label v-for="account in accountOptions" :key="account" class="ds-check">
                <input
                  type="checkbox"
                  :checked="isAccountSelected(account)"
                  :disabled="isLastSelectedAccount(account)"
                  :aria-label="`筛选账户 ${account}`"
                  :title="isLastSelectedAccount(account) ? '至少保留一个账户；要显示全部请用「全选」' : ''"
                  @change="toggleAccount(account)"
                >
                <span class="ds-check__box"></span>
                <span class="td-orders__filter-label">{{ account }}</span>
              </label>
            </div>
          </div>
        </div>
        <button
          class="ds-btn ds-btn--secondary ds-btn--sm"
          type="button"
          :disabled="store.loading"
          @click="refresh"
        >刷新</button>
      </div>
    </div>

    <!-- 降级提示条（契约 webui-ws §5 第 13 条：统一提示 + 自动退避重试，不用 0 冒充数据） -->
    <div
      v-if="!store.available"
      class="td-orders__degraded"
      role="status"
      data-test="td-degraded-banner"
    >
      <Icon name="Tips" :size="14" />
      <span>{{ store.degradedText }}</span>
    </div>

    <div v-if="store.error" class="td-orders__error" role="alert">{{ store.error }}</div>

    <div class="td-orders__table">
      <div class="td-orders__header">
        <span
          v-for="column in COLUMNS"
          :key="column.key"
          :class="column.className"
        >{{ column.label }}</span>
      </div>

      <div class="td-orders__body">
        <div v-if="store.loading && store.rows.length === 0" class="td-orders__empty">
          <p class="ds-empty__desc">加载中...</p>
        </div>
        <div v-else-if="store.isEmpty" class="td-orders__empty" data-test="td-orders-empty">
          <div class="ds-empty__icon"><Icon name="Bar-list" :size="20" /></div>
          <p class="ds-empty__title">
            {{ store.onlineAccounts.length === 0 ? '无在线交易账户' : '当日暂无委托' }}
          </p>
          <p class="ds-empty__desc">
            {{ store.onlineAccounts.length === 0
              ? '交易账户登录后，委托记录将在此显示'
              : '当前交易日还没有委托记录' }}
          </p>
        </div>
        <RecycleScroller
          v-else-if="store.rows.length > 0"
          ref="scroller"
          class="td-orders__scroller"
          :items="store.rows"
          :item-size="ROW_HEIGHT"
          key-field="order_id"
          emit-update
          @update="handleUpdate"
        >
          <template #default="{ item }">
            <div class="td-order" :data-order-id="item.order_id">
              <span class="td-order__col td-order__col--time">{{ formatTdTime(item.time) }}</span>
              <span class="td-order__col td-order__col--account" :title="item.account_id">{{ item.account_id }}</span>
              <span class="td-order__col td-order__col--instrument" :title="`${item.instrument_id} · ${item.exchange_id}`">{{ item.instrument_id }}</span>
              <span :class="directionClass(item.direction)">{{ tdDirectionText(item.direction) }}</span>
              <span class="td-order__col td-order__col--effect">{{ tdPositionEffectText(item.position_effect) }}</span>
              <span class="td-order__col td-order__col--price">{{ formatTdPrice(item.price, item.price_decimals) }}</span>
              <span class="td-order__col td-order__col--volume">{{ item.volume }}</span>
              <span class="td-order__col td-order__col--volume">{{ item.volume_traded }}</span>
              <span class="td-order__col td-order__col--price">{{ formatTdAvgPrice(item.avg_price, item.avg_price_decimals) }}</span>
              <span class="td-order__col td-order__col--status">
                <span :class="statusTagClass(item.status)">{{ tdOrderStatusText(item.status) }}</span>
              </span>
              <span class="td-order__col td-order__col--order-id">{{ item.order_id }}</span>
              <span class="td-order__col td-order__col--strategy" :title="item.strategy_id">{{ item.strategy_id }}</span>
            </div>
          </template>
        </RecycleScroller>
      </div>
    </div>

    <!-- 「N 条新数据」气泡（ADR 0043 决策 7）：不在顶部时新行只计数，点击回顶部 -->
    <button
      v-if="store.newCount > 0"
      class="td-orders__bubble"
      type="button"
      data-test="td-new-rows-bubble"
      @click="jumpToTop"
    >{{ store.newCount }} 条新数据</button>
  </div>
</template>

<style scoped>
.td-orders {
  position: relative;
  display: flex;
  flex-direction: column;
  min-height: 0;
  height: 100%;
  gap: var(--spacer-8);
}

.td-orders__toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  flex-wrap: wrap;
}

.td-orders__toolbar-left,
.td-orders__toolbar-right {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-orders__meta {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  font-variant-numeric: tabular-nums;
}

.td-orders__loading {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-orders__filter {
  position: relative;
}

.td-orders__filter-panel {
  position: absolute;
  top: calc(100% + var(--spacer-4));
  right: 0;
  z-index: 20;
  min-width: 200px;
  max-height: 280px;
  overflow-y: auto;
  padding: var(--spacer-8);
  background: var(--bg-base-default);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  box-shadow: var(--shadow-lg);
}

.td-orders__filter-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  margin-bottom: var(--spacer-4);
}

.td-orders__filter-title {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-orders__filter-list {
  display: flex;
  flex-direction: column;
}

.td-orders__filter-label {
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.td-orders__filter-empty {
  margin: 0;
  padding: var(--spacer-8) 0;
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

.td-orders__degraded {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--status-warning-default);
  border-radius: var(--radius-8);
  background: var(--status-warning-surface-l1);
  color: var(--status-warning-default);
  font-size: var(--body-sm-font-size);
}

.td-orders__error {
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--status-error-default);
  border-radius: var(--radius-8);
  background: var(--status-error-surface-l1);
  color: var(--status-error-default);
  font-size: var(--body-sm-font-size);
}

.td-orders__table {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  overflow: hidden;
}

.td-orders__header,
.td-order {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  padding: 0 var(--spacer-12);
  white-space: nowrap;
}

.td-orders__header {
  height: 32px;
  flex-shrink: 0;
  border-bottom: 1px solid var(--border-neutral-l1);
  background: var(--bg-base-secondary);
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
  font-weight: var(--font-weight-medium);
}

.td-orders__body {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.td-orders__scroller {
  height: 100%;
}

.td-order {
  height: 36px;
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  font-variant-numeric: tabular-nums;
}

.td-order:hover {
  background: var(--bg-overlay-l1);
}

.td-order__col {
  overflow: hidden;
  text-overflow: ellipsis;
}

.td-order__col--time { flex: 0 0 72px; color: var(--text-secondary); }
.td-order__col--account { flex: 0 0 110px; }
.td-order__col--instrument { flex: 0 0 100px; }
.td-order__col--direction { flex: 0 0 48px; }
.td-order__col--effect { flex: 0 0 56px; }
.td-order__col--price { flex: 0 0 96px; text-align: right; }
.td-order__col--volume { flex: 0 0 64px; text-align: right; }
.td-order__col--status { flex: 0 0 96px; }
.td-order__col--order-id { flex: 0 0 88px; color: var(--text-secondary); }
.td-order__col--strategy { flex: 1 1 120px; color: var(--text-secondary); min-width: 80px; }

.td-order__direction--long { color: var(--status-success-default); }
.td-order__direction--short { color: var(--status-error-default); }

.td-orders__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  gap: var(--spacer-4);
  text-align: center;
  color: var(--text-tertiary);
}

.td-orders__bubble {
  position: absolute;
  left: 50%;
  bottom: var(--spacer-16);
  transform: translateX(-50%);
  z-index: 10;
  padding: var(--spacer-6) var(--spacer-16);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-full);
  background: var(--bg-base-default);
  color: var(--text-brand);
  font-size: var(--body-sm-font-size);
  cursor: pointer;
  box-shadow: var(--shadow-sm);
}

.td-orders__bubble:hover {
  background: var(--bg-overlay-l1);
}
</style>
