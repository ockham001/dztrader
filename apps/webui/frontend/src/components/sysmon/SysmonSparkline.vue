<script setup lang="ts">
import { computed } from 'vue'
import {
  sparklineAriaLabel,
  sparklineGeometry,
  sparklineTitle,
  sparklineWindowComplete,
  type SparklinePoint,
  type SparklineWindow,
} from '@/composables/sysmonSparkline'

// 内联趋势迷你曲线（#127）：实时标量指标旁的一条辅助线，数据由 stores/sysmonTrend 提供
//（历史 REST 补齐 + WS 增量推进，末点 == 旁边的当前值）。
//
// 时间窗文案由 `window` 派生（#142）：完整覆盖才称「最近 15 分钟」；实际只覆盖几十秒 /
// 被 `max_points` 截断且未满窗时标注真实覆盖时长，降级保留的序列标注未刷新。满窗时截断注
// 被抑制、只留满窗陈述（#145：补拉保留的更早点可能把可见跨度拉满）。文案实现是纯函数
//（`sparklineTitle` / `sparklineAriaLabel`），与数据层 `sparklineWindow` 同一真相源。
//
// 用纯 SVG 折线而不是 ECharts：sparkline 只表达「形状」，不需要坐标轴/缩放/tooltip；
// 纯 SVG 无 canvas 依赖、可单测、宽高跟随容器（viewBox + preserveAspectRatio 拉伸，
// non-scaling-stroke 保证线宽不被拉变形），符合验收标准的「图表重排用容器自适应」。
const props = defineProps<{
  points: SparklinePoint[]
  /** 指标名（aria-label / title），点击语义是「查看该指标历史」 */
  label: string
  /** 时间窗的如实口径（#142）：完整覆盖 / 实际覆盖时长 / 截断 / 历史未刷新 */
  window: SparklineWindow
}>()

const emit = defineEmits<{ open: [] }>()

/** 有效点少于两个时返回 null：一个点画不出趋势，整条 sparkline 不渲染（静默降级） */
const geometry = computed(() => sparklineGeometry(props.points))

const title = computed(() => sparklineTitle(props.label, props.window))
const ariaLabel = computed(() => sparklineAriaLabel(props.label, props.window))
/** 是否覆盖满整个时间窗（data 属性供测试 / E2E 断言，不再作为可传字段） */
const complete = computed(() => sparklineWindowComplete(props.window))
</script>

<template>
  <button
    v-if="geometry"
    type="button"
    class="sysmon-sparkline"
    :class="{ 'sysmon-sparkline--stale': window.stale }"
    :title="title"
    :aria-label="ariaLabel"
    :data-last-value="geometry.lastValue"
    :data-window-complete="complete"
    :data-window-truncated="window.truncated"
    :data-window-stale="window.stale"
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

/* 历史未刷新（#142：补拉降级但保留上一轮序列）：虚线与 title/aria 的限定说明同时成立，
   避免把未刷新的曲线看成最新历史。 */
.sysmon-sparkline--stale .sysmon-sparkline__line {
  stroke-dasharray: 3 2;
}
</style>
