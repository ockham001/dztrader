<script setup lang="ts">
import { computed, onMounted, watch } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { BarChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent, DataZoomComponent } from 'echarts/components'
import { useLogsStore } from '@/stores/logs'
import Dropdown from '@/components/shared/Dropdown.vue'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'

use([CanvasRenderer, BarChart, GridComponent, TooltipComponent, LegendComponent, DataZoomComponent])

const store = useLogsStore()

const statCards = computed(() => {
  const s = store.stats
  if (!s) return [
    { label: '总行数', value: 0, color: 'var(--text-default)' },
    { label: 'INFO', value: 0, color: 'var(--status-success-default)' },
    { label: 'WARN', value: 0, color: 'var(--status-alert-default)' },
    { label: 'ERROR', value: 0, color: 'var(--status-error-default)' },
  ]
  return [
    { label: '总行数', value: s.total, color: 'var(--text-default)' },
    { label: 'INFO', value: s.by_level['info'] ?? 0, color: 'var(--status-success-default)' },
    { label: 'WARN', value: s.by_level['warning'] ?? 0, color: 'var(--status-alert-default)' },
    { label: 'ERROR', value: s.by_level['error'] ?? 0, color: 'var(--status-error-default)' },
  ]
})

const chartOption = computed(() => {
  const levels = ['info', 'warning', 'error', 'critical']
  const colors: Record<string, string> = {
    info: '#15A877',
    warning: '#FEA900',
    error: '#E8463A',
    critical: '#b000ff',
  }

  const xData = store.timeline.map(b => b.ts)
  const series = levels.map(lvl => ({
    name: lvl,
    type: 'bar' as const,
    stack: 'total',
    data: store.timeline.map(b => b.counts[lvl] ?? 0),
    itemStyle: { color: colors[lvl] },
  }))

  return {
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'shadow' },
    },
    legend: {
      data: levels,
      bottom: 0,
    },
    grid: {
      left: '3%',
      right: '4%',
      bottom: '15%',
      top: '5%',
      containLabel: true,
    },
    xAxis: {
      type: 'category',
      data: xData,
      axisLabel: {
        rotate: xData.length > 20 ? 45 : 0,
        fontSize: 10,
      },
    },
    yAxis: {
      type: 'value',
    },
    dataZoom: [
      { type: 'inside', start: 0, end: 100 },
      { type: 'slider', start: 0, end: 100, height: 20, bottom: 30 },
    ],
    series,
  }
})

const bucketItems: DropdownItem[] = [
  { value: 'minute', label: '分钟' },
  { value: 'hour', label: '小时' },
  { value: 'day', label: '天' },
]

async function handleBucketChange(value: string | number): Promise<void> {
  store.timelineBucket = String(value) as 'minute' | 'hour' | 'day'
  await store.loadTimeline()
}

onMounted(() => {
  if (store.selectedFile) {
    store.loadAnalysisData()
  }
})

watch(() => store.selectedFile, (newFile) => {
  if (newFile) {
    store.loadAnalysisData()
  }
})

watch(() => store.browserTab, (tab) => {
  if (tab === 'analysis' && store.selectedFile) {
    store.loadAnalysisData()
  }
})
</script>

