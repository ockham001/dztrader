<script setup lang="ts">
/**
 * 顶栏墙钟（#452 / ADR 0054）：本机 24 小时制墙钟 + 时钟偏差括号。
 *
 * 三段职责，且每段只做一件事：
 * 1. **每秒 tick 本机 `Date`**：墙钟回答「现在几点」，不用快照 `timestamp`（那是数据龄口径，
 *    停推时会让时钟停住）。页面 `document.hidden` 时停表，恢复后立即对齐当前时刻再继续——
 *    不补播隐藏期间的秒（用户要看的是现在，不是一段过期动画）。
 * 2. **把口径交给纯函数**：四段文本 / 偏差括号 / title / aria 文案全部由
 *    `composables/topbarClock.topbarClockModel` 产出（可单测），本组件只负责定时与渲染。
 * 3. **四档格式由 CSS 断点切换**（1280 / 1024 / 768）：组件恒渲染四段文本片段，可见性交给
 *    `assets/css/layout.css` 的 `.topbar-clock__*` 规则——JS 不读视口宽度，也不用容器查询。
 *
 * 卸载时清 interval 与 visibility 监听（#452 AC：组件卸载不得留下定时器）。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { formatAge } from '@/composables/format'
import { clockProbeAgeSec, resolveClockThresholds } from '@/composables/sysmonClock'
import { topbarClockModel } from '@/composables/topbarClock'
import { useSysmonStore } from '@/stores/sysmon'

const sysmon = useSysmonStore()

/** 本机当前时刻；每秒由 {@link tick} 刷新（组件内状态，只有本组件重渲染） */
const now = ref(new Date())

/** tick 定时器（`null` = 未运行：卸载后 / 页面隐藏中） */
let timer: ReturnType<typeof setInterval> | null = null

function tick(): void {
  now.value = new Date()
}

function startTimer(): void {
  if (timer !== null) return
  timer = setInterval(tick, 1_000)
}

function stopTimer(): void {
  if (timer === null) return
  clearInterval(timer)
  timer = null
}

/**
 * 页面可见性切换：隐藏时停表（后台标签不加每分钟 60 次无谓渲染）；恢复时**先对齐当前时刻**
 * 再重启，因此不会补播隐藏期间丢失的秒（那只会让墙钟从旧时刻开始追赶）。
 */
function handleVisibilityChange(): void {
  if (document.hidden) {
    stopTimer()
    return
  }
  tick()
  startTimer()
}

onMounted(() => {
  tick()
  // 首帧就落在后台标签（document.hidden）时不启动定时器：等 visibilitychange 恢复时再对齐并起表
  if (!document.hidden) startTimer()
  document.addEventListener('visibilitychange', handleVisibilityChange)
})

onUnmounted(() => {
  stopTimer()
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})

/** 浏览器 IANA 时区名（tooltip 说明「按哪个时区显示」；取不到时交给纯函数回退兜底文案） */
function resolveTimeZone(): string {
  try {
    return Intl.DateTimeFormat().resolvedOptions().timeZone ?? ''
  } catch {
    return ''
  }
}

/** 浏览器 IANA 时区名：组件生命周期内不变，取一次即可（tooltip 的「时区」字段） */
const timeZone = resolveTimeZone()

const model = computed(() => {
  // 上次探测的相对时间与监控页同口径：基准取快照 `timestamp`（与服务端 last_probe_unix_ms
  // 同一时间轴），**不混用本机墙钟**——否则两台机器时钟不一致时相对时间会失真。
  const snapshotTs = sysmon.snapshot?.timestamp
  const probeAgeSec = sysmon.clock && typeof snapshotTs === 'number'
    ? clockProbeAgeSec(sysmon.clock.last_probe_unix_ms, snapshotTs)
    : null
  return topbarClockModel({
    now: now.value,
    timeZone,
    clock: sysmon.clock,
    thresholds: resolveClockThresholds(sysmon.config?.clock),
    snapshotPresent: sysmon.snapshot !== null,
    probeAgeText: probeAgeSec === null ? null : formatAge(probeAgeSec),
  })
})

/** 只有括号文本套状态色（整块时钟永远中性）：`--warn` / `--danger` 由 layout.css 定义 */
const deviationClass = computed(() => {
  const level = model.value.deviation?.level
  return level === null || level === undefined ? undefined : `topbar-clock__offset--${level}`
})
</script>

<template>
  <!-- 注释写在根元素内部：根节点必须是单个 <time>，否则 $el 退化为包裹元素（与 Topbar 同款纪律）。
       aria-label 覆盖可访问名（完整日期星期时间 + 偏差状态）；四段文本片段由 CSS 断点取舍，
       但**秒始终保留**（#452 拍板记录 1）。 -->
  <time
    class="topbar-clock"
    data-test="topbar-clock"
    :datetime="model.dateTime"
    :aria-label="model.ariaLabel"
    :title="model.title"
  >
    <span class="topbar-clock__date" data-test="topbar-clock-date">{{ model.dateFull }}</span>
    <span class="topbar-clock__date-short" data-test="topbar-clock-date-short">{{ model.dateShort }}</span>
    <span class="topbar-clock__weekday" data-test="topbar-clock-weekday">{{ model.weekday }}</span>
    <span class="topbar-clock__time" data-test="topbar-clock-time">{{ model.time }}</span>
    <!-- 配置关闭（clock = null）时整段括号不渲染：不放 0 或空壳（ADR 0054） -->
    <span
      v-if="model.deviation"
      class="topbar-clock__offset"
      :class="deviationClass"
      data-test="topbar-clock-offset"
    >{{ model.deviation.text }}</span>
  </time>
</template>
