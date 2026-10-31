<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, useId, watch } from 'vue'
import Icon from './Icon.vue'
import { useFocusTrap } from '@/composables/useFocusTrap'

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

function handleKeydown(e: KeyboardEvent): void {
  if (e.key === 'Escape' && props.open) {
    emit('close')
  }
}

function handleOverlayClick(e: MouseEvent): void {
  if (e.target === e.currentTarget) {
    emit('close')
  }
}

// 焦点管理（#206 A）：打开时焦点移入对话框、Tab / Shift+Tab 在框内循环、关闭还原到触发元素
useFocusTrap({ active: computed(() => props.open), container: dialogEl })

// body overflow 锁定：打开时禁背景滚动，关闭还原（原有行为，勿改坏）。
// immediate 兜住「挂载即已打开」的场景（父组件把 open 一开始就置 true），
// 只监听变化会让首帧漏锁。
watch(() => props.open, (isOpen) => {
  document.body.style.overflow = isOpen ? 'hidden' : ''
}, { immediate: true })

onMounted(() => {
  document.addEventListener('keydown', handleKeydown)
})

onBeforeUnmount(() => {
  document.removeEventListener('keydown', handleKeydown)
  // 打开状态下被卸载（如路由切换）必须放开滚动锁，否则整站无法滚动
  document.body.style.overflow = ''
})
</script>

<template>
  <!-- 遮罩点击关闭是鼠标便利路径，键盘等效路径是 Esc（handleKeydown）。
       role="presentation" 表明遮罩本身不是可交互元素，避免被读屏当成按钮（#206） -->
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
