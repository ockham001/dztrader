<script setup lang="ts">
import { computed, onMounted } from 'vue'
import Dropdown from '@/components/shared/Dropdown.vue'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import Icon from '@/components/shared/Icon.vue'
import SysmonHistoryChart from '@/components/sysmon/SysmonHistoryChart.vue'
import { formatDuration } from '@/composables/format'
import {
  historyUnavailableText,
  SYSMON_HISTORY_LEVELS,
  SYSMON_HISTORY_METRICS,
  SYSMON_HISTORY_RANGES,
  useSysmonHistoryStore,
} from '@/stores/sysmonHistory'

// 资源监控历史视图（《帧契约：系统监控》§6.1）：时间范围与粒度选择、min/max 包络、缺口与时钟
// 跳变标记、悬停显示精确值与时间。
//
// 三态由 store 的 state 承载（验收标准）：有数据 / 无数据 / 历史不可用；另有「加载中」。
// 历史不可用时显示明确降级文案 + 原因，**不显示任何伪造数据**（不得用 0 冒充）。
// 本视图只发 REST 查询，不订阅任何 WS 消息——切换到历史视图不影响实时 store 的 WS 数据。
const history = useSysmonHistoryStore()

onMounted(() => { void history.load() })

const metricItems = computed<DropdownItem[]>(() =>
  SYSMON_HISTORY_METRICS.map(metric => ({ label: metric.label, value: metric.key })),
)
const rangeItems = computed<DropdownItem[]>(() =>
  SYSMON_HISTORY_RANGES.map(range => ({ label: range.label, value: String(range.seconds) })),
)
const levelItems = computed<DropdownItem[]>(() =>
  SYSMON_HISTORY_LEVELS.map(item => ({ label: item.label, value: item.value })),
)

const selectedMetricLabel = computed(() => history.metricOption?.label ?? '')
const selectedRangeLabel = computed(
  () => SYSMON_HISTORY_RANGES.find(r => r.seconds === history.rangeSeconds)?.label ?? '',
)
const selectedLevelLabel = computed(
  () => SYSMON_HISTORY_LEVELS.find(l => l.value === history.level)?.label ?? '',
)

const metricFormat = computed(() => history.metricOption?.format ?? 'number')
const metricLabel = computed(() => history.metricOption?.label ?? '')

const isUnavailable = computed(() => history.state === 'unavailable')
const isReady = computed(() => history.state === 'ready')
const isEmpty = computed(() => history.state === 'empty')
const isLoading = computed(() => history.state === 'loading')

const unavailableDesc = computed(
  () => (history.unavailableReason ? historyUnavailableText(history.unavailableReason) : ''),
)

/** 生效粒度与范围说明（auto 的选择/回退与截断后的实际覆盖时长都如实回显，不让用户猜） */
const summaryText = computed(() => {
  if (!isReady.value) return ''
  const level = history.effectiveLevel === 'raw' ? '原始记录' : '一分钟聚合'
  const interval = history.intervalSec
  const parts = [`粒度 ${level}`]
  if (interval !== null) parts.push(`桶宽 ${interval} 秒`)
  parts.push(`${history.points.length} 个点`)
  return parts.join(' · ')
})

/**
 * 截断后的实际覆盖时长（《帧契约：系统监控》§6.1.2：前端必须把范围控件同步到生效值）。
 * 换算是纯展示：把秒数说成「实际覆盖 X」，与「最近 N」的预设并列显示，
 * 用户就能看出自己看到的不是完整的 N。换算实现与内联趋势共用 `formatDuration`（#142）。
 */
const effectiveCoverageText = computed(() => {
  if (!history.truncated || history.effectiveRangeSeconds === null) return ''
  return `实际覆盖最近 ${formatDuration(history.effectiveRangeSeconds)}`
})

/** 缺口与标记的数量摘要（无标记时不渲染该行） */
const anomalySummary = computed(() => {
  if (!isReady.value) return ''
  const parts: string[] = []
  if (history.gaps.length > 0) parts.push(`${history.gaps.length} 处采集中断`)
  const jumps = history.marks.filter(mark => mark.kind === 'clock_jump').length
  if (jumps > 0) parts.push(`${jumps} 处时钟跳变`)
  return parts.join(' · ')
})

async function onMetricChange(value: string | number): Promise<void> {
  await history.setMetric(String(value))
}
async function onRangeChange(value: string | number): Promise<void> {
  await history.setRange(Number(value))
}
async function onLevelChange(value: string | number): Promise<void> {
  await history.setLevel(String(value) as 'auto' | 'raw' | 'minute')
}
</script>

