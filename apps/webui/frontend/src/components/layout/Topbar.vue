<script setup lang="ts">
import { computed } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import ThemeDropdown from './ThemeDropdown.vue'
import { useWebSocket } from '@/composables/wsClient'

const emit = defineEmits<{ toggleMobile: [] }>()

// WebSocket connection status
const { connectionState } = useWebSocket()

const connStatus = computed<{ text: string; dotClass: string }>(() => {
  switch (connectionState.value) {
    case 'connected':
      return { text: '已连接', dotClass: 'topbar__conn-dot--success' }
    case 'connecting':
      return { text: '连接中...', dotClass: 'topbar__conn-dot--warning' }
    case 'reconnecting':
      return { text: '重连中...', dotClass: 'topbar__conn-dot--warning' }
    case 'failed':
      return { text: '连接失败', dotClass: 'topbar__conn-dot--error' }
    case 'disconnected':
    default:
      return { text: '已断开', dotClass: 'topbar__conn-dot--error' }
  }
})
</script>

<template>
  <header class="topbar">
    <div class="topbar__left">
      <button
        class="topbar__hamburger"
        aria-label="打开导航菜单"
        @click="emit('toggleMobile')"
      >
        <Icon name="menu" :size="20" />
      </button>

      <!-- Resources -->
      <div class="topbar__resources" id="topbarResources">
        <!-- Service status: MD / TD / STG -->
        <span class="topbar__resource topbar__resource--svc topbar__resource--normal" id="resMd" title="行情服务">
          <span class="res-label">MD</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>
          </span>
        </span>
        <span class="topbar__resource topbar__resource--svc topbar__resource--warn" id="resTd" title="交易服务">
          <span class="res-label">TD</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
          </span>
        </span>
        <span class="topbar__resource topbar__resource--svc topbar__resource--danger" id="resStg" title="策略服务">
          <span class="res-label">STG</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>
          </span>
        </span>
        <span class="topbar__resource-divider"></span>
        <span class="topbar__resource topbar__resource--ntp topbar__resource--normal" id="resNtp" title="NTP时钟同步">
          <span class="res-label">NTP</span>
          <span class="res-value">+12ms</span>
        </span>
        <span class="topbar__resource topbar__resource--cpu topbar__resource--normal" id="resCpu" title="CPU">
          <span class="res-label">CPU</span>
          <span class="res-value">23%</span>
        </span>
        <span class="topbar__resource topbar__resource--mem topbar__resource--warn" id="resMem" title="内存">
          <span class="res-label">RAM</span>
          <span class="res-value">76%</span>
        </span>
        <span class="topbar__resource topbar__resource--disk topbar__resource--danger" id="resDisk0" title="C:">
          <span class="res-label">C:</span>
          <span class="res-value">93%</span>
        </span>
        <span class="topbar__resource topbar__resource--disk topbar__resource--normal" id="resDisk1" title="D:">
          <span class="res-label">D:</span>
          <span class="res-value">45%</span>
        </span>
      </div>
    </div>

    <div class="topbar__right">
      <span class="topbar__conn">
        <span class="topbar__conn-dot" :class="connStatus.dotClass"></span>
        <span>{{ connStatus.text }}</span>
      </span>
      <ThemeDropdown />
    </div>
  </header>
</template>

<style scoped>
.topbar__conn-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  display: inline-block;
}
.topbar__conn-dot--success {
  background: var(--status-success-default);
}
.topbar__conn-dot--warning {
  background: var(--status-warning-default);
}
.topbar__conn-dot--error {
  background: var(--status-error-default);
}
</style>
