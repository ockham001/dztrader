<script setup lang="ts">
import { onMounted, onUnmounted, watch } from 'vue'
import Icon from './Icon.vue'

const props = defineProps<{
  open: boolean
  title: string
}>()

const emit = defineEmits<{
  close: []
}>()

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

watch(() => props.open, (isOpen) => {
  if (isOpen) {
    document.body.style.overflow = 'hidden'
  } else {
    document.body.style.overflow = ''
  }
})

onMounted(() => {
  document.addEventListener('keydown', handleKeydown)
})
onUnmounted(() => {
  document.removeEventListener('keydown', handleKeydown)
  document.body.style.overflow = ''
})
</script>

<template>
  <div
    v-if="open"
    class="modal-overlay is-open"
    @click="handleOverlayClick"
  >
    <div class="ds-dialog">
      <div class="ds-dialog__head">
        <span class="ds-dialog__title">{{ title }}</span>
        <button class="ds-dialog__close" @click="emit('close')">
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