<template>
  <div class="sysmon-history" data-test="sysmon-history">
    <!-- 查询控件：时间范围 / 粒度 / 指标 -->
    <div class="sysmon-history__controls">
      <div class="sysmon-history__control">
        <span class="sysmon-history__control-label">指标</span>
        <Dropdown
          :items="metricItems"
          :model-value="history.metricKey"
          :placeholder="selectedMetricLabel"
          data-test="sysmon-history-metric"
          @update:model-value="onMetricChange"
        />
      </div>
      <div class="sysmon-history__control">
        <span class="sysmon-history__control-label">时间范围</span>
        <Dropdown
          :items="rangeItems"
          :model-value="String(history.rangeSeconds)"
          :placeholder="selectedRangeLabel"
          data-test="sysmon-history-range"
          @update:model-value="onRangeChange"
        />
      </div>
      <div class="sysmon-history__control">
        <span class="sysmon-history__control-label">粒度</span>
        <Dropdown
          :items="levelItems"
          :model-value="history.level"
          :placeholder="selectedLevelLabel"
          data-test="sysmon-history-level"
          @update:model-value="onLevelChange"
        />
      </div>
      <button
        class="ds-btn ds-btn--secondary ds-btn--sm"
        type="button"
        data-test="sysmon-history-refresh"
        @click="history.load()"
      >刷新</button>
    </div>

    <!-- 加载中 -->
    <div v-if="isLoading" class="ds-empty sysmon-history__state" data-test="sysmon-history-loading">
      <div class="ds-empty__title">加载中</div>
      <div class="ds-empty__desc">正在查询历史库</div>
    </div>

    <!-- 历史不可用（验收标准三态之二）：明确降级文案 + 原因，不显示任何伪造数据 -->
    <div
      v-else-if="isUnavailable"
      class="ds-empty sysmon-history__state"
      data-test="sysmon-history-unavailable"
    >
      <div class="ds-empty__icon"><Icon name="monitor" :size="20" /></div>
      <div class="ds-empty__title">历史不可用</div>
      <div class="ds-empty__desc" data-test="sysmon-history-unavailable-reason">{{ unavailableDesc }}</div>
    </div>

    <!-- 查询失败（HTTP 层，非降级）：可重试 -->
    <div v-else-if="history.error" class="ds-empty sysmon-history__state" data-test="sysmon-history-error">
      <div class="ds-empty__title">历史查询失败</div>
      <div class="ds-empty__desc">{{ history.error }}</div>
      <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="history.load()">重试</button>
    </div>

    <!-- 有数据（验收标准三态之一）：包络曲线 + 标记 + 摘要 -->
    <template v-else-if="isReady">
      <div class="sysmon-history__summary">
        <span class="ds-tag" data-test="sysmon-history-summary">{{ summaryText }}</span>
        <!-- 因点数上限只显示了最近一段：明确告知，不假装完整 -->
        <span v-if="history.truncated" class="ds-tag ds-tag--warning" data-test="sysmon-history-truncated">
          点数超限，仅显示最近一段<template v-if="effectiveCoverageText">（{{ effectiveCoverageText }}）</template>
        </span>
        <span v-if="anomalySummary" class="ds-tag ds-tag--warning" data-test="sysmon-history-anomaly">
          {{ anomalySummary }}
        </span>
      </div>
      <SysmonHistoryChart
        :points="history.points"
        :marks="history.marks"
        :gaps="history.gaps"
        :format="metricFormat"
        :title="metricLabel"
      />
    </template>

    <!-- 无数据（验收标准三态之三）：范围内确实没有桶，与「不可用」是不同事实 -->
    <div v-else-if="isEmpty" class="ds-empty sysmon-history__state" data-test="sysmon-history-empty">
      <div class="ds-empty__title">该时间范围无数据</div>
      <div class="ds-empty__desc">历史库可用，但所选范围内没有记录；可换更长的范围或等待采集</div>
    </div>
  </div>
</template>

<style scoped>
.sysmon-history {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
}

.sysmon-history__controls {
  display: flex;
  align-items: flex-end;
  gap: var(--spacer-12);
  flex-wrap: wrap;
}

.sysmon-history__control {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  min-width: 0;
}

.sysmon-history__control-label {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.sysmon-history__summary {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  flex-wrap: wrap;
}

.sysmon-history__state {
  padding: var(--spacer-32) var(--spacer-12);
}
</style>
