<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, useId } from 'vue'
import Icon from './Icon.vue'
import { useFocusTrap } from '@/composables/useFocusTrap'
import { useScrollLock } from '@/composables/useScrollLock'

const props = defineProps<{
  open: boolean
  title: string
}>()

const emit = defineEmits<{
  close: []
}>()

// 标题与弹窗关联：aria-labelledby 指向标题元素，屏幕阅读器进入对话框时报出名称（#206 A）
const titleId = useId()
const dialogEl = ref<HTMLElement | null>(null)

function handleOverlayClick(e: MouseEvent): void {
  if (e.target === e.currentTarget) {
    emit('close')
  }
}

// 焦点管理（#206 A）：打开时焦点移入对话框、Tab / Shift+Tab 在框内循环、关闭还原到触发元素
const trap = useFocusTrap({ active: computed(() => props.open), container: dialogEl })

// body overflow 锁定：打开时禁背景滚动，关闭还原（原有行为，勿改坏）。
// 走引用计数（useScrollLock）：同时打开多个模态时，先关闭的那个不能解掉后关闭者的锁。
useScrollLock(computed(() => props.open))

// Esc 只由最上层对话框响应：多个模态同时打开时（本组件可多层叠加，App.vue 的
// notify alertdialog 也与本组件共用同一个焦点陷阱栈），若每层都关，
// 按一次会同时关掉多层，键盘用户无法预期。
function handleKeydown(e: KeyboardEvent): void {
  if (e.key !== 'Escape' || !props.open) return
  if (!trap.isTopmost()) return
  emit('close')
}

onMounted(() => {
  document.addEventListener('keydown', handleKeydown)
})

onBeforeUnmount(() => {
  document.removeEventListener('keydown', handleKeydown)
})
</script>

<template>
  <!-- 遮罩点击关闭是鼠标便利路径，键盘等效路径是 Esc（handleKeydown）。
       role="presentation" 表明遮罩本身不是可交互元素，避免被读屏当成按钮（#206）。
       例外：调用方可以在 @close 里拒绝关闭（如强制改密场景，契约 rest §1 要求必须
       先改密再登出），此时 Esc 与遮罩点击都不生效——这是设计行为，不是键盘缺陷。 -->
  <div
    v-if="open"
    class="modal-overlay is-open"
    role="presentation"
    @click="handleOverlayClick"
  >
    <div
      ref="dialogEl"
      class="ds-dialog"
      role="dialog"
      aria-modal="true"
      :aria-labelledby="titleId"
      tabindex="-1"
    >
      <div class="ds-dialog__head">
        <span :id="titleId" class="ds-dialog__title">{{ title }}</span>
        <button type="button" class="ds-dialog__close" aria-label="关闭" @click="emit('close')">
          <Icon name="Close" :size="16" />
        </button>
      </div>
      <div class="ds-dialog__body">
        <slot />
      </div>
      <div v-if="$slots.footer" class="ds-dialog__foot">
        <slot name="footer" />
      </div>
    </div>
  </div>
</template>
