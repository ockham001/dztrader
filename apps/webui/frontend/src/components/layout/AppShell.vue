<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import Sidebar from './Sidebar.vue'
import Topbar from './Topbar.vue'
import { useWebSocket } from '@/composables/wsClient'

const EXPANDED_W = '220px'
const COLLAPSED_W = '56px'

const sidebarCollapsed = ref(false)
const mobileSidebarOpen = ref(false)
const isMobile = ref(false)

// WebSocket 连接状态 banner
const { connectionState } = useWebSocket()
const connBannerVisible = computed(() => {
  return connectionState.value === 'reconnecting' || connectionState.value === 'failed'
})
const connBannerType = computed<'reconnecting' | 'failed'>(() => {
  return connectionState.value === 'failed' ? 'failed' : 'reconnecting'
})
const connBannerText = computed(() => {
  if (connectionState.value === 'failed') return '服务器连接失败，请检查网络或刷新页面'
  if (connectionState.value === 'reconnecting') return '服务器连接断开，正在重连...'
  return ''
})

function checkMobile(): void {
  isMobile.value = window.innerWidth < 768
}

const gridTemplateColumns = computed(() => {
  if (isMobile.value) return '1fr'
  return `${sidebarCollapsed.value ? COLLAPSED_W : EXPANDED_W} minmax(0, 1fr)`
})

function toggleCollapse(): void {
  if (isMobile.value) return
  sidebarCollapsed.value = !sidebarCollapsed.value
}

function toggleMobile(): void {
  mobileSidebarOpen.value = !mobileSidebarOpen.value
}

function closeMobile(): void {
  mobileSidebarOpen.value = false
}

function handleResize(): void {
  checkMobile()
  if (isMobile.value) closeMobile()
}

onMounted(() => {
  checkMobile()
  window.addEventListener('resize', handleResize)
})
onUnmounted(() => {
  window.removeEventListener('resize', handleResize)
})
</script>

<template>
  <div
    class="app-shell"
    :class="{ 'sidebar-collapsed': sidebarCollapsed && !isMobile }"
    :style="{ display: 'grid', gridTemplateColumns, height: '100vh' }"
  >
    <!-- Mobile overlay -->
    <div
      v-if="isMobile"
      class="sidebar-overlay"
      :style="{
        display: mobileSidebarOpen ? 'block' : 'none',
        position: 'fixed',
        inset: '0',
        zIndex: 100,
        background: 'var(--bg-overlay-l4)',
      }"
      @click="closeMobile"
    />

    <Sidebar
      :collapsed="sidebarCollapsed && !isMobile"
      :mobile-open="mobileSidebarOpen"
      :is-mobile="isMobile"
      @toggle-collapse="toggleCollapse"
      @close-mobile="closeMobile"
    />

    <div
      class="workspace"
      :style="{
        minWidth: 0,
        background: 'var(--bg-base-default)',
        display: 'flex',
        flexDirection: 'column',
        height: '100%',
        overflow: 'hidden',
      }"
    >
      <Topbar @toggle-mobile="toggleMobile" />
      <!-- 服务器连接状态 banner（WS 断开/重连时显示，把内容顶下去）-->
      <div
        v-if="connBannerVisible"
        class="ds-conn-banner"
        :class="`ds-conn-banner--${connBannerType}`"
      >
        <span class="ds-conn-banner__dot"></span>
        <span>{{ connBannerText }}</span>
      </div>
      <slot />
    </div>
  </div>
</template>
