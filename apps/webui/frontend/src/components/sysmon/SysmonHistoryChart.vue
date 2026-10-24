<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent, MarkAreaComponent, MarkLineComponent } from 'echarts/components'
import type { SysmonHistoryGap, SysmonHistoryMark, SysmonHistoryPoint } from '@/types/api'

// 资源监控历史曲线（《帧契约：系统监控》§6.1）：最小 / 最大包络 + 均值 + 缺口底纹 + 标记线。
// 沿用既有图表做法（LogAnalysisTab）：按需注册组件（不整包引入 echarts），
// 只用 CanvasRenderer + LineChart + 必要组件，保持打包体积。
use([CanvasRenderer, LineChart, GridComponent, TooltipComponent, LegendComponent, MarkAreaComponent, MarkLineComponent])

const props = defineProps<{
  points: SysmonHistoryPoint[]
  marks: SysmonHistoryMark[]
  gaps: SysmonHistoryGap[]
  /** 值的展示形态：比率按百分比、字节按容量、其余按数值（与 store 的 metric 定义一致） */
  format: 'ratio' | 'bytes' | 'number'
  /** 值轴标题（指标名） */
  title: string
}>()

/** 数值格式化：null 一律显示「不可用」（不得用 0 冒充，《帧契约：系统监控》§6.1） */
function formatValue(value: number | null): string {
  if (value === null || !Number.isFinite(value)) return '不可用'
  if (props.format === 'ratio') return `${(value * 100).toFixed(1)}%`
  if (props.format === 'bytes') {
    const gib = 1024 ** 3
    const mib = 1024 ** 2
    if (Math.abs(value) >= gib) return `${(value / gib).toFixed(1)} GiB`
    if (Math.abs(value) >= mib) return `${(value / mib).toFixed(1)} MiB`
    return `${value} B`
  }
  return Number.isInteger(value) ? String(value) : value.toFixed(2)
}

/** 时间轴标签：MM-DD HH:mm（跨天范围也能区分） */
function formatAxisTime(unixSec: number): string {
  const date = new Date(unixSec * 1000)
  if (Number.isNaN(date.getTime())) return ''
  const pad = (n: number): string => String(n).padStart(2, '0')
  return `${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`
}

/** 完整时刻（tooltip）：YYYY-MM-DD HH:mm:ss（精确到秒，验收标准「悬停显示精确值与时间」） */
function formatFullTime(unixSec: number): string {
  const date = new Date(unixSec * 1000)
  if (Number.isNaN(date.getTime())) return ''
  const pad = (n: number): string => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} `
    + `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
}

/**
 * 设计 token → 实际颜色值。
 *
 * ECharts 用 Canvas 渲染，**不能**解析 `var(--token)` 字符串（zrender 的样式解析器
 * 只认具体颜色值，遇到 `var(...)` 会得到 undefined 并静默回退默认色）。而 design-tokens
 * 的红线要求颜色必须走 token、不得硬编码。两者同时满足的唯一做法：从 CSS 自定义属性
 * 读出当前主题下的实际值再交给 ECharts——token 仍是唯一来源，主题切换也跟着生效。
 *
 * 为什么不直接写 hex（`LogAnalysisTab` 的既有做法）：那会把 token 的值复制进 TS，
 * 主题/令牌调整时这里就漂移了。
 */
function resolveToken(token: string): string {
  // 主题类挂在 documentElement 上（useTheme），因此从根元素读；SSR/jsdom 下取不到时
  // 给一个中性的回退（transparent 让图形"隐去"而不是画出错误的颜色）。
  if (typeof window === 'undefined' || typeof getComputedStyle !== 'function') return 'transparent'
  const value = getComputedStyle(document.documentElement).getPropertyValue(token).trim()
  return value || 'transparent'
}

/**
 * 当前主题下的图表配色。
 *
 * 必须在主题切换时重新解析：`useTheme` 只是给 `documentElement` 换 class（light/dark），
 * 不重载页面，因此只解析一次的话，挂载期间切主题会让曲线保留旧主题的颜色。
 * 用 MutationObserver 监听 `class` 变化即可覆盖两件事：手工切换与「跟随系统」下的
 * `prefers-color-scheme` 变化（后者由 CSS 媒体查询生效，不改 class——那种情况由
 * media query 监听补齐）。
 */
function currentPalette(): Record<'max' | 'avg' | 'min' | 'gap' | 'mark', string> {
  return {
    max: resolveToken('--status-error-default'),
    avg: resolveToken('--status-primary-default'),
    min: resolveToken('--status-success-default'),
    gap: resolveToken('--status-warning-surface-l1'),
    mark: resolveToken('--status-alert-default'),
  }
}

const palette = ref(currentPalette())

let classObserver: MutationObserver | undefined
let mediaQuery: MediaQueryList | undefined

function refreshPalette(): void {
  palette.value = currentPalette()
}

