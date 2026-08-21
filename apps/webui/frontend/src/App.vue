<script setup lang="ts">
import { watch, onMounted, onUnmounted } from 'vue'
import { useRoute } from 'vue-router'
import ToastContainer from '@/components/shared/ToastContainer.vue'
import AppShell from '@/components/layout/AppShell.vue'
import { useAuthStore } from '@/stores/auth'
import { useSystemStore } from '@/stores/system'
import { useNotifyStore } from '@/stores/notify'
import { useWebSocket } from '@/composables/wsClient'
import { useToastStore } from '@/stores/toast'
import { authApi } from '@/api/auth'
import Icon from '@/components/shared/Icon.vue'

const auth = useAuthStore()
const system = useSystemStore()
const notify = useNotifyStore()
const ws = useWebSocket()
const route = useRoute()
const toast = useToastStore()

// WS 全局生命周期：登录后连接，登出时断开
// systemStore 拉取后端进程名（免认证接口），用于禁用自我日志 tail
onMounted(() => {
  auth.restoreUser()
  system.init()
  if (auth.token) {
    ws.connect()
  }
})

watch(() => auth.token, (token) => {
  if (token) {
    ws.connect()
  } else {
    ws.disconnect()
  }
})

// 默认密码告警：后端推送 default_password_warning 时 (wsHandlers 中) 会置
// auth.isDefaultPassword=true。此处提供持久 banner + "我知道了"
// 按钮调用 ack API,确认后置 isDefaultPassword=false,banner 由 v-if 隐藏。
async function ackDefaultPassword() {
  try {
    await authApi.ackDefaultPassword()
    auth.isDefaultPassword = false
  } catch {
    toast.error('确认失败,请稍后重试')
  }
}

onUnmounted(() => {
  ws.disconnect()
})
</script>

<template>
  <template v-if="route.meta.public">
    <router-view />
  </template>
  <AppShell v-else>
    <router-view />
  </AppShell>
  <div v-if="auth.isDefaultPassword" class="default-password-banner" role="alert">
    <span class="default-password-banner__icon">
      <Icon name="Tips" :size="16" />
    </span>
    <span class="default-password-banner__msg">
      系统正在使用默认管理员密码 88888888,存在安全风险,请尽快修改密码
    </span>
    <button type="button" class="default-password-banner__action" @click="ackDefaultPassword">
      我知道了
    </button>
  </div>
  <!-- 契约 notify-ui 前端义务: NOTIFY_UI level=error 且 popup=true 必须打断用户展示
       （遮罩 modal + 确认按钮，逐条出队） -->
  <div v-if="notify.popupCurrent" class="popup-overlay" role="alertdialog" aria-modal="true">
    <div class="popup-modal">
      <span class="popup-modal__icon">
        <Icon name="Tips" :size="16" />
      </span>
      <span class="popup-modal__msg">{{ notify.popupCurrent.message }}</span>
      <button type="button" class="popup-modal__action" @click="notify.ackPopup()">
        确认
      </button>
    </div>
  </div>
  <ToastContainer />
</template>

<style scoped>
.default-password-banner {
  position: fixed;
  top: var(--spacer-16);
  left: 50%;
  transform: translateX(-50%);
  z-index: 9000;
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  min-width: 360px;
  max-width: 640px;
  padding: var(--spacer-8) var(--spacer-12);
  border-radius: var(--radius-8);
  border: 1px solid var(--status-warning-default);
  background: var(--status-warning-surface-l1);
  box-shadow: var(--shadow-lg);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-default);
}

.default-password-banner__icon {
  flex-shrink: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  color: var(--status-warning-default);
}

.default-password-banner__msg {
  flex: 1;
  min-width: 0;
  word-break: break-word;
}

.default-password-banner__action {
  flex-shrink: 0;
  padding: var(--spacer-4) var(--spacer-12);
  border: 1px solid var(--status-warning-default);
  background: var(--status-warning-default);
  color: var(--text-inverse);
  border-radius: var(--radius-4);
  font-size: var(--body-sm-font-size);
  cursor: pointer;
  transition: background 120ms ease, border-color 120ms ease;
}

.default-password-banner__action:hover {
  background: var(--status-warning-hover);
  border-color: var(--status-warning-hover);
}

.popup-overlay {
  position: fixed;
  inset: 0;
  z-index: 10000;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--overlay-scrim);
}

.popup-modal {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--spacer-12);
  min-width: 320px;
  max-width: 480px;
  padding: var(--spacer-16);
  border-radius: var(--radius-8);
  border: 1px solid var(--status-error-default);
  background: var(--status-error-surface-l1);
  box-shadow: var(--shadow-xl);
}

.popup-modal__icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  color: var(--status-error-default);
}

.popup-modal__msg {
  word-break: break-word;
  text-align: center;
  font-size: var(--body-md-font-size);
  line-height: var(--body-md-line-height);
  color: var(--text-default);
}

.popup-modal__action {
  padding: var(--spacer-4) var(--spacer-16);
  border: 1px solid var(--status-error-default);
  background: var(--status-error-default);
  color: var(--text-inverse);
  border-radius: var(--radius-4);
  font-size: var(--body-sm-font-size);
  cursor: pointer;
  transition: background 120ms ease, border-color 120ms ease;
}

.popup-modal__action:hover {
  background: var(--status-error-hover);
  border-color: var(--status-error-hover);
}
</style>
