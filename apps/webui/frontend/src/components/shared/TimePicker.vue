<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'

const props = withDefaults(defineProps<{
  modelValue?: string
  disabled?: boolean
  placeholder?: string
  /** minute step (1/5/15/30). 默认 1 */
  step?: number
}>(), {
  modelValue: '',
  disabled: false,
  placeholder: '选择时间',
  step: 1,
})

const emit = defineEmits<{
  'update:modelValue': [value: string]
}>()

const isOpen = ref(false)
const triggerRef = ref<HTMLElement | null>(null)
const panelRef = ref<HTMLElement | null>(null)
const panelStyle = ref<Record<string, string>>({})
const hourListRef = ref<HTMLElement | null>(null)
const minuteListRef = ref<HTMLElement | null>(null)

const selectedHour = computed<number>(() => {
  if (!props.modelValue) return -1
  const parts = props.modelValue.split(':')
  const h = Number(parts[0])
  return isNaN(h) ? -1 : h
})

const selectedMinute = computed<number>(() => {
  if (!props.modelValue) return -1
  const parts = props.modelValue.split(':')
  const m = Number(parts[1])
  return isNaN(m) ? -1 : m
})

const displayLabel = computed(() => {
  if (!props.modelValue) return props.placeholder
  return props.modelValue
})

// hours: 0..23
const hours = computed(() => {
  const arr: number[] = []
  for (let h = 0; h < 24; h++) arr.push(h)
  return arr
})

// minutes: 0, step, 2*step, ... 59
const minutes = computed(() => {
  const arr: number[] = []
  for (let m = 0; m < 60; m += props.step) arr.push(m)
  return arr
})

function pad(n: number): string {
  return String(n).padStart(2, '0')
}

function selectHour(h: number): void {
  const m = selectedMinute.value >= 0 ? selectedMinute.value : 0
  emit('update:modelValue', `${pad(h)}:${pad(m)}`)
}

function selectMinute(m: number): void {
  const h = selectedHour.value >= 0 ? selectedHour.value : 0
  emit('update:modelValue', `${pad(h)}:${pad(m)}`)
}

// ===== Panel positioning (Teleport + fixed) =====
function updatePanelPosition(): void {
  if (!triggerRef.value) return
  const rect = triggerRef.value.getBoundingClientRect()
  const vw = window.innerWidth
  const vh = window.innerHeight
  const panelWidth = 200
  const panelHeight = 280

  let left = rect.left
  let top = rect.bottom + 4

  if (left + panelWidth > vw - 8) {
    left = Math.max(8, vw - panelWidth - 8)
  }
  if (top + panelHeight > vh - 8) {
    const upTop = rect.top - panelHeight - 4
    if (upTop > 8) top = upTop
  }

  panelStyle.value = {
    position: 'fixed',
    left: `${left}px`,
    top: `${top}px`,
    zIndex: '9999',
  }
}

function toggle(): void {
  if (props.disabled) return
  isOpen.value = !isOpen.value
  if (isOpen.value) {
    nextTick(() => {
      updatePanelPosition()
      scrollToSelected()
    })
  }
}

function close(): void {
  isOpen.value = false
}

function scrollToSelected(): void {
  // 把选中的 hour/minute 滚动到可视区中部
  if (hourListRef.value && selectedHour.value >= 0) {
    const el = hourListRef.value.querySelector<HTMLElement>(`[data-hour="${selectedHour.value}"]`)
    if (el) {
      const container = hourListRef.value
      container.scrollTop = Math.max(0, el.offsetTop - container.clientHeight / 2 + el.clientHeight / 2)
    }
  }
  if (minuteListRef.value && selectedMinute.value >= 0) {
    const el = minuteListRef.value.querySelector<HTMLElement>(`[data-minute="${selectedMinute.value}"]`)
    if (el) {
      const container = minuteListRef.value
      container.scrollTop = Math.max(0, el.offsetTop - container.clientHeight / 2 + el.clientHeight / 2)
    }
  }
}

function handleClickOutside(e: MouseEvent): void {
  if (!isOpen.value) return
  const target = e.target as HTMLElement
  if (triggerRef.value && triggerRef.value.contains(target)) return
  if (panelRef.value && panelRef.value.contains(target)) return
  close()
}

function onReposition(): void {
  if (isOpen.value) updatePanelPosition()
}

watch(() => props.modelValue, () => {
  if (isOpen.value) nextTick(scrollToSelected)
})

onMounted(() => {
  document.addEventListener('click', handleClickOutside)
  window.addEventListener('resize', onReposition)
  window.addEventListener('scroll', onReposition, true)
})
onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside)
  window.removeEventListener('resize', onReposition)
  window.removeEventListener('scroll', onReposition, true)
})
</script>