<template>
  <div class="log-analysis">
    <div v-if="!store.selectedFile" class="log-analysis__empty">
      请在左侧选择一个日志文件
    </div>

    <template v-else>
      <div class="log-analysis__cards">
        <div
          v-for="card in statCards"
          :key="card.label"
          class="log-analysis__card"
        >
          <div class="log-analysis__card-value" :style="{ color: card.color }">{{ card.value }}</div>
          <div class="log-analysis__card-label">{{ card.label }}</div>
        </div>
      </div>

      <div class="log-analysis__chart-section">
        <div class="log-analysis__section-header">
          <span class="log-analysis__section-title">时间轴</span>
          <Dropdown
            :items="bucketItems"
            :model-value="store.timelineBucket"
            placeholder="分钟"
            @update:model-value="handleBucketChange"
          />
        </div>
        <div v-if="store.timelineLoading" class="log-analysis__loading">加载中...</div>
        <div v-else-if="store.timeline.length === 0" class="log-analysis__loading">暂无时间轴数据</div>
        <VChart
          v-else
          class="log-analysis__chart"
          :option="chartOption"
          autoresize
        />
      </div>

      <div class="log-analysis__aggregate-section">
        <div class="log-analysis__section-header">
          <span class="log-analysis__section-title">错误聚合</span>
        </div>
        <div v-if="store.aggregateLoading" class="log-analysis__loading">加载中...</div>
        <div v-else-if="store.aggregate.length === 0" class="log-analysis__loading">暂无错误聚合数据</div>
        <table v-else class="ds-table">
          <thead>
            <tr>
              <th>错误模板</th>
              <th style="width: 80px">次数</th>
              <th style="width: 180px">首次时间</th>
              <th style="width: 180px">末次时间</th>
              <th style="width: 80px">操作</th>
            </tr>
          </thead>
          <tbody>
            <template v-for="agg in store.aggregate" :key="agg.msg_pattern">
              <tr>
                <td class="log-analysis__msg-pattern">{{ agg.msg_pattern }}</td>
                <td>{{ agg.count }}</td>
                <td class="log-analysis__ts">{{ agg.first_ts.substring(0, 19) }}</td>
                <td class="log-analysis__ts">{{ agg.last_ts.substring(0, 19) }}</td>
                <td>
                  <button
                    class="ds-btn ds-btn--secondary ds-btn--sm"
                    type="button"
                    @click="store.togglePatternExpanded(agg.msg_pattern)"
                  >{{ store.expandedPatterns.has(agg.msg_pattern) ? '收起' : '查看样本' }}</button>
                </td>
              </tr>
              <tr v-if="store.expandedPatterns.has(agg.msg_pattern)">
                <td colspan="5" class="log-analysis__samples">
                  <div v-for="(sample, i) in agg.samples" :key="i" class="log-analysis__sample-line">
                    {{ sample }}
                  </div>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </template>
  </div>
</template>

<style scoped>
.log-analysis {
  flex: 1;
  min-height: 400px;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
  overflow-y: auto;
  padding: var(--spacer-12);
}
.log-analysis__empty {
  padding: var(--spacer-48);
  text-align: center;
  color: var(--text-tertiary);
  font-size: var(--body-base-font-size);
}

.log-analysis__cards {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: var(--spacer-12);
}
.log-analysis__card {
  padding: var(--spacer-16);
  text-align: center;
  background: var(--bg-overlay-l1);
  border-radius: var(--radius-8);
}
.log-analysis__card-value {
  font-size: 28px;
  font-weight: 700;
  font-variant-numeric: tabular-nums;
  line-height: 1.2;
}
.log-analysis__card-label {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  margin-top: var(--spacer-4);
}

.log-analysis__chart-section {
  padding: var(--spacer-16);
  background: var(--bg-overlay-l1);
  border-radius: var(--radius-8);
}
.log-analysis__section-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--spacer-12);
}
.log-analysis__section-title {
  font-size: var(--body-base-font-size);
  font-weight: 600;
  color: var(--text-default);
}
.log-analysis__section-header .ds-dropdown {
  width: auto;
  flex-shrink: 0;
  max-width: 160px;
}
.log-analysis__chart {
  height: 320px;
  width: 100%;
}
.log-analysis__loading {
  height: 200px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
}

.log-analysis__aggregate-section {
  padding: var(--spacer-16);
  background: var(--bg-overlay-l1);
  border-radius: var(--radius-8);
  overflow-x: auto;
}
.log-analysis__msg-pattern {
  font-family: var(--code-terminal-font-family);
  font-size: var(--body-sm-font-size);
  max-width: 400px;
  word-break: break-all;
}
.log-analysis__ts {
  font-family: var(--code-terminal-font-family);
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
  font-variant-numeric: tabular-nums;
}
.log-analysis__samples {
  background: var(--bg-base-secondary);
  padding: var(--spacer-8) var(--spacer-12);
}
.log-analysis__sample-line {
  font-family: var(--code-terminal-font-family);
  font-size: var(--code-terminal-font-size);
  line-height: var(--code-terminal-line-height);
  color: var(--text-secondary);
  white-space: pre-wrap;
  word-break: break-all;
  padding: 2px 0;
}

@media (max-width: 768px) {
  .log-analysis__cards {
    grid-template-columns: repeat(2, 1fr);
  }
  .log-analysis__chart {
    height: 240px;
  }
}
</style>
