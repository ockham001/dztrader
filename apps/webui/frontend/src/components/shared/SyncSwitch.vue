<script setup lang="ts">
/**
 * SyncSwitch — 同步开关公共组件
 *
 * 用于需要等待服务器确认的开关场景。点击后进入 pending 状态，
 * 父组件完成异步操作后通过 v-model 更新最终状态。
 *
 * 用法：
 *   <SyncSwitch v-model="enabled" @change="onToggle" />
 *
 *   async function onToggle(val: boolean) {
 *     try {
 *       await api.set(val)
 *       toast.success(val ? '已启用' : '已禁用')
 *     } catch {
 *       toast.error('操作失败')
 *       // v-model 不会更新，switch 自动回退
 *     }
 *   }
 */

const props = defineProps<{
  modelValue: boolean
  pending?: boolean
  disabled?: boolean
}>()

const emit = defineEmits<{
  'update:modelValue': [value: boolean]
  change: [value: boolean]
}>()

function onToggle(e: Event): void {
  const target = e.target as HTMLInputElement
  const next = target.checked
  // 不让原生 checkbox 立即改变 — 等待父组件更新 v-model
  target.checked = props.modelValue
  if (props.pending || props.disabled) return
  emit('change', next)
}
</script>

<template>
  <label
    class="ds-switch"
    :class="{
      'ds-switch--pending': pending,
      'ds-switch--disabled': disabled,
    }"
    :style="{ cursor: disabled ? 'not-allowed' : 'pointer', opacity: disabled ? 0.5 : 1 }"
  >
    <input
      type="checkbox"
      :checked="modelValue"
      :disabled="disabled || pending"
      @change="onToggle"
    >
    <span class="ds-switch__track">
      <span class="ds-switch__thumb"></span>
    </span>
  </label>
</template>
