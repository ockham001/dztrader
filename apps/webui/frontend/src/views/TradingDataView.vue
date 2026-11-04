<script setup lang="ts">
/**
 * 交易数据页（#246/#247）：页头 + 统计条 + 「委托」（默认）/「成交」两个 tab。
 *
 * 委托 tab 是 #246 交付内容（REST 基线 + WS 增量 + 虚拟滚动 + 有界窗口）；
 * 成交 tab 本轮为明确占位（列表归 #248，design §0）。两个 tab 用 v-show 而非 v-if：
 * 切到成交再切回时保留委托 tab 的滚动位置与 store 窗口，不重建订阅、不重拉基线。
 *
 * 统计条（#247，契约 rest §2.9.5）放在 tab 栏**上方**：统计是整页口径（不随 tab 切换），
 * 且与 tab 无关，放在 tab 上方才能一眼看到。StatsBar 自行 init/dispose（与 OrdersTab 同型）。
 */
import { ref } from 'vue'
import OrdersTab from '@/components/trading/OrdersTab.vue'
import StatsBar from '@/components/trading/StatsBar.vue'
import TradesTab from '@/components/trading/TradesTab.vue'

/** 当前 tab（委托默认，design §4.1） */
const tab = ref<'orders' | 'trades'>('orders')
</script>

<template>
  <div class="trading-data-view">
    <div class="page-header">
      <h1 class="trading-data-view__title">交易数据</h1>
    </div>

    <StatsBar class="trading-data-view__stats" />

    <div class="ds-tabs" :style="{ marginBottom: 'var(--spacer-16)' }">
      <button
        class="ds-tab"
        :class="{ 'is-active': tab === 'orders' }"
        type="button"
        data-test="td-tab-orders"
        @click="tab = 'orders'"
      >委托</button>
      <button
        class="ds-tab"
        :class="{ 'is-active': tab === 'trades' }"
        type="button"
        data-test="td-tab-trades"
        @click="tab = 'trades'"
      >成交</button>
    </div>

    <div class="trading-data-view__body">
      <div v-show="tab === 'orders'" class="trading-data-view__panel">
        <OrdersTab />
      </div>
      <div v-show="tab === 'trades'" class="trading-data-view__panel">
        <TradesTab />
      </div>
    </div>
  </div>
</template>

<style scoped>
.trading-data-view {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
  max-width: 1600px;
  width: 100%;
  margin: 0 auto;
  padding: var(--spacer-32) var(--spacer-24);
}

.page-header {
  margin-bottom: var(--spacer-24);
}

.trading-data-view__title {
  margin: 0;
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
}

/* 统计条与 tab 栏之间的间距（tab 栏自带 marginBottom 到面板） */
.trading-data-view__stats {
  margin-bottom: var(--spacer-16);
}

.trading-data-view__body {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.trading-data-view__panel {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
</style>
