<script setup lang="ts">
import { computed } from 'vue'
import StatusIndicator from '@/components/shared/StatusIndicator.vue'
import ProcessStartButton from './ProcessStartButton.vue'
import { processStateText, processStateColor } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// GenericCard 接收 MarketSource (契约 md-config)
// ui_card 未命中映射表时的回退卡片, 只显示基本信息 (未知大类)
// 完整 CTP 功能见 CtpCard.vue
const props = defineProps<{
  source: MarketSourceView
}>()

const store = useMarketSourcesStore()

const src = computed(() => props.source)
</script>

<template>
  <div class="source-card ds-card">
    <!-- Card Header (可展开/收起, 但 body 仅显示基本信息) -->
    <div class="source-card__header" @click="store.toggleExpand(src.id)">
      <span class="source-card__chevron" :class="{ 'is-expanded': src.expanded }" aria-hidden="true">›</span>
      <div class="source-card__info">
        <span class="source-card__info-name">{{ src.display_name || src.source_name }}</span>
        <span class="source-card__info-meta">{{ src.source_name }} (ui_card: {{ src.ui_card || '未知' }})</span>
      </div>
      <div class="source-card__meta-row">
        <span class="source-card__info-meta" :style="{ color: processStateColor(src.process_state) }">进程：<span>{{ processStateText(src.process_state) }}</span></span>
      </div>
      <StatusIndicator
        :current="src.progressView.current"
        :max="src.progressView.max"
        :min="src.progressView.min"
        :desc="src.progressView.desc"
        :idle-text="'未登录'"
        :loading-text="'登录中'"
        :done-text="'已登录'"
        :mini="true"
      />
    </div>

    <!-- Card Body: 仅显示基本信息提示 -->
    <div v-if="src.expanded" class="source-card__body is-open">
      <div style="margin-bottom: var(--spacer-12)">
        <ProcessStartButton :source="src" />
      </div>
      <div class="card-hint">
        未知大类 <code class="mono">{{ src.ui_card }}</code>，未注册专用卡片组件。
        请在 <code class="mono">marketSourcesCardRegistry.ts</code> 中注册对应卡片。
      </div>
    </div>
  </div>
</template>

<style scoped>
.source-card {
  margin-bottom: var(--spacer-12);
  overflow: hidden;
  color: var(--text-default);
  min-width: 0;
}

.source-card__header {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  padding: var(--spacer-16) var(--spacer-20);
  cursor: pointer;
  flex-wrap: nowrap;
  overflow-x: auto;
}

.source-card__header:hover {
  background: var(--bg-overlay-l1);
}

.source-card__chevron {
  display: inline-block;
  width: 16px;
  height: 16px;
  font-size: 16px;
  line-height: 1;
  color: var(--icon-tertiary);
  transition: transform 0.2s ease;
  flex-shrink: 0;
}

.source-card__chevron.is-expanded {
  transform: rotate(90deg);
}

.source-card__body {
  padding: 0 var(--spacer-20) var(--spacer-20);
  overflow-x: auto;
}

.source-card__body.is-open {
  display: block;
}

.source-card__info {
  display: flex;
  flex-direction: column;
  gap: 1px;
  flex-shrink: 0;
}

.source-card__info-name {
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-secondary);
  white-space: nowrap;
}

.source-card__info-meta {
  font-size: 10px;
  color: var(--text-tertiary);
  white-space: nowrap;
}

.source-card__meta-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  flex-shrink: 1;
  flex-wrap: wrap;
  row-gap: 2px;
}

.card-hint {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  padding: var(--spacer-4) 0;
}

.mono {
  font-family: var(--code-editor-font-family);
  font-variant-numeric: tabular-nums;
}
</style>
