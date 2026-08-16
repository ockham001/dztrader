<script setup lang="ts">
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// 停止源重启入口（P3 N1 回归修复）：DB 真相源改造后停止源常驻列表，
// 但此前无任何启动入口（store.start 是死代码、"添加"下拉对 added 源直接 return）。
// start 无 running 前置（REST /start 仅需 admin + 源存在），任意非 Running 态可重启
// 注意：Starting 态（master 启动中，startPending 通常为 true 禁用）也满足
// "非 Running"，同渲染按钮——重复点击 start 经 process store 防重入 + 后端幂等兜底。
// 注：模板用 props.source 访问（非裸 source）——既满足 noUnusedLocals（props 被使用），
// 又能让父组件把该导入识别为模板组件引用。
const props = defineProps<{ source: MarketSourceView }>()
const store = useMarketSourcesStore()
</script>

<template>
  <button
    v-if="props.source.process_state !== 'Running'"
    class="ds-btn ds-btn--sm ds-btn--primary"
    type="button"
    :disabled="props.source.startPending"
    @click="store.start(props.source.id)"
  >
    <span v-if="props.source.startPending" class="psb__spinner"></span>
    {{ props.source.startPending ? '启动中' : '启动进程' }}
  </button>
</template>

<style scoped>
.psb__spinner {
  display: inline-block; width: 12px; height: 12px;
  border: 1.5px solid currentColor; border-top-color: transparent;
  border-radius: 50%; animation: psb-spin 0.6s linear infinite;
  opacity: 0.7; flex-shrink: 0; margin-right: 4px;
}
@keyframes psb-spin { to { transform: rotate(360deg); } }
</style>