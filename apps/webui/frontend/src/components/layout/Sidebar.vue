<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import { useAuthStore } from '@/stores/auth'
import { useSystemStore } from '@/stores/system'
import ChangePasswordModal from '@/components/shared/ChangePasswordModal.vue'
import { navItems } from '@/router/routes'

const visibleNavItems = computed(() => {
  if (auth.user?.role === 'admin') return navItems
  return navItems.filter(item => !item.adminOnly)
})

const props = defineProps<{
  collapsed: boolean
  mobileOpen: boolean
  isMobile: boolean
}>()

const emit = defineEmits<{
  toggleCollapse: []
  closeMobile: []
}>()

const router = useRouter()
const auth = useAuthStore()

const userDropdownOpen = ref(false)

// Change password modal state（表单/校验/契约错误码映射都在 ChangePasswordModal 内，
// 侧边栏只负责开合——避免两套校验与文案漂移）
const changePwdOpen = ref(false)

// About modal state
const aboutOpen = ref(false)

const displayName = computed(() => auth.user?.display_name || auth.user?.username || 'Admin')
const avatarLetter = computed(() => displayName.value.charAt(0).toUpperCase())

// 版本号唯一来源：后端 /api/system/info 的 version（工程版本，单一真相源 CMake project(VERSION)）。
// 前端不再硬编码版本（#207 A3）——版本升级只改 CMakeLists.txt，无需回改组件。
// 后端信息未到达/拉取失败时显示「未知」，不编造版本号。
const system = useSystemStore()
const aboutVersion = computed(() => system.version ? `Version ${system.version}` : 'Version 未知')

function toggleUserDropdown(): void {
  userDropdownOpen.value = !userDropdownOpen.value
}

// 点击外部关闭用户下拉菜单
const userSectionRef = ref<HTMLElement | null>(null)
// 触发按钮引用：菜单按 Esc 关闭后焦点必须回到它（#206）
const userSectionBtnRef = ref<HTMLButtonElement | null>(null)
function handleOutsideClick(e: MouseEvent): void {
  if (!userDropdownOpen.value) return
  if (userSectionRef.value && !userSectionRef.value.contains(e.target as Node)) {
    userDropdownOpen.value = false
  }
}

// 用户菜单键盘可达（#206）：菜单打开时按 Esc 关闭，并把焦点还给触发按钮。
// 监听挂在 document 上而不是模板 @keydown：user-section 容器本身不是可聚焦控件，
// 给它加 keydown 会被 a11y 规则判为「静态元素带交互处理器 / 交互角色必须可聚焦」；
// 这也与点击外部关闭共用同一条挂载路径。
function handleGlobalKeydown(e: KeyboardEvent): void {
  if (e.key !== 'Escape' || !userDropdownOpen.value) return
  userDropdownOpen.value = false
  userSectionBtnRef.value?.focus()
}

onMounted(() => {
  document.addEventListener('click', handleOutsideClick)
  document.addEventListener('keydown', handleGlobalKeydown)
})
onUnmounted(() => {
  document.removeEventListener('click', handleOutsideClick)
  document.removeEventListener('keydown', handleGlobalKeydown)
})

function openChangePassword(): void {
  changePwdOpen.value = true
  userDropdownOpen.value = false
}

function closeChangePassword(): void {
  changePwdOpen.value = false
}

function onBrandToggle(): void {
  emit('toggleCollapse')
}

function onNavClick(): void {
  if (props.isMobile) emit('closeMobile')
}

function logout(): void {
  if (auth.user?.username) {
    try { sessionStorage.setItem('last_username', auth.user.username) } catch { /* ignore */ }
  }
  auth.logout()
  router.push('/login')
}

function openAbout(): void {
  aboutOpen.value = true
}
</script>

