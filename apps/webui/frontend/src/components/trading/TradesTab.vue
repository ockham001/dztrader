<script setup lang="ts">
/**
 * 成交 tab（#248；契约 rest §2.9、契约 webui-ws §2.5/§5，ADR 0043 决策 7）。
 *
 * 展示职责全部在 `stores/tdTrades` 与 `composables/tdTradeView`（合并/精度/排序都是纯函数或
 * store 状态）；交互逻辑（筛选/视口/预取/气泡）与委托 tab 共用
 * `composables/tdListTab`——两份 tab 只有「哪一份 store、哪几列」的差别，不为成交另写一套。
 *
 * 虚拟滚动：`RecycleScroller` 只渲染可视行（先例 OrdersTab.vue），有界窗口 2000 行由 store 保证。
 * 精度：一律 `toFixed(price_decimals)`（契约 webui-ws §5 第 12 条，禁止自行推断小数位）。
 * 行 key：成交业务键 `account|date|exchange|trade_id`（`trade_id` 仅交易所内唯一，不能用它单键）。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { RecycleScroller } from 'vue-virtual-scroller'
import type { RecycleScrollerExposed } from 'vue-virtual-scroller'
import 'vue-virtual-scroller/dist/vue-virtual-scroller.css'
import Icon from '@/components/shared/Icon.vue'
import { useTdTradesStore } from '@/stores/tdTrades'
import {
  formatTdPrice,
  formatTdTime,
  tdDirectionText,
  tdPositionEffectText,
} from '@/composables/tdOrderView'
import { tdTradeBusinessKey } from '@/composables/tdTradeView'
import {
  handleListUpdate,
  isTdListAccountSelected,
  isTdListLastSelectedAccount,
  jumpListToTop,
  refreshTdList,
  tdListAccountOptions,
  tdListFilterLabel,
  toggleTdListAccount,
} from '@/composables/tdListTab'
import type { TdListColumn } from '@/composables/tdListTab'
import type { TdTradeRow } from '@/types/api'

const store = useTdTradesStore()
const scroller = ref<RecycleScrollerExposed<TdTradeRow> | null>(null)

/** 行高固定 36px：RecycleScroller 据此算可视窗口（列多、单行不可换行） */
const ROW_HEIGHT = 36

/** 账户筛选面板开合（面板本身常驻 DOM 会挤压表格宽度，默认收起） */
const filterOpen = ref(false)

/** 筛选面板里的账户选项（与委托 tab 同一口径：在线集合排序） */
const accountOptions = computed<string[]>(() => tdListAccountOptions(store))

/** 按钮文案（空筛选 = 契约缺省语义「全部在线账户」） */
const filterLabel = computed<string>(() => tdListFilterLabel(store))

function isAccountSelected(account: string): boolean {
  return isTdListAccountSelected(store, account)
}

function isLastSelectedAccount(account: string): boolean {
  return isTdListLastSelectedAccount(store, account)
}

function toggleAccount(account: string): void {
  toggleTdListAccount(store, account)
}

function selectAll(): void {
  void store.selectAllAccounts()
}

/**
 * 方向单元格的类名（列宽类必须一并给上，否则该列不参与 flex 定宽、后面所有列都会错位）。
 * 买=成功色、卖=错误色，净/未知用中性色。
 */
function directionClass(direction: TdTradeRow['direction']): string {
  const base = 'td-trade__col td-trade__col--direction td-trade__direction'
  if (direction === 'Long') return `${base} td-trade__direction--long`
  if (direction === 'Short') return `${base} td-trade__direction--short`
  return base
}

function handleUpdate(
  startIndex: number,
  endIndex: number,
  visibleStartIndex: number,
  visibleEndIndex: number,
): void {
  handleListUpdate(store, startIndex, endIndex, visibleStartIndex, visibleEndIndex)
}

function jumpToTop(): void {
  jumpListToTop(store, () => scroller.value?.scrollToItem(0))
}

/** 手动刷新：丢弃游标从首页重拉（与日切/data_changed 同一路径） */
function refresh(): void {
  refreshTdList(store)
}

/** 行 key = 成交业务键（`trade_id` 仅交易所内唯一，必须与账户/日期/交易所组合） */
function tradeKey(row: TdTradeRow): string {
  return tdTradeBusinessKey(row)
}

