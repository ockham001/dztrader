<script setup lang="ts">
import { computed } from 'vue'

const props = withDefaults(defineProps<{
  current: number
  max?: number
  min?: number
  desc?: string // eslint-disable-line vue/require-default-prop -- 可选展示文本，不传即无描述，undefined 合法
  idleText?: string
  loadingText?: string
  doneText?: string
  failedText?: string
  /**
   * 失败态由调用方判定（#509）：md 没有失败态（登录失败停在中间值，ADR 0059 决策 1）；
   * td 的四个失败态停在 3/5/7/8，判定在各自 composable 里。共享组件不懂业务词，
   * 只接收结论——`true` 时压过其余状态，徽章显示 failedText（默认「失败」）。
   */
  failed?: boolean
  mini?: boolean
}>(), {
  max: 1,
  min: 0,
  idleText: '未登录',
  loadingText: '登录中',
  doneText: '已登录',
  failedText: '失败',
  failed: false,
  mini: false,
})

type StatusState = 'idle' | 'loading' | 'done' | 'failed' | 'indeterminate'

const state = computed<StatusState>(() => {
  // failed 是调用方给的语义结论，优先级最高：失败态下 current/max 停在失败阶段，数值映射会
  // 说成「进行中」——那正是用户分不清「还在登录」与「已经失败」的根源（ADR 0059）。
  if (props.failed) return 'failed'
  // 契约 progress: max <= min（含 max==0）为不确定进度，current 值忽略
  if (props.max <= props.min) return 'indeterminate'
  if (props.current <= props.min) return 'idle'
  if (props.current >= props.max) return 'done'
  return 'loading'
})

// 文本源: 失败态用调用方给的 failedText（desc 是阶段文案，不能替代「失败」结论）；
// 其余态后端 desc 优先（契约 progress: desc 为展示文案，状态判定仍走数值映射），
// 未提供时回落调用方三态固定文案；不确定进度复用 loading 文案
const text = computed(() => {
  if (state.value === 'failed') return props.failedText
  if (props.desc) return props.desc
  if (state.value === 'idle') return props.idleText
  if (state.value === 'done') return props.doneText
  return props.loadingText
})

const fillPercent = computed(() => {
  if (state.value !== 'loading') return null
  return ((props.current - props.min) / (props.max - props.min)) * 100
})

const isIndeterminate = computed(() => {
  return state.value === 'indeterminate'
})

const stateClass = computed(() => {
  return `ds-status--${state.value}`
})

const fillStyle = computed(() => {
  if (fillPercent.value !== null) {
    return { '--ds-status-fill': `${fillPercent.value}%` }
  }
  return {}
})
</script>

<template>
  <div
    class="ds-status"
    :class="[stateClass, { 'ds-status--mini': mini, 'ds-status--indeterminate': isIndeterminate }]"
    :style="fillStyle"
  >
    <span class="ds-status__text">{{ text }}</span>
    <div class="ds-status__progress">
      <span class="ds-status__progress-text">{{ text }}</span>
      <span class="ds-status__bar"><span class="ds-status__bar-fill"></span></span>
    </div>
  </div>
</template>
