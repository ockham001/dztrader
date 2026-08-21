<script setup lang="ts">
// =====================================================================
// 行情源卡片「四件套骨架」模板（P3 任务 3 领域功能模板，可复制）
//
// 新增一个行情源类型（如 XTP）时：
//   1. 把本文件复制为 <Type>Card.vue（如 XtpCard.vue）；
//   2. 改源类注释 + 下方分组，把 GenericCard 的「未知」提示换成真实展示；
//   3. 到 stores/marketSourcesCardRegistry.ts 映射表加一行 `xtp: XtpCard`。
//
// 契约（template 层唯一要求，请不要违反）：
//   - props 必须接收 { source: MarketSourceView }（MarketSourcesView 动态传入）；
//   - 读写统一经 useMarketSourcesStore()（勿新增 store/api/view，见 registry 头注释）；
//   - 数据只读 src.*（MarketSourceView 各字段契约见 composables/marketSourceView.ts）；
//   - 若本类型的 md_status 字段与 CTP 差异大，在自己的 <script setup> 里定义独立解析，
//     不复用 parseMdStatus（见 marketSourceView.ts 注释）。
// =====================================================================
import { computed } from 'vue'
import StatusIndicator from '@/components/shared/StatusIndicator.vue'
import ProcessStartButton from './ProcessStartButton.vue'
import { processStateText, processStateColor } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// 待改：把「这里」替换成真正的行情源类型说明
const props = defineProps<{
  source: MarketSourceView
}>()

const store = useMarketSourcesStore()

const src = computed(() => props.source)
</script>

<template>
  <div class="source-card ds-card">
    <!-- 卡片头：展示名 + 进程状态 + 进度，与 GenericCard/CtpCard 一致（可直接复用） -->
    <div class="source-card__header" @click="store.toggleExpand(src.id)">
      <span class="source-card__chevron" :class="{ 'is-expanded': src.expanded }" aria-hidden="true">›</span>
      <div class="source-card__info">
        <span class="source-card__info-name">{{ src.display_name || src.source_name }}</span>
        <span class="source-card__info-meta">{{ src.source_name }}</span>
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

    <!-- 卡片体：在下方按需填本类型的专属展示与操作。 -->
    <div v-if="src.expanded" class="source-card__body is-open">
      <div style="margin-bottom: var(--spacer-12)">
        <ProcessStartButton :source="src" />
      </div>

      <!-- TODO：替换为真实内容（示例分区，删除留空即可） -->
      <div class="card-hint">
        该卡片是骨架模板，请复制本文件为「TypeCard.vue」后实现真实展示。
      </div>
    </div>
  </div>
</template>

<style scoped>
/* 骨架样式可直接沿用 GenericCard 的 source-card 布局；新增只允许 token 变量，禁止裸颜色/间距字面量（P4 红线） */
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
</style>