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
import { notificationsApi } from '@/api/notifications'
import ChangePasswordModal from '@/components/shared/ChangePasswordModal.vue'
import Icon from '@/components/shared/Icon.vue'

const auth = useAuthStore()
const system = useSystemStore()
const notify = useNotifyStore()
const ws = useWebSocket()
const route = useRoute()
const toast = useToastStore()

// WS 全局生命周期：登录后连接，登出时断开
// systemStore 拉取后端进程名（免认证接口），用于禁用自我日志 tail
// 契约 rest §1「强制改密」：must_change_password 的账户 WS 一并被拒（auth failed），
// 而 wsClient 把 auth failed 当作会话失效并登出。因此强制改密期间**不建 WS 连接**，
// 否则用户会在「登录 → 强制改密弹窗 → 重连被拒 → 登出」之间循环。
function syncWsConnection(): void {
  if (auth.token && !auth.mustChangePassword) {
    ws.connect()
  } else {
    ws.disconnect()
    // 登出 / 会话失效即清空消息中心：否则下一个账号登录后会看到上一会话的通知
    // （通知是进程级事件、不含账号维度，跨账号残留既是信息泄漏也是误导）。
    notify.clear()
  }
}

onMounted(() => {
  auth.restoreUser()
  system.init()
  syncWsConnection()
})

watch(() => auth.token, syncWsConnection)
watch(() => auth.mustChangePassword, syncWsConnection)

// 默认密码告警：后端推送 default_password_warning 时 (wsHandlers 中) 会把后端文案写进
// auth.defaultPasswordWarning。此处提供持久 banner + "我知道了" 按钮调用 ack API；
// 确认后清空告警态，banner 由 v-if 隐藏。
// 契约 rest §2.1 / webui-ws §2.3：文案唯一真相源在后端，本组件不硬编码任何告警文本。
async function ackDefaultPassword() {
  try {
    await authApi.ackDefaultPassword()
    auth.clearDefaultPasswordWarning()
  } catch {
    toast.error('确认失败,请稍后重试')
  }
}

// notify 的呈现（ADR 0060 决策 3）：popup=true 的条目走**常驻 toast**（不自动消失、
// 不遮挡、不阻断操作），由 ToastContainer 渲染。旧的全屏遮罩居中弹窗（alertdialog +
// 焦点陷阱 + 滚动锁）已删除——它阻挡行情/持仓/委托，与盯盘场景冲突。
// 因此本文件不再持有任何 popup 焦点管理代码。

// 通知历史基线（契约 rest §2.6）：刷新后消息中心仍有内容。
// WS 增量随连接建立后到达，REST 基线只拉一次；登录成功后才拉（未登录时接口 401）。
watch(
  () => auth.token,
  async (token) => {
    if (!token) return
    try {
      const payloads = await notificationsApi.list()
      notify.applyBaseline(payloads)
    } catch {
      // 基线失败不打扰用户：WS 增量仍会填充消息中心
    }
  },
  { immediate: true },
)

onUnmounted(() => {
  ws.disconnect()
})
</script>

<template>
  <template v-if="route.meta.public">
    <router-view />
  </template>
  <AppShell v-else-if="!auth.mustChangePassword">
    <router-view />
  </AppShell>
  <div
    v-if="auth.defaultPasswordWarning && !auth.mustChangePassword"
    class="default-password-banner"
    role="alert"
  >
    <span class="default-password-banner__icon">
      <Icon name="Tips" :size="16" />
    </span>
    <span class="default-password-banner__msg">
      {{ auth.defaultPasswordWarning }}
    </span>
    <button type="button" class="default-password-banner__action" @click="ackDefaultPassword">
      我知道了
    </button>
  </div>
  <!-- 契约 rest §1「强制改密」：被管理员重置口令的账户登录后必须改密才能用系统；
       后端同时拦接口（403 password_change_required），前端这一层只是让路径可走完 -->
  <ChangePasswordModal :open="auth.mustChangePassword" forced />
  <!-- 契约 notify-ui 前端义务（ADR 0060 决策 3）: popup=true 必须**常驻**呈现，
       但不得以全屏遮罩等阻断操作的形态。由 ToastContainer 渲染为右下角常驻卡片。 -->
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
</style>
