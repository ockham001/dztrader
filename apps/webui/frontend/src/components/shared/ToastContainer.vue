<script setup lang="ts">
import { useToastStore } from '@/stores/toast'
import Icon from '@/components/shared/Icon.vue'

const toast = useToastStore()

const ICON_MAP: Record<string, string> = {
  success: 'check',
  warning: 'Tips',
  error: 'Error',
}
</script>

<template>
  <div class="toast-container" role="region" aria-label="通知">
    <TransitionGroup name="toast">
      <div
        v-for="item in toast.items"
        :key="item.id"
        class="ds-toast"
        :class="`ds-toast--${item.level}`"
        role="alert"
      >
        <span class="ds-toast__icon">
          <Icon :name="ICON_MAP[item.level] ?? 'Tips'" :size="16" />
        </span>
        <span class="ds-toast__msg">{{ item.message }}</span>
        <button
          class="ds-toast__close"
          type="button"
          aria-label="关闭"
          @click="toast.dismiss(item.id)"
        >
          <Icon name="close" :size="14" />
        </button>
        <span v-if="item.duration > 0" class="ds-toast__progress">
          <span class="ds-toast__progress-bar" :style="{ animationDuration: `${item.duration}ms` }"></span>
        </span>
      </div>
    </TransitionGroup>
  </div>
</template>

<style scoped>
.toast-container {
  position: fixed;
  right: var(--spacer-24);
  bottom: var(--spacer-24);
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  pointer-events: none;
}

.ds-toast {
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
  min-width: 280px;
  max-width: 420px;
  padding: var(--spacer-10) var(--spacer-12);
  padding-right: var(--spacer-10);
  border-radius: var(--radius-8);
  border: 1px solid var(--border-neutral-l1);
  background: var(--bg-base-default);
  box-shadow: var(--shadow-lg);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-default);
  overflow: hidden;
  position: relative;
}

.ds-toast__icon {
  flex-shrink: 0;
  width: 18px;
  height: 18px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.ds-toast--success .ds-toast__icon { color: var(--status-success-default); }
.ds-toast--success { border-left: 3px solid var(--status-success-default); }

.ds-toast--warning .ds-toast__icon { color: var(--status-warning-default); }
.ds-toast--warning { border-left: 3px solid var(--status-warning-default); }

.ds-toast--error .ds-toast__icon { color: var(--status-error-default); }
.ds-toast--error { border-left: 3px solid var(--status-error-default); }

.ds-toast__msg {
  flex: 1;
  min-width: 0;
  word-break: break-word;
}

.ds-toast__close {
  flex-shrink: 0;
  width: 20px;
  height: 20px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  cursor: pointer;
  color: var(--icon-tertiary);
  border-radius: var(--radius-4);
  transition: background 120ms ease, color 120ms ease;
}

.ds-toast__close:hover {
  background: var(--bg-overlay-l2);
  color: var(--icon-default);
}

.ds-toast__progress {
  position: absolute;
  left: 0;
  bottom: 0;
  width: 100%;
  height: 2px;
  background: transparent;
  overflow: hidden;
}

.ds-toast__progress-bar {
  display: block;
  height: 100%;
  width: 100%;
  transform-origin: left;
  animation: toast-shrink linear forwards;
}

.ds-toast--success .ds-toast__progress-bar { background: var(--status-success-default); }
.ds-toast--warning .ds-toast__progress-bar { background: var(--status-warning-default); }
.ds-toast--error .ds-toast__progress-bar { background: var(--status-error-default); }

@keyframes toast-shrink {
  from { transform: scaleX(1); }
  to { transform: scaleX(0); }
}

/* Vue TransitionGroup */
.toast-enter-active {
  transition: all 300ms cubic-bezier(0.2, 0, 0, 1);
}

.toast-leave-active {
  transition: all 250ms cubic-bezier(0.4, 0, 1, 1);
  position: absolute;
  right: 0;
  bottom: 0;
}

.toast-enter-from {
  opacity: 0;
  transform: translateX(100%);
}

.toast-leave-to {
  opacity: 0;
  transform: translateX(100%);
}

.toast-move {
  transition: transform 300ms ease;
}
</style>
