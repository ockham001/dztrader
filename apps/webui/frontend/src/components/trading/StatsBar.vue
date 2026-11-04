<script setup lang="ts">
/**
 * 交易数据 · 统计条（#247；契约 rest §2.9.5、契约 webui-ws §5 第 10 条）。
 *
 * 一条横条展示「未成交 / 总」，右侧是笔数/手数切换（纯展示，不发请求）。语义全部来自
 * `stores/tdStats`，组件只负责三态渲染：
 * - **降级**（`available === false`）：显示统一降级文案，**不显示 0**（计数器未就绪 ≠ 当日 0 委托）；
 * - **无在线账户**（`trading_day === null`）：显示「无在线交易账户」（与委托 tab 空态同义）；
 * - **未拿到值**（还没拉过）：显示占位 `—`。
 *
 * 挂载/卸载与 OrdersTab 同型：组件自行 `init()` / `dispose()`，不依赖父视图。
 */
import { onMounted, onUnmounted } from 'vue'
import { useTdStatsStore } from '@/stores/tdStats'

const store = useTdStatsStore()

/** 单位切换：笔数 ↔ 手数（只改口径，不触发请求） */
function toggleUnit(): void {
  store.setUnit(store.unit === 'orders' ? 'volume' : 'orders')
}

onMounted(() => {
  void store.init()
})

onUnmounted(() => {
  // 离开页面即停轮询（契约 webui-ws §5 第 10 条：统计按需轮询，无视图不该产生流量）
  store.dispose()
})
</script>

<template>
  <div class="td-stats-bar" data-test="td-stats-bar">
    <!-- 降级：只显示文案，绝不显示 0（契约 webui-ws §5 第 13 条） -->
    <div v-if="!store.available" class="td-stats-bar__degraded" role="status" data-test="td-stats-degraded">
      {{ store.degradedText }}
    </div>
    <template v-else>
      <!-- 未拿到值（还没拉过 / HTTP 失败保留旧值为空）：显示占位，不显示 0 -->
      <span v-if="store.values === null" class="td-stats-bar__empty" data-test="td-stats-pending">—</span>
      <span v-else-if="store.tradingDay === null" class="td-stats-bar__empty">无在线交易账户</span>
      <span v-else class="td-stats-bar__values" data-test="td-stats-values">
        未成交 <b class="td-stats-bar__num">{{ store.unfilledText }}</b>
        <span class="td-stats-bar__sep">/</span>
        总 <b class="td-stats-bar__num">{{ store.totalText }}</b>
        <span class="td-stats-bar__unit">{{ store.unitLabel }}</span>
      </span>
      <button
        class="ds-btn ds-btn--secondary ds-btn--sm td-stats-bar__toggle"
        type="button"
        data-test="td-stats-unit-toggle"
        @click="toggleUnit"
      >{{ store.unit === 'orders' ? '按手数' : '按笔数' }}</button>
    </template>
  </div>
</template>

<style scoped>
.td-stats-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  padding: var(--spacer-8) var(--spacer-12);
  background: var(--bg-overlay-l1);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
}

.td-stats-bar__values {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  font-variant-numeric: tabular-nums;
}

.td-stats-bar__num {
  color: var(--text-default);
  font-weight: var(--font-weight-strong);
}

.td-stats-bar__sep {
  margin: 0 var(--spacer-4);
  color: var(--text-tertiary);
}

.td-stats-bar__unit {
  margin-left: var(--spacer-2);
  color: var(--text-tertiary);
}

.td-stats-bar__empty,
.td-stats-bar__degraded {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
}

.td-stats-bar__degraded {
  color: var(--status-warning-default);
}

.td-stats-bar__toggle {
  flex-shrink: 0;
}
</style>
