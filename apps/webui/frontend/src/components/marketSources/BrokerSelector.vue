<script setup lang="ts">
import { computed } from 'vue'
import Dropdown, { type DropdownItem } from '@/components/shared/Dropdown.vue'
import { isStateIdle } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// BrokerSelector — 当前选中经纪商 Dropdown (F-C4: 受状态保护)
// 直接调 store (与现状 CtpCard 一致), props 仅传 source
const props = defineProps<{
  source: MarketSourceView
}>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// Broker dropdown items per source — value 为 broker.name (broker 由 name 标识)
// 契约 §6: 当前选中经纪商可以为空 (列表非空时也可为空, 不强制选第一个).
// 提供首项 "无选中" (value='') 让用户能主动切换到空选中状态.
function brokerItems(s: MarketSourceView): DropdownItem[] {
  return [
    { value: '', label: '无选中' },
    ...s.brokers.map(b => ({ value: b.name, label: b.name })),
  ]
}
</script>

<template>
  <!-- 当前选中经纪商 Dropdown — F-C4: 受状态保护 -->
  <div class="broker-select-row">
    <div class="broker-select-row__label">当前选中</div>
    <div class="broker-select-row__select">
      <Dropdown
        :items="brokerItems(src)"
        :model-value="src.selectedBrokerId ?? ''"
        :placeholder="src.brokerSelectPending ? '切换中…' : '请选择经纪商'"
        :loading-text="src.brokerSelectPending ? '切换中…' : ''"
        :disabled="!isStateIdle(src.process_state, src.loginState) || src.brokerSelectPending"
        @update:model-value="(v: string | number) => store.selectBroker(src.id, String(v))"
      />
    </div>
  </div>
</template>

<style scoped>
/* Broker select row */
.broker-select-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-12);
}

.broker-select-row__label {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  font-weight: var(--font-weight-medium, 500);
  flex-shrink: 0;
  width: 70px;
}

.broker-select-row__select {
  max-width: 280px;
  width: 280px;
}
</style>