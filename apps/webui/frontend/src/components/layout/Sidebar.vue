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
//
// 作用域：只在「焦点仍落在菜单内或触发按钮上」时响应。否则页面别处按 Esc（例如
// 关掉某个对话框）会顺带把本菜单关掉并无故抢走焦点——对话框自己也要用 Esc，
// 两层同时响应会让键盘用户无法预期到底关掉了哪一层。
function handleGlobalKeydown(e: KeyboardEvent): void {
  if (e.key !== 'Escape' || !userDropdownOpen.value) return
  const active = document.activeElement
  const menuEl = userSectionRef.value?.querySelector('[role="menu"]')
  const inMenu = active instanceof Node
    && (Boolean(menuEl?.contains(active)) || active === userSectionBtnRef.value)
  if (!inMenu) return
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

// 打开改密弹窗：先把焦点从菜单项（即将被 v-if 移除）移回触发按钮，再关菜单。
// 顺序很重要——焦点陷阱在 open=true 时记录 document.activeElement 作为还原目标，
// 若此时焦点还挂在已被移除的菜单项上，关闭弹窗后无法还原（isConnected 保护会跳过），
// 焦点会掉到 body。这是 useFocusTrap 文档里写明的调用方约定。
function openChangePassword(): void {
  userSectionBtnRef.value?.focus()
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
  >
    <!-- 注：position/top/height/zIndex 已归 layout.css 的 .sidebar-panel
         （P5-T2：布局属性归 CSS，消除 media !important 覆盖 inline 的打架） -->
    <!-- Brand row -->
    <div class="sidebar-header">
      <button type="button"
        class="brand-toggle"
        aria-label="折叠/展开侧边栏"
        @click="onBrandToggle"
      >
        <img
          src="/icons/builtin/DZ-Bolt.svg"
          alt=""
          class="sidebar-brand-logo"
        />
        <span
          class="sidebar-brand-text sidebar-brand-title"
        >DZTrader</span>
      </button>
    </div>

    <!-- Navigation -->
    <div
      class="sidebar-nav-scroll no-scrollbar"
    >
      <nav class="sidebar-nav">
        <router-link
          v-for="item in visibleNavItems"
          :key="item.label"
          :to="item.route"
          class="nav-link"
          active-class="nav-link--active"
          @click="onNavClick"
        >
          <span
            class="icon"
            aria-hidden="true"
          >
            <Icon :name="item.icon" :size="16" />
          </span>
          <span
            class="nav-label"
          >{{ item.label }}</span>
          <!-- #207 C4：占位页功能在导航上先标出「开发中」，用户不必点进去才发现 -->
          <span
            v-if="item.wip"
            class="ds-tag ds-tag--warning sidebar-wip-tag"
            data-test="nav-wip-tag"
            :title="`${item.label}功能开发中`"
          >开发中</span>
        </router-link>
      </nav>
    </div>

    <!-- User section -->
    <div
      ref="userSectionRef"
      class="sidebar-user"
    >
      <!-- User menu button -->
      <button type="button"
        ref="userSectionBtnRef"
        class="user-section-btn"
        aria-haspopup="menu"
        :aria-expanded="userDropdownOpen"
        @click.stop="toggleUserDropdown"
      >
        <span class="ds-avatar ds-avatar--sm sidebar-avatar">{{ avatarLetter }}</span>
        <span
          class="sidebar-brand-text sidebar-user-name"
        >{{ displayName }}</span>
        <span
          class="icon sidebar-brand-text sidebar-chevron"
          aria-hidden="true"
        >
          <Icon name="chevron_up_large" :size="12" />
        </span>
      </button>

      <!-- User dropdown -->
      <div
        v-if="userDropdownOpen"
        class="user-dropdown"
        @click.stop
      >
        <div
          class="ds-menu sidebar-user-menu"
          role="menu"
        >
          <!-- 菜单项用原生 <button>（而不是 div + @click）：Tab 可达 + Enter/Space 由浏览器
               原生派发 click，不需要也不允许再写 keydown.enter，否则会双触发。 -->
          <button
            type="button"
            role="menuitem"
            class="ds-menu__item user-menu__item"
            @click="openChangePassword"
          >
            <span class="icon sidebar-user-menu-icon" aria-hidden="true"><Icon name="authentication" :size="16" /></span>
            修改密码
          </button>
          <!-- role="menu" 的允许子角色只有 menuitem / menuitemradio / menuitemcheckbox /
               group / separator；裸 div 会让读屏把分隔线当作菜单项读出来 -->
          <div class="ds-menu__divider" role="separator"></div>
          <button
            type="button"
            role="menuitem"
            class="ds-menu__item ds-menu__item--danger user-menu__item"
            @click="logout"
          >
            <span class="icon sidebar-user-menu-icon" aria-hidden="true"><Icon name="logout" :size="16" /></span>
            登出
          </button>
        </div>
      </div>

      <!-- About button -->
      <button type="button"
        class="about-btn"
        @click="openAbout"
      >
        <span
          class="icon"
          aria-hidden="true"
        >
          <Icon name="Tips" :size="20" />
        </span>
        <span
          class="sidebar-brand-text sidebar-about-label"
        >关于</span>
      </button>
    </div>

    <!-- Change password modal（表单实现见 components/shared/ChangePasswordModal.vue） -->
    <ChangePasswordModal :open="changePwdOpen" @close="closeChangePassword" />

    <!-- About modal -->
    <Modal :open="aboutOpen" title="关于 DZTrader" @close="aboutOpen = false">
      <div class="sidebar-about">
        <div class="sidebar-about-title">DZTrader</div>
        <div class="sidebar-about-subtitle">DZTrader 量化交易系统</div>
        <div class="sidebar-about-version" data-test="about-version">{{ aboutVersion }}</div>
        <div class="sidebar-about-divider"></div>
        <div class="sidebar-about-section">
          <div class="sidebar-about-heading">技术栈 / 开源致谢</div>
          <div class="sidebar-about-list">
            • Drogon/Trantor (HTTP/WebSocket)<br>
            • spdlog (日志)<br>
            • nlohmann/json (JSON)<br>
            • Boost (process/system)<br>
            • Vue 3 + ECharts (前端)<br>
            • SQLiteCpp (数据库)<br>
            • msgpack-c (序列化)
          </div>
        </div>
        <div class="sidebar-about-divider"></div>
        <div class="sidebar-about-section">
          <div class="sidebar-about-heading">免责声明</div>
          <div class="sidebar-about-disclaimer">
            本软件仅供学习和研究使用。使用本软件进行实盘交易所产生的一切盈亏由使用者自行承担，软件作者及贡献者不承担任何责任。
          </div>
        </div>
        <div class="sidebar-about-divider"></div>
        <div class="sidebar-about-copyright">© 2024-2026 DZTrader Contributors</div>
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

/* ═══ #346：模板里的静态内联样式归位（只搬运、不改值；验收 = 计算样式零变化）═══
   特异性说明：原内联样式压过 layout.css 里所有非 !important 的形态覆盖
   （.sidebar-collapsed .* 、@media (max-width: 767.98px) 的 .sidebar-panel .*）。
   提成类后必须保持同等压制力，否则折叠态/移动端的计算值会变 —— 故对存在竞争规则的元素
   多套一层组件祖先类（.sidebar-panel .nav-link / .sidebar-user .user-dropdown 等），
   使其特异性严格高于上述 0,2,0 规则。原先就被 !important 压住的内联声明照原样搬运，不顺手修。 */

/* 面板容器（position/top/height/z-index 仍归 assets/css/layout.css，见模板注释） */
.sidebar-panel {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
  padding: 0 var(--spacer-12) var(--spacer-16);
  background: var(--bg-base-default);
}

/* 品牌行 */
.sidebar-header {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  height: 32px;
  padding: 0 var(--spacer-4);
  border-bottom: 1px solid var(--bg-base-secondary);
}

.brand-toggle {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  background: none;
  border: none;
  cursor: pointer;
  font: inherit;
  color: var(--text-default);
  padding: 0;
  flex: 1;
  min-width: 0;
}

.sidebar-brand-logo {
  width: var(--icon-size-20);
  height: var(--icon-size-20);
  border-radius: var(--radius-4);
  flex-shrink: 0;
}

.sidebar-brand-title {
  font-size: var(--heading-xs-font-size);
  line-height: var(--heading-xs-line-height);
  font-weight: var(--heading-xs-font-weight);
  color: var(--text-default);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* 导航容器 */
.sidebar-nav-scroll {
  flex: 1;
  overflow-y: auto;
  min-height: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
}

.sidebar-nav {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  flex: 1;
}

/* 导航项：桌面形态。折叠态（.sidebar-collapsed .nav-link）与 <md 的 layout.css 规则
   都是非 !important，原先被内联压住，这里继续压住（否则折叠/移动端间距会变）。 */
.sidebar-panel .nav-link {
  display: flex;
  align-items: center;
  gap: var(--spacer-10);
  min-height: 32px;
  padding: 0 var(--spacer-10);
  border-radius: var(--radius-8);
  color: var(--text-secondary);
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  text-decoration: none;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  border-left: 2px solid transparent;
  transition: background-color 120ms cubic-bezier(.2,0,0,1), color 120ms cubic-bezier(.2,0,0,1);
}

.sidebar-panel .nav-link .icon {
  width: var(--icon-size-16);
  height: var(--icon-size-16);
  color: var(--icon-tertiary);
  flex-shrink: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.sidebar-panel .nav-label {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sidebar-wip-tag {
  margin-left: auto;
  flex-shrink: 0;
  font-size: var(--body-xs-font-size);
}

/* 底部用户区容器（顶部细线 + margin-top: auto 把用户区推到底部） */
.sidebar-user {
  position: relative;
  margin-top: auto;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  padding-top: var(--spacer-8);
  padding-bottom: var(--spacer-8);
  border-top: 1px solid var(--border-neutral-l1);
}

/* 用户区按钮：折叠态与 <md 的 layout.css 规则非 !important，原先被内联压住，继续压住 */
.sidebar-panel .user-section-btn {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  width: 100%;
  min-height: 36px;
  padding: var(--spacer-6) var(--spacer-8);
  border-radius: var(--radius-8);
  background: transparent;
  border: none;
  color: var(--text-default);
  cursor: pointer;
  font: inherit;
  text-align: left;
  transition: background-color 120ms cubic-bezier(.2,0,0,1);
}

.sidebar-avatar {
  flex-shrink: 0;
}

.sidebar-user-name {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  color: var(--text-default);
  font-weight: var(--font-weight-medium);
}

.sidebar-chevron {
  width: var(--icon-size-12);
  height: var(--icon-size-12);
  color: var(--icon-tertiary);
  flex-shrink: 0;
}

/* 用户下拉：折叠态（.sidebar-collapsed .user-dropdown）与 <md 的 layout.css 规则非 !important，
   原先被内联压住，继续压住（min-width 220px 等桌面值保持不变）。 */
.sidebar-user .user-dropdown {
  display: block;
  position: absolute;
  bottom: calc(100% + var(--spacer-4));
  left: 0;
  z-index: 200;
  min-width: 220px;
}

.sidebar-user-menu {
  margin: 0;
  box-shadow: 0 12px 32px color-mix(in srgb, var(--text-default) 12%, transparent), 0 2px 8px color-mix(in srgb, var(--text-default) 8%, transparent);
  background: var(--bg-menu);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  padding: var(--spacer-4);
}

.sidebar-user-menu-icon {
  width: var(--icon-size-16);
  height: var(--icon-size-16);
}

/* 关于按钮：折叠态与 <md 的 layout.css 规则非 !important，原先被内联压住，继续压住 */
.sidebar-panel .about-btn {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  width: 100%;
  min-height: 36px;
  padding: var(--spacer-6) var(--spacer-8);
  border-radius: var(--radius-8);
  background: transparent;
  border: none;
  color: var(--text-default);
  cursor: pointer;
  font: inherit;
  text-align: left;
  transition: background-color 120ms cubic-bezier(.2,0,0,1);
}

.sidebar-panel .about-btn .icon {
  width: var(--icon-size-24);
  height: var(--icon-size-24);
  color: var(--icon-tertiary);
  flex-shrink: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.sidebar-about-label {
  flex: 1;
  font-size: var(--body-base-font-size);
  color: var(--text-secondary);
}

/* 「关于」弹窗内容 */
.sidebar-about {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacer-16);
  padding: var(--spacer-16) 0;
}

.sidebar-about-title {
  font-size: var(--font-size-48);
  font-weight: 700;
  color: var(--brand-primary);
}

.sidebar-about-subtitle {
  font-size: var(--body-lg-font-size);
  color: var(--text-secondary);
}

.sidebar-about-version {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}

.sidebar-about-divider {
  width: 100%;
  height: 1px;
  background: var(--border-neutral-l1);
  margin: var(--spacer-8) 0;
}

.sidebar-about-section {
  width: 100%;
}

.sidebar-about-heading {
  font-size: var(--body-base-font-size);
  font-weight: var(--font-weight-strong);
  margin-bottom: var(--spacer-8);
  color: var(--text-default);
}

.sidebar-about-list {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  line-height: 1.8;
}

.sidebar-about-disclaimer {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  line-height: 1.6;
}

.sidebar-about-copyright {
  font-size: var(--body-sm-font-size);
  color: var(--text-tertiary);
}
</style>
