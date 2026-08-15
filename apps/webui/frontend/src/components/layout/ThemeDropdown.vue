<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import { useTheme, type ThemeName } from '@/composables/useTheme'

const { currentTheme, setTheme } = useTheme()
const menuOpen = ref(false)

// 按钮图标跟随当前主题：light→Sun, dark→Moon, system→Switch
const buttonIcon = computed(() => {
  if (currentTheme.value === 'light') return 'Sun'
  if (currentTheme.value === 'dark') return 'Moon'
  return 'Switch'
})

function toggleMenu(): void {
  menuOpen.value = !menuOpen.value
}

function closeMenu(): void {
  menuOpen.value = false
}

function select(theme: ThemeName): void {
  setTheme(theme)
  closeMenu()
}

function handleOutsideClick(e: MouseEvent): void {
  const target = e.target as HTMLElement
  if (!target.closest('.theme-dropdown')) {
    closeMenu()
  }
}

onMounted(() => {
  document.addEventListener('click', handleOutsideClick)
})
onUnmounted(() => {
  document.removeEventListener('click', handleOutsideClick)
})
</script>

<template>
  <div class="filter-dropdown theme-dropdown" :style="{ position: 'relative' }">
    <button
      class="topbar__theme"
      :aria-label="'切换主题'"
      aria-haspopup="true"
      :aria-expanded="menuOpen"
      title="切换主题"
      @click.stop="toggleMenu"
    >
      <Icon :name="buttonIcon" :size="16" />
    </button>
    <div
      v-if="menuOpen"
      class="theme-dropdown__menu theme-dropdown__menu--open"
      role="menu"
      aria-label="主题选择"
    >
      <button
        class="theme-dropdown__item"
        :class="{ 'is-selected': currentTheme === 'light' }"
        type="button"
        role="menuitem"
        @click="select('light')"
      >
        <Icon name="Sun" :size="16" />
        <span class="theme-dropdown__label">浅色</span>
        <span class="theme-dropdown__check icon">
          <Icon name="check" :size="14" />
        </span>
      </button>
      <button
        class="theme-dropdown__item"
        :class="{ 'is-selected': currentTheme === 'dark' }"
        type="button"
        role="menuitem"
        @click="select('dark')"
      >
        <Icon name="Moon" :size="16" />
        <span class="theme-dropdown__label">深色</span>
        <span class="theme-dropdown__check icon">
          <Icon name="check" :size="14" />
        </span>
      </button>
      <button
        class="theme-dropdown__item"
        :class="{ 'is-selected': currentTheme === 'system' }"
        type="button"
        role="menuitem"
        @click="select('system')"
      >
        <Icon name="Switch" :size="16" />
        <span class="theme-dropdown__label">跟随系统</span>
        <span class="theme-dropdown__check icon">
          <Icon name="check" :size="14" />
        </span>
      </button>
    </div>
  </div>
</template>
