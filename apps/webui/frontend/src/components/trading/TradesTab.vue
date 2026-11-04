<script setup lang="ts">
/**
 * 成交 tab 占位（#246 范围外：成交列表与均价实时累计归 #248）。
 *
 * 明确写出「后续版本提供」，避免用户把空态误解为「当天没有成交」——成交数据本轮虽已落库，
 * 但 REST/WS 出口与列表渲染不在本票范围（design §0）。
 */
import Icon from '@/components/shared/Icon.vue'
</script>

<template>
  <div class="td-trades" data-test="td-trades-placeholder">
    <div class="ds-empty__icon"><Icon name="Bar-list" :size="20" /></div>
    <p class="ds-empty__title">成交列表将在后续版本提供</p>
    <p class="ds-empty__desc">成交数据已随委托同步落库，列表展示与均价统计将在后续版本开放。</p>
  </div>
</template>

<style scoped>
.td-trades {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  min-height: 320px;
  gap: var(--spacer-4);
  text-align: center;
  color: var(--text-tertiary);
}
</style>
