<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import Sidebar from './Sidebar.vue'
import Topbar from './Topbar.vue'
import { useWebSocket } from '@/composables/wsClient'
import { useBreakpoint } from '@/composables/useBreakpoint'

// P5-T2 布局机制归一（CSS 主导）：grid 列模板/侧栏抽屉态全部在 layout.css 的
// @media 断点中定义（Tailwind 档）；本组件 JS 只做【行为分支】——
// useBreakpoint(matchMedia 事件驱动，跨断点瞬间触发，非 resize 逐像素) 判定
// 抽屉模式(<md) vs 桌面 rail(≥md)，跨断点时自动收起 mobile 抽屉。

const sidebarCollapsed = ref(false)
const mobileSidebarOpen = ref(false)

const { isMobile } = useBreakpoint()

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

// 跨断点（如 pad 竖↔横、窗口拖宽）回到桌面时收起 mobile 抽屉残留态；rail 折叠偏好保持
watch(isMobile, (mobile) => {
  if (!mobile) mobileSidebarOpen.value = false
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
</script>

<template>
  <!-- P5-T2: grid 列模板归 CSS（layout.css 按 md 断点切换 抽屉/rail），此处不再 inline -->
  <div
    class="app-shell"
    :class="{ 'sidebar-collapsed': sidebarCollapsed && !isMobile }"
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
