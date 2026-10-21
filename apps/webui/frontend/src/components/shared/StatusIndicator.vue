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
  mini?: boolean
}>(), {
  max: 1,
  min: 0,
  idleText: '未登录',
  loadingText: '登录中',
  doneText: '已登录',
  mini: false,
})

type StatusState = 'idle' | 'loading' | 'done' | 'indeterminate'

const state = computed<StatusState>(() => {
  // 契约 progress: max <= min（含 max==0）为不确定进度，current 值忽略
  if (props.max <= props.min) return 'indeterminate'
  if (props.current <= props.min) return 'idle'
  if (props.current >= props.max) return 'done'
  return 'loading'
})

// 文本源: 后端 desc 优先（契约 progress: desc 为展示文案，状态判定仍走数值映射），
// 未提供时回落调用方三态固定文案；不确定进度复用 loading 文案
const text = computed(() => {
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
