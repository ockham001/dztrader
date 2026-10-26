<script setup lang="ts">
import { computed } from 'vue'
import {
  SPARKLINE_WINDOW_SEC,
  sparklineGeometry,
  type SparklinePoint,
} from '@/composables/sysmonSparkline'

// 内联趋势迷你曲线（#127）：实时标量指标旁的一条辅助线，数据由 stores/sysmonTrend 提供
//（历史 REST 补齐 + WS 增量推进，末点 == 旁边的当前值）。
//
// 用纯 SVG 折线而不是 ECharts：sparkline 只表达「形状」，不需要坐标轴/缩放/tooltip；
// 纯 SVG 无 canvas 依赖、可单测、宽高跟随容器（viewBox + preserveAspectRatio 拉伸，
// non-scaling-stroke 保证线宽不被拉变形），符合验收标准的「图表重排用容器自适应」。
const props = defineProps<{
  points: SparklinePoint[]
  /** 指标名（aria-label / title），点击语义是「查看该指标历史」 */
  label: string
}>()

const emit = defineEmits<{ open: [] }>()

/** 有效点少于两个时返回 null：一个点画不出趋势，整条 sparkline 不渲染（静默降级） */
const geometry = computed(() => sparklineGeometry(props.points))

/** 时间窗文案由常量派生（窗口调整时 title 不漂移） */
const windowMinutes = SPARKLINE_WINDOW_SEC / 60
</script>

<template>
  <button
    v-if="geometry"
    type="button"
    class="sysmon-sparkline"
    :title="`${label} · 最近 ${windowMinutes} 分钟（点击查看历史）`"
    :aria-label="`${label} 最近 ${windowMinutes} 分钟趋势，查看历史`"
    :data-last-value="geometry.lastValue"
    @click="emit('open')"
  >
    <svg
      class="sysmon-sparkline__svg"
      viewBox="0 0 100 24"
      preserveAspectRatio="none"
      aria-hidden="true"
      focusable="false"
    >
      <polyline class="sysmon-sparkline__line" :points="geometry.points" vector-effect="non-scaling-stroke" />
    </svg>
  </button>
</template>

<style scoped>
/* 辅助线：宽度跟随容器（拖窗口 / 折叠侧栏即重排），高度固定为辅助信息量级，不抢首屏 */
.sysmon-sparkline {
  display: block;
  width: 100%;
  min-width: 48px;
  height: 24px;
  padding: 0;
  border: none;
  background: transparent;
  cursor: pointer;
}

.sysmon-sparkline__svg {
  display: block;
  width: 100%;
  height: 100%;
  overflow: visible;
}

.sysmon-sparkline__line {
  fill: none;
  stroke: var(--status-primary-default);
  stroke-width: 1.5;
  stroke-linecap: round;
  stroke-linejoin: round;
}
</style>