<template>
  <aside
    class="sidebar-panel"
    :class="{
      'sidebar-collapsed': collapsed,
      'sidebar-mobile-open': mobileOpen,
    }"
    :style="{
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--spacer-16)',
      padding: '0 var(--spacer-12) var(--spacer-16)',
      background: 'var(--bg-base-default)',
    }"
  >
    <!-- 注：position/top/height/zIndex 已归 layout.css 的 .sidebar-panel
         （P5-T2：布局属性归 CSS，消除 media !important 覆盖 inline 的打架） -->
    <!-- Brand row -->
    <div
      class="sidebar-header"
      :style="{
        display: 'flex',
        alignItems: 'center',
        gap: 'var(--spacer-8)',
        height: '32px',
        padding: '0 var(--spacer-4)',
        borderBottom: '1px solid var(--bg-base-secondary)',
      }"
    >
      <button type="button"
        class="brand-toggle"
        :style="{
          display: 'flex',
          alignItems: 'center',
          gap: 'var(--spacer-8)',
          background: 'none',
          border: 'none',
          cursor: 'pointer',
          font: 'inherit',
          color: 'var(--text-default)',
          padding: 0,
          flex: 1,
          minWidth: 0,
        }"
        aria-label="折叠/展开侧边栏"
        @click="onBrandToggle"
      >
        <img
          src="/icons/builtin/DZ-Bolt.svg"
          alt=""
          :style="{ width: '20px', height: '20px', borderRadius: '4px', flexShrink: 0 }"
        />
        <span
          class="sidebar-brand-text"
          :style="{
            fontSize: 'var(--heading-xs-font-size)',
            lineHeight: 'var(--heading-xs-line-height)',
            fontWeight: 'var(--heading-xs-font-weight)',
            color: 'var(--text-default)',
            whiteSpace: 'nowrap',
            overflow: 'hidden',
            textOverflow: 'ellipsis',
          }"
        >DZTrader</span>
      </button>
    </div>

    <!-- Navigation -->
    <div
      class="sidebar-nav-scroll no-scrollbar"
      :style="{ flex: 1, overflowY: 'auto', minHeight: 0, display: 'flex', flexDirection: 'column', gap: 'var(--spacer-4)' }"
    >
      <nav :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-2)', flex: 1 }">
        <router-link
          v-for="item in visibleNavItems"
          :key="item.label"
          :to="item.route"
          class="nav-link"
          active-class="nav-link--active"
          :style="{
            display: 'flex',
            alignItems: 'center',
            gap: 'var(--spacer-10)',
            minHeight: '32px',
            padding: '0 var(--spacer-10)',
            borderRadius: 'var(--radius-8)',
            color: 'var(--text-secondary)',
            fontSize: 'var(--body-base-font-size)',
            lineHeight: 'var(--body-base-line-height)',
            textDecoration: 'none',
            whiteSpace: 'nowrap',
            overflow: 'hidden',
            textOverflow: 'ellipsis',
            borderLeft: '2px solid transparent',
            transition: 'background-color 120ms cubic-bezier(.2,0,0,1), color 120ms cubic-bezier(.2,0,0,1)',
          }"
          @click="onNavClick"
        >
          <span
            class="icon"
            :style="{ width: '16px', height: '16px', color: 'var(--icon-tertiary)', flexShrink: 0, display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }"
            aria-hidden="true"
          >
            <Icon :name="item.icon" :size="16" />
          </span>
          <span
            class="nav-label"
            :style="{ minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis' }"
          >{{ item.label }}</span>
          <!-- #207 C4：占位页功能在导航上先标出「开发中」，用户不必点进去才发现 -->
          <span
            v-if="item.wip"
            class="ds-tag ds-tag--warning"
            :style="{ marginLeft: 'auto', flexShrink: 0, fontSize: 'var(--body-xs-font-size)' }"
            data-test="nav-wip-tag"
            :title="`${item.label}功能开发中`"
          >开发中</span>
        </router-link>
      </nav>
    </div>

    <!-- User section -->
    <div
      ref="userSectionRef"
      :style="{
        position: 'relative',
        marginTop: 'auto',
        display: 'flex',
        flexDirection: 'column',
        gap: 'var(--spacer-4)',
        paddingTop: 'var(--spacer-8)',
        paddingBottom: 'var(--spacer-8)',
        borderTop: '1px solid var(--border-neutral-l1)',
      }"
    >
      <!-- User menu button -->
      <button type="button"
        ref="userSectionBtnRef"
        class="user-section-btn"
        aria-haspopup="menu"
        :aria-expanded="userDropdownOpen"
        :style="{
          display: 'flex',
          alignItems: 'center',
          gap: 'var(--spacer-8)',
          width: '100%',
          minHeight: '36px',
          padding: 'var(--spacer-6) var(--spacer-8)',
          borderRadius: 'var(--radius-8)',
          background: 'transparent',
          border: 'none',
          color: 'var(--text-default)',
          cursor: 'pointer',
          font: 'inherit',
          textAlign: 'left',
          transition: 'background-color 120ms cubic-bezier(.2,0,0,1)',
        }"
        @click.stop="toggleUserDropdown"
      >
        <span class="ds-avatar ds-avatar--sm sidebar-avatar" :style="{ flexShrink: 0 }">{{ avatarLetter }}</span>
        <span
          class="sidebar-brand-text"
          :style="{
            flex: 1,
            minWidth: 0,
            overflow: 'hidden',
            textOverflow: 'ellipsis',
            whiteSpace: 'nowrap',
            fontSize: 'var(--body-base-font-size)',
            lineHeight: 'var(--body-base-line-height)',
            color: 'var(--text-default)',
            fontWeight: 'var(--font-weight-medium)',
          }"
        >{{ displayName }}</span>
        <span
          class="icon sidebar-brand-text"
          :style="{ width: '12px', height: '12px', color: 'var(--icon-tertiary)', flexShrink: 0 }"
          aria-hidden="true"
        >
          <Icon name="chevron_up_large" :size="12" />
        </span>
      </button>

      <!-- User dropdown -->
      <div
        v-if="userDropdownOpen"
        class="user-dropdown"
        :style="{
          display: 'block',
          position: 'absolute',
          bottom: 'calc(100% + 4px)',
          left: 0,
          zIndex: 200,
          minWidth: '220px',
        }"
        @click.stop
      >
        <div
          class="ds-menu"
          role="menu"
          :style="{
            margin: 0,
            boxShadow: '0 12px 32px color-mix(in srgb, var(--text-default) 12%, transparent), 0 2px 8px color-mix(in srgb, var(--text-default) 8%, transparent)',
            background: 'var(--bg-menu)',
            border: '1px solid var(--border-neutral-l1)',
            borderRadius: 'var(--radius-8)',
            padding: 'var(--spacer-4)',
          }"
        >
          <!-- 菜单项用原生 <button>（而不是 div + @click）：Tab 可达 + Enter/Space 由浏览器
               原生派发 click，不需要也不允许再写 keydown.enter，否则会双触发。 -->
          <button
            type="button"
            role="menuitem"
            class="ds-menu__item user-menu__item"
            @click="openChangePassword"
          >
            <span class="icon" :style="{ width: '16px', height: '16px' }" aria-hidden="true"><Icon name="authentication" :size="16" /></span>
            修改密码
          </button>
          <div class="ds-menu__divider"></div>
          <button
            type="button"
            role="menuitem"
            class="ds-menu__item ds-menu__item--danger user-menu__item"
            @click="logout"
          >
            <span class="icon" :style="{ width: '16px', height: '16px' }" aria-hidden="true"><Icon name="logout" :size="16" /></span>
            登出
          </button>
        </div>
      </div>

      <!-- About button -->
      <button type="button"
        class="about-btn"
        :style="{
          display: 'flex',
          alignItems: 'center',
          gap: 'var(--spacer-8)',
          width: '100%',
          minHeight: '36px',
          padding: 'var(--spacer-6) var(--spacer-8)',
          borderRadius: 'var(--radius-8)',
          background: 'transparent',
          border: 'none',
          color: 'var(--text-default)',
          cursor: 'pointer',
          font: 'inherit',
          textAlign: 'left',
          transition: 'background-color 120ms cubic-bezier(.2,0,0,1)',
        }"
        @click="openAbout"
      >
        <span
          class="icon"
          :style="{ width: '24px', height: '24px', color: 'var(--icon-tertiary)', flexShrink: 0, display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }"
          aria-hidden="true"
        >
          <Icon name="Tips" :size="20" />
        </span>
        <span
          class="sidebar-brand-text"
          :style="{ flex: 1, fontSize: 'var(--body-base-font-size)', color: 'var(--text-secondary)' }"
        >关于</span>
      </button>
    </div>

    <!-- Change password modal（表单实现见 components/shared/ChangePasswordModal.vue） -->
    <ChangePasswordModal :open="changePwdOpen" @close="closeChangePassword" />

    <!-- About modal -->
    <Modal :open="aboutOpen" title="关于 DZTrader" @close="aboutOpen = false">
      <div :style="{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 'var(--spacer-16)', padding: 'var(--spacer-16) 0' }">
        <div :style="{ fontSize: '48px', fontWeight: 700, color: 'var(--brand-primary)' }">DZTrader</div>
        <div :style="{ fontSize: 'var(--body-lg-font-size)', color: 'var(--text-secondary)' }">DZTrader 量化交易系统</div>
        <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }" data-test="about-version">{{ aboutVersion }}</div>
        <div :style="{ width: '100%', height: '1px', background: 'var(--border-neutral-l1)', margin: 'var(--spacer-8) 0' }"></div>
        <div :style="{ width: '100%' }">
          <div :style="{ fontSize: 'var(--body-base-font-size)', fontWeight: 600, marginBottom: 'var(--spacer-8)', color: 'var(--text-default)' }">技术栈 / 开源致谢</div>
          <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-secondary)', lineHeight: 1.8 }">
            • Drogon/Trantor (HTTP/WebSocket)<br>
            • spdlog (日志)<br>
            • nlohmann/json (JSON)<br>
            • Boost (process/system)<br>
            • Vue 3 + ECharts (前端)<br>
            • SQLiteCpp (数据库)<br>
            • msgpack-c (序列化)
          </div>
        </div>
        <div :style="{ width: '100%', height: '1px', background: 'var(--border-neutral-l1)', margin: 'var(--spacer-8) 0' }"></div>
        <div :style="{ width: '100%' }">
          <div :style="{ fontSize: 'var(--body-base-font-size)', fontWeight: 600, marginBottom: 'var(--spacer-8)', color: 'var(--text-default)' }">免责声明</div>
          <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-secondary)', lineHeight: 1.6 }">
            本软件仅供学习和研究使用。使用本软件进行实盘交易所产生的一切盈亏由使用者自行承担，软件作者及贡献者不承担任何责任。
          </div>
        </div>
        <div :style="{ width: '100%', height: '1px', background: 'var(--border-neutral-l1)', margin: 'var(--spacer-8) 0' }"></div>
        <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }">© 2024-2026 DZTrader Contributors</div>
      </div>
      <template #footer>
        <button type="button" class="ds-btn ds-btn--primary" @click="aboutOpen = false">确定</button>
      </template>
    </Modal>
  </aside>
</template>

<style scoped>
/* 用户菜单项（#206）：两个菜单项由 div 改为原生 <button> 后，覆盖 button 的 UA 默认样式
   （button 自带灰底、居中文本、系统字体、可点区域不撑满），使视觉与原 div 版本一致。
   类名用本地前缀（.user-menu__item），不定义 .ds-* 模式库类（组件三层规则）；
   所有值与颜色只引用 token，不写字面量。 */
.user-menu__item {
  width: 100%;
  text-align: left;
  background: transparent;
  border: 0;
  font: inherit;
  color: inherit;
  cursor: pointer;

  /* font: inherit 会把 .ds-menu__item 已声明的排版 token 一并重置成继承值，
     这里显式写回同一组 token：两种 CSS 抽离顺序（dev 注入 / build 提取）下视觉都不变。 */
  font-family: var(--body-base-font-family);
  font-size: var(--body-base-font-size);
  font-weight: var(--body-base-font-weight);
  line-height: var(--body-base-line-height);
}

/* danger 项沿用 .ds-menu__item--danger 的语义色；上面 color: inherit 需要在这里重新声明 */
.user-menu__item.ds-menu__item--danger {
  color: var(--status-error-default);
}
</style>