<template>
  <div class="ds-timepicker" :class="{ 'is-open': isOpen, 'is-disabled': props.disabled }">
    <button
      ref="triggerRef"
      class="ds-timepicker__trigger"
      type="button"
      :disabled="props.disabled"
      :aria-expanded="isOpen"
      @click="toggle()"
    >
      <span class="ds-timepicker__trigger-label">{{ displayLabel }}</span>
      <span class="icon ds-timepicker__trigger-icon" data-icon :style="{ width: '16px', height: '16px', '--icon-url': `url('/icons/builtin/time.svg')` }" aria-hidden="true"></span>
    </button>

    <Teleport to="body">
      <transition name="ds-timepicker-fade">
        <div
          v-if="isOpen"
          ref="panelRef"
          class="ds-timepicker__panel"
          :style="panelStyle"
          role="dialog"
          @click.stop
        >
          <div class="ds-timepicker__header">
            <span class="ds-timepicker__header-col">时</span>
            <span class="ds-timepicker__header-col">分</span>
          </div>
          <div class="ds-timepicker__body">
            <div ref="hourListRef" class="ds-timepicker__list">
              <button
                v-for="h in hours"
                :key="`h-${h}`"
                class="ds-timepicker__item"
                :class="{ 'is-selected': h === selectedHour }"
                type="button"
                :data-hour="h"
                @click="selectHour(h)"
              >{{ pad(h) }}</button>
            </div>
            <div ref="minuteListRef" class="ds-timepicker__list">
              <button
                v-for="m in minutes"
                :key="`m-${m}`"
                class="ds-timepicker__item"
                :class="{ 'is-selected': m === selectedMinute }"
                type="button"
                :data-minute="m"
                @click="selectMinute(m)"
              >{{ pad(m) }}</button>
            </div>
          </div>
        </div>
      </transition>
    </Teleport>
  </div>
</template>

<style scoped>
.ds-timepicker {
  position: relative;
  display: inline-block;
  width: 100%;
}

.ds-timepicker__trigger {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  width: 100%;
  height: 32px;
  padding: 0 var(--spacer-12);
  background: var(--bg-overlay-l1);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  color: var(--text-default);
  font: inherit;
  font-size: var(--body-base-font-size);
  font-variant-numeric: tabular-nums;
  cursor: pointer;
  transition: border-color 0.12s ease, background 0.12s ease;
}

.ds-timepicker__trigger:hover {
  border-color: var(--border-neutral-l2);
}

.ds-timepicker__trigger:focus {
  border-color: var(--border-neutral-l2);
  outline: none;
}

.ds-timepicker.is-open .ds-timepicker__trigger {
  border-color: var(--border-neutral-l3);
}

.ds-timepicker__trigger-label {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  text-align: left;
}

.ds-timepicker__trigger-icon {
  color: var(--icon-secondary);
  flex-shrink: 0;
  display: flex;
  align-items: center;
  transition: color 0.12s ease;
}

.ds-timepicker.is-open .ds-timepicker__trigger-icon {
  color: var(--icon-default);
}

.ds-timepicker.is-disabled .ds-timepicker__trigger {
  cursor: not-allowed;
  background: var(--bg-base-secondary);
  color: var(--text-disabled);
}

/* Panel */
.ds-timepicker__panel {
  width: 200px;
  background: var(--bg-menu);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-12);
  padding: var(--spacer-8);
  box-shadow: 0 12px 32px color-mix(in srgb, var(--text-default) 16%, transparent),
              0 2px 8px color-mix(in srgb, var(--text-default) 8%, transparent);
}

.ds-timepicker-fade-enter-active,
.ds-timepicker-fade-leave-active {
  transition: opacity 0.12s ease, transform 0.12s ease;
  transform-origin: top center;
}

.ds-timepicker-fade-enter-from,
.ds-timepicker-fade-leave-to {
  opacity: 0;
  transform: translateY(-4px) scale(0.98);
}

.ds-timepicker__header {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: var(--spacer-4);
  padding: 0 var(--spacer-4) var(--spacer-6);
  margin-bottom: var(--spacer-4);
  border-bottom: 1px solid var(--border-neutral-l1);
}

.ds-timepicker__header-col {
  text-align: center;
  font-size: var(--body-xs-font-size);
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-tertiary);
  letter-spacing: 0.04em;
}

.ds-timepicker__body {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: var(--spacer-4);
}

.ds-timepicker__list {
  max-height: 220px;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 2px;
  /* slim scrollbar */
  scrollbar-width: thin;
  scrollbar-color: var(--border-neutral-l2) transparent;
}

.ds-timepicker__list::-webkit-scrollbar {
  width: 4px;
}

.ds-timepicker__list::-webkit-scrollbar-thumb {
  background: var(--border-neutral-l2);
  border-radius: 2px;
}

.ds-timepicker__item {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 28px;
  padding: 0;
  border: 1px solid transparent;
  border-radius: var(--radius-6);
  background: transparent;
  color: var(--text-default);
  font: inherit;
  font-size: var(--body-sm-font-size);
  font-variant-numeric: tabular-nums;
  cursor: pointer;
  transition: background 0.12s ease, border-color 0.12s ease, color 0.12s ease;
}

.ds-timepicker__item:hover {
  background: var(--bg-overlay-l2);
}

.ds-timepicker__item.is-selected {
  background: var(--bg-brand);
  color: var(--text-onbrand);
  border-color: var(--bg-brand);
}
</style>