/** 表格列宽：委托号/策略固定，其余按内容分配（表头与行共用同一组类） */
const COLUMNS: TdListColumn[] = [
  { key: 'time', label: '时间', className: 'td-trade__col td-trade__col--time' },
  { key: 'account', label: '账户', className: 'td-trade__col td-trade__col--account' },
  { key: 'instrument', label: '合约', className: 'td-trade__col td-trade__col--instrument' },
  { key: 'direction', label: '方向', className: 'td-trade__col td-trade__col--direction' },
  { key: 'effect', label: '开平', className: 'td-trade__col td-trade__col--effect' },
  { key: 'price', label: '成交价', className: 'td-trade__col td-trade__col--price' },
  { key: 'volume', label: '成交量', className: 'td-trade__col td-trade__col--volume' },
  { key: 'orderId', label: '委托号', className: 'td-trade__col td-trade__col--order-id' },
  { key: 'strategy', label: '策略', className: 'td-trade__col td-trade__col--strategy' },
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
  <div class="td-trades">
    <div class="td-trades__toolbar">
      <div class="td-trades__toolbar-left">
        <span class="td-trades__meta">
          <template v-if="store.tradingDay">交易日 {{ store.tradingDay }}</template>
          <template v-else>无在线交易账户</template>
          <template v-if="store.rows.length > 0"> · 显示 {{ store.rows.length }} 条</template>
        </span>
        <span v-if="store.loading" class="td-trades__loading">加载中...</span>
      </div>
      <div class="td-trades__toolbar-right">
        <div class="td-trades__filter">
          <button
            class="ds-btn ds-btn--secondary ds-btn--sm"
            type="button"
            :aria-expanded="filterOpen"
            aria-controls="td-trade-account-filter"
            @click="filterOpen = !filterOpen"
          >{{ filterLabel }}</button>
          <div v-if="filterOpen" id="td-trade-account-filter" class="td-trades__filter-panel">
            <div class="td-trades__filter-head">
              <span class="td-trades__filter-title">账户筛选</span>
              <button
                v-if="!store.allAccountsSelected"
                class="ds-btn ds-btn--tertiary ds-btn--sm"
                type="button"
                @click="selectAll"
              >全选</button>
            </div>
            <p v-if="accountOptions.length === 0" class="td-trades__filter-empty">暂无在线账户</p>
            <div v-else data-test="td-trade-account-filter" class="td-trades__filter-list">
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
                <span class="td-trades__filter-label">{{ account }}</span>
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
      class="td-trades__degraded"
      role="status"
      data-test="td-degraded-banner"
    >
      <Icon name="Tips" :size="14" />
      <span>{{ store.degradedText }}</span>
    </div>

    <div v-if="store.error" class="td-trades__error" role="alert">{{ store.error }}</div>

    <div class="td-trades__table">
      <div class="td-trades__header" data-test="td-trades-header">
        <span
          v-for="column in COLUMNS"
          :key="column.key"
          :class="column.className"
        >{{ column.label }}</span>
      </div>

      <div class="td-trades__body">
        <div v-if="store.loading && store.rows.length === 0" class="td-trades__empty">
          <p class="ds-empty__desc">加载中...</p>
        </div>
        <div v-else-if="store.isEmpty" class="td-trades__empty" data-test="td-trades-empty">
          <div class="ds-empty__icon"><Icon name="Bar-list" :size="20" /></div>
          <p class="ds-empty__title">
            {{ store.onlineAccounts.length === 0 ? '无在线交易账户' : '当日暂无成交' }}
          </p>
          <p class="ds-empty__desc">
            {{ store.onlineAccounts.length === 0
              ? '交易账户登录后，成交记录将在此显示'
              : '当前交易日还没有成交记录' }}
          </p>
        </div>
        <RecycleScroller
          v-else-if="store.rows.length > 0"
          ref="scroller"
          class="td-trades__scroller"
          :items="store.rows"
          :item-size="ROW_HEIGHT"
          :key-field="tradeKey"
          emit-update
          @update="handleUpdate"
        >
          <template #default="{ item }">
            <div class="td-trade" :data-trade-id="item.trade_id">
              <span class="td-trade__col td-trade__col--time">{{ formatTdTime(item.time) }}</span>
              <span class="td-trade__col td-trade__col--account" :title="item.account_id">{{ item.account_id }}</span>
              <span class="td-trade__col td-trade__col--instrument" :title="`${item.instrument_id} · ${item.exchange_id}`">{{ item.instrument_id }}</span>
              <span :class="directionClass(item.direction)">{{ tdDirectionText(item.direction) }}</span>
              <span class="td-trade__col td-trade__col--effect">{{ tdPositionEffectText(item.position_effect) }}</span>
              <span class="td-trade__col td-trade__col--price">{{ formatTdPrice(item.price, item.price_decimals) }}</span>
              <span class="td-trade__col td-trade__col--volume">{{ item.volume }}</span>
              <span class="td-trade__col td-trade__col--order-id">{{ item.order_id }}</span>
              <span class="td-trade__col td-trade__col--strategy" :title="item.strategy_id">{{ item.strategy_id }}</span>
            </div>
          </template>
        </RecycleScroller>
      </div>
    </div>

    <!-- 「N 条新数据」气泡（ADR 0043 决策 7）：不在顶部时新行只计数，点击回顶部 -->
    <button
      v-if="store.newCount > 0"
      class="td-trades__bubble"
      type="button"
      data-test="td-new-rows-bubble"
      @click="jumpToTop"
    >{{ store.newCount }} 条新数据</button>
  </div>
</template>

<style scoped>
.td-trades {
  position: relative;
  display: flex;
  flex-direction: column;
  min-height: 0;
  height: 100%;
  gap: var(--spacer-8);
}

.td-trades__toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  flex-wrap: wrap;
}