onMounted(() => {
  refreshPalette()
  if (typeof MutationObserver !== 'function') return
  classObserver = new MutationObserver(refreshPalette)
  classObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] })
  if (typeof window.matchMedia === 'function') {
    mediaQuery = window.matchMedia('(prefers-color-scheme: dark)')
    mediaQuery.addEventListener('change', refreshPalette)
  }
})

onUnmounted(() => {
  classObserver?.disconnect()
  mediaQuery?.removeEventListener('change', refreshPalette)
})

/** 标记 kind 的中文文案（图例 / tooltip） */
function markKindText(kind: string): string {
  if (kind === 'restart') return '进程重启'
  if (kind === 'sample_gap') return '采集中断'
  if (kind === 'clock_jump') return '时钟跳变'
  if (kind === 'unmarked') return '未标记缺口'
  return kind
}

/**
 * 缺口底纹：markArea 的 x 轴区间。
 * ECharts 的 markArea 用「类目轴名称」或「数值轴坐标」定位；这里 x 轴是数值型时间轴
 * （`type: 'value'`），因此可直接给 `[from, to]` 的秒数——无需把缺口映射到类目下标，
 * 也就不会因降采样/截断导致下标错位。
 */
const gapAreas = computed(() =>
  props.gaps.map(gap => [
    {
      xAxis: gap.from * 1000,
      itemStyle: { color: palette.value.gap },
      label: { show: false, formatter: markKindText(gap.kind) },
    },
    { xAxis: gap.to * 1000 },
  ]),
)

/** 标记线：每类标记取首个时刻画一条竖线（同类多条只在 tooltip 汇总，避免竖线过密） */
const markLines = computed(() => {
  const kinds = new Map<string, number>()
  for (const mark of props.marks) {
    if (!kinds.has(mark.kind)) kinds.set(mark.kind, mark.ts)
  }
  return [...kinds.entries()].map(([kind, ts]) => ({
    xAxis: ts * 1000,
    label: { show: true, formatter: markKindText(kind), fontSize: 10 },
    lineStyle: { color: palette.value.mark, type: 'dashed' as const },
  }))
})

/**
 * 图表配置。
 * - x 轴用 `value` 型时间轴（毫秒），轴标签格式化为墙钟时刻；
 * - min / max 构成上下包络（面积填充），avg 画中线；last 不画（与 avg 重叠，图更干净）；
 * - 缺口与标记分别用 markArea / markLine 叠加，不改动数据系列本身。
 */
const chartOption = computed(() => {
  const times = props.points.map(p => p.ts * 1000)

  return {
    tooltip: {
      trigger: 'axis',
      // 悬停显示精确值与时间（验收标准）：tooltip 头为完整时刻，各系列为格式化后的数值
      formatter: (params: unknown) => {
        const items = Array.isArray(params) ? params : [params]
        const first = items[0] as { axisValue?: number } | undefined
        if (!first || typeof first.axisValue !== 'number') return ''
        const lines = [`<strong>${formatFullTime(first.axisValue / 1000)}</strong>`]
        for (const item of items as { seriesName?: string; value?: unknown; marker?: string }[]) {
          const value = typeof item.value === 'number' ? item.value : null
          lines.push(`${item.marker ?? ''}${item.seriesName ?? ''}: ${formatValue(value)}`)
        }
        return lines.join('<br/>')
      },
    },
    legend: { data: ['最大', '平均', '最小'], bottom: 0, textStyle: { fontSize: 10 } },
    grid: { left: '3%', right: '4%', bottom: '12%', top: '6%', containLabel: true },
    xAxis: {
      type: 'value',
      min: times.length > 0 ? times[0] : undefined,
      max: times.length > 0 ? times[times.length - 1] : undefined,
      axisLabel: {
        fontSize: 10,
        formatter: (value: number) => formatAxisTime(value / 1000),
      },
    },
    yAxis: {
      type: 'value',
      name: props.title,
      nameTextStyle: { fontSize: 10 },
      axisLabel: { fontSize: 10, formatter: (value: number) => formatValue(value) },
    },
    series: [
      {
        name: '最大',
        type: 'line' as const,
        data: props.points.map((_, i) => [times[i], props.points[i].max]),
        showSymbol: false,
        lineStyle: { width: 1 },
        itemStyle: { color: palette.value.max },
        markArea: { silent: true, data: gapAreas.value },
        markLine: { silent: true, symbol: 'none', data: markLines.value },
      },
      {
        name: '平均',
        type: 'line' as const,
        data: props.points.map((_, i) => [times[i], props.points[i].avg]),
        showSymbol: false,
        lineStyle: { width: 1.5 },
        itemStyle: { color: palette.value.avg },
      },
      {
        name: '最小',
        type: 'line' as const,
        data: props.points.map((_, i) => [times[i], props.points[i].min]),
        showSymbol: false,
        lineStyle: { width: 1 },
        itemStyle: { color: palette.value.min },
      },
    ],
  }
})
</script>

<template>
  <VChart class="sysmon-history-chart" :option="chartOption" autoresize />
</template>

<style scoped>
.sysmon-history-chart {
  width: 100%;
  height: 320px;
}
</style>
