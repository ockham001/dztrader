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
    <!-- Skip link（#380）：键盘用户按下的第一个 Tab 就落在这里，Enter 直达正文，不必逐项
         穿过侧栏品牌 / 8 个导航项 / 折叠按钮 / 头像 / 设置 / 关于（/monitor 实测前 15 项全是壳）。
         放在壳层第一个位置：它是文档顺序里的第一站，`position: fixed` 让它在任何滚动位置
         都能从视口顶端出现（样式在 assets/css/layout.css，`.ds-*` 不得定义在 scoped 块）。 -->
    <a href="#main-content" class="ds-skip-link">跳到主内容</a>

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
      <!-- 正文地标（#380）：内容槽是全站唯一的 `<main>`，skip link 的锚点指向它。
           只包 `<slot />` 而不含 Topbar —— 这一层的边界就是 banner 数量的边界：
           `<header>` 只有在**不是** main/article/aside/nav/section 的后代时才是 banner 地标，
           所以壳层 `<header class="topbar">` 留在 main 外仍是唯一 banner，而各页面自带
           `<header>`（如 MonitorView 的标题组）落进 main 后自动不再是 banner，
           无需去改任何页面组件（#371 正在改 MonitorView.vue，本票与它零交集）。

           `tabindex="-1"`：锚点跳转后焦点真的落进正文（可编程聚焦、不进 Tab 序列）。
           没有它浏览器只滚动不移动焦点，下一个 Tab 又回到导航壳，skip link 等于没生效。 -->
      <main id="main-content" tabindex="-1" class="app-shell__main">
        <slot />
      </main>
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

/* 正文区（#380）：workspace 里除顶栏之外的剩余高度全给页面，页面组件自己
   （`.monitor-view` / `.logs-view` / … 都有 `flex: 1`）才能撑满并各自滚动。
   在 `<div>` 改成 `<main>` 之前，slot 的直接父级是 workspace 自身（flex 容器），
   现在多了一层 main —— 它必须自己吃下剩余空间并继续做 flex 容器，否则页面高度塌陷。

   这 4 条声明**不是可选装饰**，删掉任何一条都会全站内容被裁死（复核实测：
   删掉后 main 高 868→2349、页面内滚动失效、文档滚动高度仍 900）。jsdom 无布局，
   单测抓不到，故由 AppShellLandmarks.spec.ts 直接断言规则体文本守护。 */
.app-shell__main {
  flex: 1 1 auto;
  min-height: 0;
  display: flex;
  flex-direction: column;
  /* 键盘聚焦这块正文时（skip link 的落点），UA 会在**整块正文**外围画一圈 1px 描边
     （1440 下实测 1220×868 一圈）——那不是聚焦指示，是把整个视口框起来，读起来像选中了全页。
     退到 2px 内缩的位置：仍保留可见的聚焦反馈（不依赖 skip link 自身的可见性），
     又不会贴着内容边缘割裂版面。 */
  outline-offset: -2px;
  /* 窄屏 sticky 顶栏（32px，layout.css 的 .topbar）会压住锚点落点后的首行；
     留出这段滚动外边距，Enter 跳转后正文标题不被顶栏遮住。 */
  scroll-margin-top: var(--spacer-64);
}
</style>
