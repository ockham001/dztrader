<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'

const props = withDefaults(defineProps<{
  modelValue?: string
  disabled?: boolean
  placeholder?: string
}>(), {
  modelValue: '',
  disabled: false,
  placeholder: '选择日期',
})

const emit = defineEmits<{
  'update:modelValue': [value: string]
}>()

const isOpen = ref(false)
const viewYear = ref(new Date().getFullYear())
const viewMonth = ref(new Date().getMonth())
const triggerRef = ref<HTMLElement | null>(null)
const panelStyle = ref<Record<string, string>>({})

const selectedDate = computed<Date | null>(() => {
  if (!props.modelValue) return null
  const parts = props.modelValue.split('-').map(Number)
  if (parts.length !== 3 || parts.some(isNaN)) return null
  return new Date(parts[0], parts[1] - 1, parts[2])
})

const displayLabel = computed(() => {
  if (!selectedDate.value) return props.placeholder
  return props.modelValue
})

const monthLabel = computed(() => {
  return `${viewYear.value}年${viewMonth.value + 1}月`
})

interface CalendarDay {
  date: Date
  day: number
  isCurrentMonth: boolean
  isToday: boolean
  isSelected: boolean
}

const weekdays = ['日', '一', '二', '三', '四', '五', '六']

const calendarDays = computed<CalendarDay[]>(() => {
  const year = viewYear.value
  const month = viewMonth.value
  const firstDay = new Date(year, month, 1)
  const startWeekday = firstDay.getDay()
  const lastDay = new Date(year, month + 1, 0)
  const daysInMonth = lastDay.getDate()
  const prevMonthLast = new Date(year, month, 0)
  const daysInPrevMonth = prevMonthLast.getDate()

  const now = new Date()
  now.setHours(0, 0, 0, 0)

  function makeDay(date: Date, isCurrentMonth: boolean): CalendarDay {
    return {
      date,
      day: date.getDate(),
      isCurrentMonth,
      isToday: sameDay(date, now),
      isSelected: selectedDate.value ? sameDay(date, selectedDate.value) : false,
    }
  }

  const days: CalendarDay[] = []

  for (let i = startWeekday - 1; i >= 0; i--) {
    days.push(makeDay(new Date(year, month - 1, daysInPrevMonth - i), false))
  }
  for (let d = 1; d <= daysInMonth; d++) {
    days.push(makeDay(new Date(year, month, d), true))
  }
  const remaining = 42 - days.length
  for (let d = 1; d <= remaining; d++) {
    days.push(makeDay(new Date(year, month + 1, d), false))
  }
  return days
})

function sameDay(a: Date, b: Date): boolean {
  return a.getFullYear() === b.getFullYear() &&
    a.getMonth() === b.getMonth() &&
    a.getDate() === b.getDate()
}

function formatDate(date: Date): string {
  const y = date.getFullYear()
  const m = String(date.getMonth() + 1).padStart(2, '0')
  const d = String(date.getDate()).padStart(2, '0')
  return `${y}-${m}-${d}`
}

// ===== Panel positioning (Teleport + fixed) =====
function updatePanelPosition(): void {
  if (!triggerRef.value) return
  const rect = triggerRef.value.getBoundingClientRect()
  const vw = window.innerWidth
  const vh = window.innerHeight
  const panelWidth = 280
  const panelHeight = 360

  let left = rect.left
  let top = rect.bottom + 4

  // 右溢出
  if (left + panelWidth > vw - 8) {
    left = Math.max(8, vw - panelWidth - 8)
  }
  // 底溢出 → 向上展开
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
  if (!isOpen.value && selectedDate.value) {
    viewYear.value = selectedDate.value.getFullYear()
    viewMonth.value = selectedDate.value.getMonth()
  }
  isOpen.value = !isOpen.value
  if (isOpen.value) {
    nextTick(() => updatePanelPosition())
  }
}

function close(): void {
  isOpen.value = false
}

function selectDate(day: CalendarDay): void {
  emit('update:modelValue', formatDate(day.date))
  close()
}

function prevMonth(): void {
  viewMonth.value--
  if (viewMonth.value < 0) {
    viewMonth.value = 11
    viewYear.value--
  }
}

function nextMonth(): void {
  viewMonth.value++
  if (viewMonth.value > 11) {
    viewMonth.value = 0
    viewYear.value++
  }
}

function handleClickOutside(e: MouseEvent): void {
  if (!isOpen.value) return
  const target = e.target as HTMLElement
  // trigger 内部点击由 toggle 处理
  if (triggerRef.value && triggerRef.value.contains(target)) return
  // panel 在 body 下
  if (target.closest('.ds-datepicker__panel')) return
  close()
}

function onReposition(): void {
  if (isOpen.value) updatePanelPosition()
}

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
  <div class="ds-datepicker" :class="{ 'is-open': isOpen, 'is-disabled': props.disabled }">
    <button
      ref="triggerRef"
      class="ds-datepicker__trigger"
      type="button"
      :disabled="props.disabled"
      :aria-expanded="isOpen"
      @click.stop="toggle()"
    >
      <span class="ds-datepicker__trigger-label">{{ displayLabel }}</span>
      <span class="icon ds-datepicker__trigger-icon" data-icon :style="{ width: '16px', height: '16px', '--icon-url': `url('/icons/builtin/calendar.svg')` }" aria-hidden="true"></span>
    </button>

    <!-- Panel: Teleport to body to escape parent overflow / stacking contexts -->
    <Teleport to="body">
      <transition name="ds-datepicker-fade">
        <div
          v-if="isOpen"
          class="ds-datepicker__panel"
          :style="panelStyle"
          role="dialog"
          @click.stop
        >
          <div class="ds-datepicker__header">
            <button class="ds-datepicker__nav" type="button" @click="prevMonth" aria-label="上个月">
              <span class="icon" data-icon :style="{ width: '16px', height: '16px', '--icon-url': `url('/icons/builtin/arrow_left_large.svg')` }" aria-hidden="true"></span>
            </button>
            <span class="ds-datepicker__month">{{ monthLabel }}</span>
            <button class="ds-datepicker__nav" type="button" @click="nextMonth" aria-label="下个月">
              <span class="icon" data-icon :style="{ width: '16px', height: '16px', '--icon-url': `url('/icons/builtin/arrow_right_large.svg')` }" aria-hidden="true"></span>
            </button>
          </div>
          <div class="ds-datepicker__weekdays">
            <span v-for="w in weekdays" :key="w" class="ds-datepicker__weekday">{{ w }}</span>
          </div>
          <div class="ds-datepicker__days">
            <button
              v-for="(day, i) in calendarDays"
              :key="i"
              class="ds-datepicker__day"
              :class="{
                'is-outside': !day.isCurrentMonth,
                'is-today': day.isToday,
                'is-selected': day.isSelected,
              }"
              type="button"
              @click="selectDate(day)"
            >{{ day.day }}</button>
          </div>
        </div>
      </transition>
    </Teleport>
  </div>
</template>
