<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'

export interface DropdownItem {
  value: string | number
  label: string
  /** Optional status dot color (any CSS color, e.g. 'var(--status-error-default)') */
  dot?: string
}

const props = withDefaults(defineProps<{
  items: DropdownItem[]
  modelValue?: string | number // eslint-disable-line vue/require-default-prop -- v-model 双向绑定，父组件保证传入或 undefined 合法
  placeholder?: string
  disabled?: boolean
  /** When set, overrides selectedLabel (e.g. "切换中…" during pending) */
  loadingText?: string
}>(), {
  placeholder: '请选择',
  disabled: false,
  loadingText: '',
})

const emit = defineEmits<{
  'update:modelValue': [value: string | number]
}>()

const isOpen = ref(false)

const selectedItem = computed(() => {
  return props.items.find(i => i.value === props.modelValue) ?? null
})

const selectedLabel = computed(() => {
  // loadingText 优先 (pending 时强制显示切换中文案, 不显示旧选中)
  if (props.loadingText) return props.loadingText
  return selectedItem.value ? selectedItem.value.label : props.placeholder
})

function toggle(): void {
  isOpen.value = !isOpen.value
}

function close(): void {
  isOpen.value = false
}

function select(item: DropdownItem): void {
  if (item.value === props.modelValue) {
    close()
    return
  }
  emit('update:modelValue', item.value)
  close()
}

function handleClickOutside(e: MouseEvent): void {
  const target = e.target as HTMLElement
  if (!target.closest('.ds-dropdown')) {
    close()
  }
}

onMounted(() => {
  document.addEventListener('click', handleClickOutside)
})
onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside)
})
</script>

<template>
  <div class="ds-dropdown" :class="{ 'is-open': isOpen, 'is-disabled': props.disabled }">
    <button class="ds-dropdown__trigger" type="button" :disabled="props.disabled" @click.stop="!props.disabled && toggle()">
      <span
        v-if="selectedItem?.dot"
        class="ds-dropdown__trigger-dot"
        :style="{ background: selectedItem.dot }"
        aria-hidden="true"
      ></span>
      <span class="ds-dropdown__trigger-label">{{ selectedLabel }}</span>
      <span class="icon ds-dropdown__trigger-icon" data-icon :style="{ width: '16px', height: '16px', '--icon-url': `url('/icons/builtin/chevron_up_large.svg')` }" aria-hidden="true"></span>
    </button>
    <div class="ds-dropdown__menu" role="menu">
      <button
        v-for="item in items"
        :key="item.value"
        class="ds-dropdown__item"
        :class="{ 'is-selected': item.value === modelValue }"
        type="button"
        role="menuitem"
        @click="select(item)"
      >
        <span
          v-if="item.dot"
          class="ds-dropdown__item-dot"
          :style="{ background: item.dot }"
          aria-hidden="true"
        ></span>
        <span class="ds-dropdown__item-label">{{ item.label }}</span>
        <span class="icon ds-dropdown__item-check" data-icon :style="{ width: '14px', height: '14px', '--icon-url': `url('/icons/builtin/check.svg')` }" aria-hidden="true"></span>
      </button>
    </div>
  </div>
</template>
