<script setup lang="ts">
import { computed } from 'vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import ProcessStartButton from './ProcessStartButton.vue'
import { canRequestLogin } from '@/composables/marketSourceView'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'

// LoginPanel — 行情源登录/登出操作 + 自动登录开关
// 直接调 store (与现状 CtpCard 一致), props 仅传 source
const props = defineProps<{
  source: MarketSourceView
}>()

const store = useMarketSourcesStore()
const src = computed(() => props.source)

// 自动登录状态文案 + pill class (对照现状 autoLoginStatusText/Class)
const autoLoginStatusText = (s: MarketSourceView): string =>
  s.autoLoginPending ? '切换中' : (s.auto_login ? '已启用' : '未启用')

const autoLoginStatusClass = (s: MarketSourceView): string => {
  if (s.autoLoginPending) return 'is-pending'
  return s.auto_login ? 'is-on' : 'is-off'
}
</script>

<template>
  <!-- ── 登录操作 ── -->
  <div class="card-actions">
    <ProcessStartButton :source="src" />
    <!-- ADR 0051 / 契约 md-status 前端义务③：登录只在完全未登录（progress 端点到 min，
         loginState=offline）可用；连接中/登录中/断线重连中/登录失败态一律禁用——失败后
         不提供原地重试，恢复路径统一为「先登出、再登录」（登出在任意非未登录态可用）。 -->
    <button
      class="ds-btn ds-btn--sm"
      :class="src.loginState === 'offline' ? 'ds-btn--primary' : 'ds-btn--secondary'"
      type="button"
      :disabled="src.loginPending || src.process_state !== 'Running' || !canRequestLogin(src)"
      :title="src.process_state !== 'Running'
        ? '进程未运行，请先启动进程'
        : src.loginPending ? '登录请求已发出，等待进程回报'
        : src.loginState === 'online' ? '已登录，如需重新登录请先登出'
        : src.loginState === 'pending' ? '连接/登录/重连中或上次登录失败，请先登出再登录'
        : undefined"
      @click="store.login(src.id)"
    >
      <span v-if="src.loginPending" class="ds-btn__spinner"></span>
      {{ src.loginPending ? '登录中' : '登录' }}
    </button>
    <button
      class="ds-btn ds-btn--sm ds-btn--danger-subtle"
      type="button"
      :disabled="src.logoutPending || src.loginState === 'offline' || src.process_state !== 'Running'"
      :title="src.process_state !== 'Running'
        ? '进程未运行，请先启动进程'
        : src.loginState === 'offline' ? '当前未登录' : '结束当前会话（连接中/登录中/重连中时点此可强制中断）'"
      @click="store.logout(src.id)"
    >
      <span v-if="src.logoutPending" class="ds-btn__spinner"></span>
      {{ src.logoutPending ? '登出中' : '登出' }}
    </button>
  </div>

  <!-- ── 自动登录 ── -->
  <div class="card-section">
    <div class="card-section__row">
      <div class="auto-login-control">
        <SyncSwitch
          :model-value="src.auto_login"
          :pending="src.autoLoginPending || src.scheduleAddPending || src.scheduleRemovePending"
          label="自动登录"
          @change="(v: boolean) => store.toggleAutoLogin(src.id, v)"
        />
        <span class="auto-login-control__label">自动登录</span>
        <span class="auto-login-control__status" :class="autoLoginStatusClass(src)">{{ autoLoginStatusText(src) }}</span>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* Action toolbar (login/logout) */
.card-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: var(--spacer-8);
  padding-bottom: var(--spacer-12);
  border-bottom: 1px solid var(--border-neutral-l1);
  margin-bottom: var(--spacer-12);
}

/* Section */
.card-section {
  padding-bottom: var(--spacer-12);
  border-bottom: 1px solid var(--border-neutral-l1);
  margin-bottom: var(--spacer-12);
}

.card-section__row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-12);
  margin-bottom: var(--spacer-8);
}

/* Auto-login control row */
.auto-login-control {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}

.auto-login-control__label {
  font-size: var(--body-sm-font-size);
  font-weight: var(--font-weight-medium, 500);
  color: var(--text-secondary);
}

.auto-login-control__status {
  font-size: var(--body-xs-font-size);
  font-weight: var(--font-weight-medium, 500);
  white-space: nowrap;
}

.auto-login-control__status.is-on {
  color: var(--status-success-default);
}

.auto-login-control__status.is-off {
  color: var(--status-alert-default);
}

.auto-login-control__status.is-pending {
  color: var(--text-tertiary);
}

/* Spinner: 复用全局 components.css 的 .ds-btn__spinner（P4 T4 去重复，统一 14px） */
</style>