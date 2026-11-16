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
    <!-- Mobile overlay：点击关闭抽屉是鼠标便利路径，键盘等效路径是顶栏抽屉按钮与抽屉内导航；
         role="presentation" 表明遮罩本身不是可交互控件（#206） -->
    <!-- dz-allow-inline-style: display 是 JS 状态驱动的开/关（静态定位与层叠已进 .app-shell__overlay）。
         未改成修饰类的理由：AppShell.spec.ts 把「抽屉开/关」直接断言成 element.style.display 的
         内联值（3 处），改类要连带改该 spec 的断言口径 —— 那是行为契约的变更，不属于本票
         「纯搬运不改行为」的范围，留给后续票。 -->
    <div
      v-if="isMobile"
      class="sidebar-overlay app-shell__overlay"
      role="presentation"
      :style="{ display: mobileSidebarOpen ? 'block' : 'none' }"
      @click="closeMobile"
    />

    <Sidebar
      :collapsed="sidebarCollapsed && !isMobile"
      :mobile-open="mobileSidebarOpen"
      :is-mobile="isMobile"
      @toggle-collapse="toggleCollapse"
      @close-mobile="closeMobile"
    />

    <div class="workspace app-shell__workspace">
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

<style scoped>
/* #346：原模板 :style 的静态声明归位。display 留在内联（mobileSidebarOpen 逐实例动态，
   AppShell.spec.ts 断言 element.style.display）；其余定位/层叠/底色均静态，进类。
   窄屏（≤1024px 或 ≤600px 高）时 dashboard.css 的
   `.app-shell > .workspace { height:auto; overflow:visible }` 会压过本类的同名声明：
   两者都是 (0,2,0)（scoped 的 `[data-v-*]` 与 `.app-shell` + `.workspace` 各算一个类），
   平局由**源码顺序**决定 —— dashboard.css 在 main.ts 里最后导入，故它胜。
   （#349 去掉了那里的 !important；行为与原内联被 !important 压住时一致，快照无差异。） */
.app-shell__overlay {
  position: fixed;
  inset: 0;
  z-index: 100;
  background: var(--bg-overlay-l4);
}

.app-shell__workspace {
  min-width: 0;
  background: var(--bg-base-default);
  display: flex;
  flex-direction: column;
  height: 100%;
  overflow: hidden;
}
</style>
