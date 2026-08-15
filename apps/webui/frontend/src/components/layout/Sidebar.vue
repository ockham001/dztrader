<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useRouter } from 'vue-router'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import { authApi, type ChangePasswordResponse } from '@/api/auth'
import { ApiError } from '@/api/client'
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
const toast = useToastStore()

const userDropdownOpen = ref(false)

// Change password modal state
const changePwdOpen = ref(false)
const newPassword = ref('')
const confirmPassword = ref('')
const changePwdError = ref<string | null>(null)
const changePwdLoading = ref(false)

// About modal state
const aboutOpen = ref(false)

const displayName = computed(() => auth.user?.display_name || auth.user?.username || 'Admin')
const avatarLetter = computed(() => displayName.value.charAt(0).toUpperCase())

function toggleUserDropdown(): void {
  userDropdownOpen.value = !userDropdownOpen.value
}

// 点击外部关闭用户下拉菜单
const userSectionRef = ref<HTMLElement | null>(null)
function handleOutsideClick(e: MouseEvent): void {
  if (!userDropdownOpen.value) return
  if (userSectionRef.value && !userSectionRef.value.contains(e.target as Node)) {
    userDropdownOpen.value = false
  }
}

onMounted(() => {
  document.addEventListener('click', handleOutsideClick)
})
onUnmounted(() => {
  document.removeEventListener('click', handleOutsideClick)
})

function openChangePassword(): void {
  newPassword.value = ''
  confirmPassword.value = ''
  changePwdError.value = null
  changePwdLoading.value = false
  changePwdOpen.value = true
  userDropdownOpen.value = false
}

async function submitChangePassword(): Promise<void> {
  changePwdError.value = null

  if (!newPassword.value) {
    changePwdError.value = '请输入新密码'
    return
  }
  if (newPassword.value !== confirmPassword.value) {
    changePwdError.value = '两次输入的密码不一致'
    return
  }

  changePwdLoading.value = true
  try {
    const resp: ChangePasswordResponse = await authApi.changePassword(newPassword.value)
    if (resp?.ok) {
      toast.success('密码修改成功')
      changePwdOpen.value = false
    } else {
      changePwdError.value = '密码修改失败'
      toast.error('密码修改失败')
    }
  } catch (e: unknown) {
    let msg = '密码修改失败'
    if (e instanceof ApiError) {
      msg = e.body?.error || e.message
    } else if (e instanceof Error) {
      msg = e.message
    }
    changePwdError.value = msg
    toast.error(msg)
  } finally {
    changePwdLoading.value = false
  }
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
      position: 'sticky',
      top: 0,
      height: '100vh',
      display: 'flex',
      flexDirection: 'column',
      gap: 'var(--spacer-16)',
      padding: '0 var(--spacer-12) var(--spacer-16)',
      background: 'var(--bg-base-default)',
      zIndex: 300,
    }"
  >
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
      <button
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
      <button
        class="user-section-btn"
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
          :style="{
            margin: 0,
            boxShadow: '0 12px 32px color-mix(in srgb, var(--text-default) 12%, transparent), 0 2px 8px color-mix(in srgb, var(--text-default) 8%, transparent)',
            background: 'var(--bg-menu)',
            border: '1px solid var(--border-neutral-l1)',
            borderRadius: 'var(--radius-8)',
            padding: 'var(--spacer-4)',
          }"
        >
          <div class="ds-menu__item" :style="{ cursor: 'pointer' }" @click="openChangePassword">
            <span class="icon" :style="{ width: '16px', height: '16px' }" aria-hidden="true"><Icon name="authentication" :size="16" /></span>
            修改密码
          </div>
          <div class="ds-menu__divider"></div>
          <div
            class="ds-menu__item ds-menu__item--danger"
            :style="{ textDecoration: 'none', cursor: 'pointer' }"
            @click="logout"
          >
            <span class="icon" :style="{ width: '16px', height: '16px' }" aria-hidden="true"><Icon name="download2" :size="16" /></span>
            登出
          </div>
        </div>
      </div>

      <!-- About button -->
      <button
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

    <!-- Change password modal -->
    <Modal :open="changePwdOpen" title="修改密码" @close="changePwdOpen = false">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: 'var(--spacer-12)' }">
        <div class="ds-input">
          <input v-model="newPassword" type="password" placeholder="新密码" />
        </div>
        <div class="ds-input">
          <input v-model="confirmPassword" type="password" placeholder="确认新密码" />
        </div>
        <div
          v-if="changePwdError"
          :style="{ color: 'var(--status-error-default)', fontSize: 'var(--body-sm-font-size)' }"
        >
          {{ changePwdError }}
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" @click="changePwdOpen = false">取消</button>
        <button class="ds-btn ds-btn--primary" :disabled="changePwdLoading" @click="submitChangePassword">
          {{ changePwdLoading ? '提交中...' : '确认' }}
        </button>
      </template>
    </Modal>

    <!-- About modal -->
    <Modal :open="aboutOpen" title="关于 DZTrader" @close="aboutOpen = false">
      <div :style="{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 'var(--spacer-16)', padding: 'var(--spacer-16) 0' }">
        <div :style="{ fontSize: '48px', fontWeight: 700, color: 'var(--brand-primary)' }">DZTrader</div>
        <div :style="{ fontSize: 'var(--body-lg-font-size)', color: 'var(--text-secondary)' }">DZTrader 量化交易系统</div>
        <div :style="{ fontSize: 'var(--body-sm-font-size)', color: 'var(--text-tertiary)' }">Version 0.0.1</div>
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
        <button class="ds-btn ds-btn--primary" @click="aboutOpen = false">确定</button>
      </template>
    </Modal>
  </aside>
</template>
