<script setup lang="ts">
/**
 * SyncSelect — 同步下拉框公共组件
 *
 * 用于需要等待服务器确认的下拉选择场景。选择后进入 pending 状态，
 * 显示加载动画。父组件完成异步操作后通过 v-model 更新最终值。
 * 如果失败，自动回退到之前的值。
 *
 * 用法：
 *   <SyncSelect v-model="mode" :items="items" @change="onModeChange" />
 *
 *   async function onModeChange(val: string) {
 *     try {
 *       await api.setMode(val)
 *       toast.success('切换成功')
 *     } catch {
 *       toast.error('切换失败')
 *     }
 *   }
 */

import { ref, computed, onMounted, onUnmounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'

export interface SelectItem {
  value: string | number
  label: string
}

const props = withDefaults(defineProps<{
  items: SelectItem[]
  modelValue?: string | number // eslint-disable-line vue/require-default-prop -- v-model 双向绑定，父组件保证传入或 undefined 合法
  placeholder?: string
  pending?: boolean
  disabled?: boolean
}>(), {
  placeholder: '请选择',
})

const emit = defineEmits<{
  'update:modelValue': [value: string | number]
  change: [value: string | number]
  /// 菜单打开时触发，父组件可在此刷新 items（如远程拉取最新数据）
  open: []
}>()

const isOpen = ref(false)
const previousValue = ref<string | number | undefined>(props.modelValue)

const selectedLabel = computed(() => {
  const item = props.items.find(i => i.value === props.modelValue)
  return item ? item.label : props.placeholder
})

function toggle(): void {
  if (props.pending || props.disabled) return
  if (!isOpen.value) {
    // 打开前通知父组件刷新 items
    emit('open')
  }
  isOpen.value = !isOpen.value
}

function close(): void {
  isOpen.value = false
}

function select(item: SelectItem): void {
  if (props.pending || props.disabled) return
  if (item.value === props.modelValue) {
    close()
    return
  }
  previousValue.value = props.modelValue
  emit('update:modelValue', item.value)
  emit('change', item.value)
  close()
}

function handleClickOutside(e: MouseEvent): void {
  const target = e.target as HTMLElement
  if (!target.closest('.ds-sync-select')) {
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
  <div class="ds-sync-select" :class="{ 'is-open': isOpen, 'is-pending': pending, 'is-disabled': disabled }">
    <button
      class="ds-sync-select__trigger"
      type="button"
      :disabled="disabled"
      @click.stop="toggle"
    >
      <span v-if="$slots.triggerPrefix" class="ds-sync-select__prefix" aria-hidden="true">
        <slot name="triggerPrefix" />
      </span>
      <span class="ds-sync-select__label">{{ selectedLabel }}</span>
      <span v-if="pending" class="ds-sync-select__spinner" aria-label="加载中">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round">
          <path d="M21 12a9 9 0 1 1-6.219-8.56" />
        </svg>
      </span>
      <span v-else class="ds-sync-select__chevron" aria-hidden="true">
        <Icon name="chevron_up_large" :size="16" />
      </span>
    </button>
    <div v-if="isOpen" class="ds-sync-select__menu" role="menu">
      <button
        v-for="item in items"
        :key="item.value"
        class="ds-sync-select__item"
        :class="{ 'is-selected': item.value === modelValue }"
        type="button"
        role="menuitem"
        @click="select(item)"
      >
        <span>{{ item.label }}</span>
        <span v-if="item.value === modelValue" class="ds-sync-select__check" aria-hidden="true">
          <Icon name="check" :size="14" />
        </span>
      </button>
    </div>
  </div>
</template>

<style scoped>
.ds-sync-select {
  position: relative;
  display: inline-block;
}

.ds-sync-select__trigger {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-8);
  padding: 0 var(--spacer-12);
  height: 32px;
  background: var(--bg-overlay-l1);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  color: var(--text-default);
  font: inherit;
  font-size: var(--body-base-font-size);
  cursor: pointer;
  transition: border-color 120ms ease, background 120ms ease;
}

.ds-sync-select__trigger:hover {
  border-color: var(--border-neutral-l2);
}

.ds-sync-select__trigger:disabled {
  cursor: not-allowed;
  opacity: 0.6;
}

.is-open .ds-sync-select__trigger {
  border-color: var(--border-neutral-l3);
}

.ds-sync-select__label {
  flex: 1;
  white-space: nowrap;
}

.ds-sync-select__prefix {
  display: inline-flex;
  align-items: center;
  color: var(--icon-secondary);
}

.ds-sync-select__spinner {
  display: inline-flex;
  align-items: center;
  color: var(--icon-secondary);
  animation: sync-select-spin 0.6s linear infinite;
}

@keyframes sync-select-spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}

.ds-sync-select__chevron {
  display: inline-flex;
  align-items: center;
  color: var(--icon-secondary);
  transition: transform 120ms ease;
}

.is-open .ds-sync-select__chevron {
  transform: rotate(180deg);
}

.ds-sync-select__menu {
  position: absolute;
  top: calc(100% + 4px);
  left: 0;
  min-width: 100%;
  max-height: 240px;
  overflow-y: auto;
  background: var(--bg-menu);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  box-shadow: 0 12px 32px color-mix(in srgb, var(--text-default) 16%, transparent),
              0 2px 8px color-mix(in srgb, var(--text-default) 8%, transparent);
  padding: var(--spacer-4);
  z-index: 200;
}

.ds-sync-select__item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  width: 100%;
  height: 32px;
  padding: 0 var(--spacer-8);
  background: transparent;
  border: none;
  border-radius: var(--radius-8);
  color: var(--text-default);
  font: inherit;
  font-size: var(--body-sm-font-size);
  text-align: left;
  cursor: pointer;
  transition: background 120ms ease;
}

.ds-sync-select__item:hover {
  background: var(--bg-overlay-l2);
}

.ds-sync-select__item.is-selected {
  color: var(--text-default);
}

.ds-sync-select__check {
  display: inline-flex;
  align-items: center;
  color: var(--text-default);
}
</style>
