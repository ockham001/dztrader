<script setup lang="ts">
import { useToastStore } from '@/stores/toast'
import Icon from '@/components/shared/Icon.vue'

const toast = useToastStore()

const ICON_MAP: Record<string, string> = {
  success: 'check',
  info: 'Tips',
  warning: 'Tips',
  error: 'Error',
}

// 概要（ADR 0060 决策 3/6）：
// - `duration=0` 的条目 = 常驻（不自动消失、可手动关），但**不遮挡、不阻断操作**
//   （旧的全屏遮罩居中弹窗已删除）
// - 同时最多 3 条，超出排队（stores/toast.ts 的 TOAST_MAX_VISIBLE）
// - hover / 键盘聚焦暂停倒计时
// - 进场 200ms / 出场 150ms，cubic-bezier(0.16, 1, 0.3, 1)；出场快于入场
// - 状态色走图标 + 1px 全边 + 阴影，**不用彩色左边框**（impeccable craft-floor 硬禁）
//
// 可访问性用**单容器**：`aria-live="polite"` 覆盖全部条目；error 条目自身 `role="alert"`
// （隐式 assertive），读屏按元素粒度提升，不需要第二个容器。
// 双容器方案已弃用——它要用「按 3 条定高」硬算偏移，不足 3 条时留空、多行卡会重叠。
</script>

<template>
  <div
    class="toast-container"
    role="region"
    aria-label="通知"
    aria-live="polite"
    aria-atomic="false"
  >
    <TransitionGroup name="toast">
      <!-- eslint-disable-next-line vuejs-accessibility/no-static-element-interactions -- 整条 toast 不是可点击控件（无 click/激活语义），mouseenter/focusin 只用于「暂停倒计时」这一被动行为；键盘等价交互已由 focusin/focusout 提供 -->
      <div
        v-for="item in toast.items"
        :key="item.id"
        class="ds-toast"
        :class="[
          `ds-toast--${item.level}`,
          { 'ds-toast--persistent': item.duration <= 0 },
        ]"
        :role="item.level === 'error' ? 'alert' : undefined"
        @mouseenter="toast.pause(item.id)"
        @mouseleave="toast.resume(item.id)"
        @focusin="toast.pause(item.id)"
        @focusout="toast.resume(item.id)"
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
          <Icon name="Close" :size="14" />
        </button>
        <span v-if="item.duration > 0" class="ds-toast__progress">
          <span
            class="ds-toast__progress-bar"
            :class="{ 'is-paused': item.paused }"
            :style="{ animationDuration: `${item.duration}ms` }"
          ></span>
        </span>
      </div>
    </TransitionGroup>
  </div>
</template>

<style scoped>
/* 单容器、右下角向上堆叠：新条目在底部（column 的末尾），旧条目向上推。 */
.toast-container {
  position: fixed;
  right: var(--spacer-24);
  bottom: var(--spacer-24);
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  pointer-events: none;
  max-width: 420px;
}

.ds-toast {
  pointer-events: auto;
  display: flex;
  align-items: flex-start;
  gap: var(--spacer-8);
  min-width: 280px;
  max-width: 420px;
  padding: var(--spacer-10) var(--spacer-12);
  padding-right: var(--spacer-10);
  border-radius: var(--radius-8);
  /* 1px 全边（不是彩色左边框）：impeccable craft-floor 禁止 >1px 的彩色单边条 */
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
.ds-toast--info .ds-toast__icon { color: var(--status-primary-default); }
.ds-toast--warning .ds-toast__icon { color: var(--status-warning-default); }
.ds-toast--error .ds-toast__icon { color: var(--status-error-default); }

/* error 加强存在感：微红底 + 红描边（不是彩色左边条） */
.ds-toast--error {
  background: var(--status-error-surface-l1);
  border-color: var(--status-error-surface-l2);
}

/* 常驻条目：描边更实，让人知道「这条不会自己走」 */
.ds-toast--persistent {
  border-color: var(--status-error-default);
}

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

/* hover / 聚焦暂停：冻结进度条动画（计时器由 item.paused 控制） */
.ds-toast__progress-bar.is-paused {
  animation-play-state: paused;
}

.ds-toast--success .ds-toast__progress-bar { background: var(--status-success-default); }
.ds-toast--info .ds-toast__progress-bar { background: var(--status-primary-default); }
.ds-toast--warning .ds-toast__progress-bar { background: var(--status-warning-default); }
.ds-toast--error .ds-toast__progress-bar { background: var(--status-error-default); }

@keyframes toast-shrink {
  from { transform: scaleX(1); }
  to { transform: scaleX(0); }
}

/* Vue TransitionGroup：进场 200ms / 出场 150ms（出场快于入场，impeccable animate.md）
   位移改小（8px，不是 100%）——安静的滑入，不抢注意力。 */
.toast-enter-active {
  transition: opacity 200ms cubic-bezier(0.16, 1, 0.3, 1),
              transform 200ms cubic-bezier(0.16, 1, 0.3, 1);
}

.toast-leave-active {
  transition: opacity 150ms cubic-bezier(0.4, 0, 1, 1),
              transform 150ms cubic-bezier(0.4, 0, 1, 1);
  position: absolute;
  right: 0;
  bottom: 0;
}

.toast-enter-from {
  opacity: 0;
  transform: translateY(8px);
}

.toast-leave-to {
  opacity: 0;
  transform: translateY(8px);
}

.toast-move {
  transition: transform 200ms ease;
}

/* 减少动效：**只去位移、保留透明度**（impeccable animate.md:77 / audit.md:15
   —— 全局 0.01ms 一刀切会连反馈一起抹掉） */
@media (prefers-reduced-motion: reduce) {
  .toast-enter-active,
  .toast-leave-active,
  .toast-move {
    transition-property: opacity;
  }

  .toast-enter-from,
  .toast-leave-to {
    transform: none;
  }

  .ds-toast__progress-bar {
    animation: none;
  }
}
</style>