.td-trades__toolbar-left,
.td-trades__toolbar-right {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.td-trades__meta {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  font-variant-numeric: tabular-nums;
}

.td-trades__loading {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-trades__filter {
  position: relative;
}

.td-trades__filter-panel {
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

.td-trades__filter-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  margin-bottom: var(--spacer-4);
}

.td-trades__filter-title {
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.td-trades__filter-list {
  display: flex;
  flex-direction: column;
}

.td-trades__filter-label {
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.td-trades__filter-empty {
  margin: 0;
  padding: var(--spacer-8) 0;
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

.td-trades__degraded {
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

.td-trades__error {
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--status-error-default);
  border-radius: var(--radius-8);
  background: var(--status-error-surface-l1);
  color: var(--status-error-default);
  font-size: var(--body-sm-font-size);
}

.td-trades__table {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  overflow: hidden;
}

.td-trades__header,
.td-trade {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  padding: 0 var(--spacer-12);
  white-space: nowrap;
}

.td-trades__header {
  height: 32px;
  flex-shrink: 0;
  border-bottom: 1px solid var(--border-neutral-l1);
  background: var(--bg-base-secondary);
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
  font-weight: var(--font-weight-medium);
}

.td-trades__body {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.td-trades__scroller {
  height: 100%;
}

.td-trade {
  height: 36px;
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  font-variant-numeric: tabular-nums;
}

.td-trade:hover {
  background: var(--bg-overlay-l1);
}

.td-trade__col {
  overflow: hidden;
  text-overflow: ellipsis;
}

.td-trade__col--time { flex: 0 0 72px; color: var(--text-secondary); }
.td-trade__col--account { flex: 0 0 110px; }
.td-trade__col--instrument { flex: 0 0 100px; }
.td-trade__col--direction { flex: 0 0 48px; }
.td-trade__col--effect { flex: 0 0 56px; }
.td-trade__col--price { flex: 0 0 96px; text-align: right; }
.td-trade__col--volume { flex: 0 0 64px; text-align: right; }
.td-trade__col--order-id { flex: 0 0 88px; color: var(--text-secondary); }
.td-trade__col--strategy { flex: 1 1 120px; color: var(--text-secondary); min-width: 80px; }

.td-trade__direction--long { color: var(--status-success-default); }
.td-trade__direction--short { color: var(--status-error-default); }

.td-trades__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  gap: var(--spacer-4);
  text-align: center;
  color: var(--text-tertiary);
}

.td-trades__bubble {
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

.td-trades__bubble:hover {
  background: var(--bg-overlay-l1);
}
</style>
