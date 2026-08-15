<script setup lang="ts">
import { computed } from 'vue'

const props = withDefaults(defineProps<{
  current: number
  max?: number
  min?: number
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

type StatusState = 'idle' | 'loading' | 'done'

const state = computed<StatusState>(() => {
  if (props.current <= props.min) return 'idle'
  if (props.current >= props.max) return 'done'
  return 'loading'
})

const text = computed(() => {
  if (state.value === 'idle') return props.idleText
  if (state.value === 'done') return props.doneText
  return props.loadingText
})

const fillPercent = computed(() => {
  if (state.value !== 'loading') return null
  if (props.max === props.min) return 0
  return ((props.current - props.min) / (props.max - props.min)) * 100
})

const isIndeterminate = computed(() => {
  return state.value === 'loading' && fillPercent.value === null
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
