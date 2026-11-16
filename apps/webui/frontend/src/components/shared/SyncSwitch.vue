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
  /**
   * 开关的无障碍名称（#233）。本组件是纯图形开关，label 内没有可读文字——
   * 视觉上的名称由调用方写在相邻的 `<span>` 里，读屏拿不到，因此必须显式传入。
   * 例：`<SyncSwitch label="自动登录" />`
   */
  label?: string
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
  >
    <input
      type="checkbox"
      :checked="modelValue"
      :disabled="disabled || pending"
      :aria-label="props.label"
      @change="onToggle"
    >
    <span class="ds-switch__track">
      <span class="ds-switch__thumb"></span>
    </span>
  </label>
</template>

<style scoped>
/* #346：禁用态光标与透明度原为内联三元（两个分支都是静态字面量 → 提为修饰类）。
   .ds-switch--disabled 在 components.css 里没有定义（只有 --pending），故在此本地定义；
   非禁用态走 .ds-switch 基础规则：cursor: pointer 已有，opacity 默认 1，计算值与原内联一致。 */
.ds-switch--disabled {
  cursor: not-allowed;
  opacity: 0.5;
}
</style>
