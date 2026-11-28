<script setup lang="ts">
import { useToastStore } from '@/stores/toast'
import Icon from '@/components/shared/Icon.vue'

const toast = useToastStore()

const ICON_MAP: Record<string, string> = {
  success: 'check',
  info: 'info',
  warning: 'warning_line',
  error: 'Error',
}

// 概要（ADR 0060 决策 3/6；视觉规格见 DESIGN.md「Notifications (toasts)」）：
// - `duration=0` 的条目 = 常驻（不自动消失、可手动关），但**不遮挡、不阻断操作**
//   （旧的全屏遮罩居中弹窗已删除）；常驻与瞬态**外观完全一致**——时长不是视觉属性
// - 同时最多 3 条，超出排队（stores/toast.ts 的 TOAST_MAX_VISIBLE）
// - hover / 键盘聚焦暂停倒计时
// - 中性浮层：--bg-elevated / --border-neutral-l2 / --shadow-overlay / --radius-12；
//   级别**只由图标表达**（形状 + 状态色），卡片本体、描边、文字不随级别变化
// - 不设倒计时进度条：一条彩色线同时编码级别与剩余时间（One Job Rule 违例），
//   且它是整页唯一持续运动的元素
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
        :class="`ds-toast--${item.level}`"
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
      </div>
    </TransitionGroup>
  </div>
</template>

<style scoped>
/* 单容器、右下角向上堆叠：新条目在底部（column 的末尾），旧条目向上推。 */
.toast-container {
  position: fixed;
  right: var(--spacer-20);
  bottom: var(--spacer-20);
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  pointer-events: none;
}

.ds-toast {
  pointer-events: auto;
  display: flex;
  align-items: flex-start;
  gap: var(--spacer-8);
  /* 定宽而不是 min/max 双档：同一时刻堆叠的几条必须读成一摞 */
  width: min(360px, calc(100vw - 2 * var(--spacer-20)));
  padding: var(--spacer-10);
  padding-left: var(--spacer-12);
  border-radius: var(--radius-12);
  /* 1px 全边（不是彩色左边框）：impeccable craft-floor 禁止 >1px 的彩色单边条 */
  border: 1px solid var(--border-neutral-l2);
  background: var(--bg-elevated);
  box-shadow: var(--shadow-overlay);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-default);
}

.ds-toast__icon {
  flex-shrink: 0;
  width: 16px;
  height: 16px;
  /* 图标与 18px 行高的首行视觉居中（16px 图形比行盒矮 2px） */
  margin-top: var(--spacer-1);
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.ds-toast--success .ds-toast__icon { color: var(--status-success-default); }
.ds-toast--info .ds-toast__icon { color: var(--status-primary-default); }
.ds-toast--warning .ds-toast__icon { color: var(--status-warning-default); }
.ds-toast--error .ds-toast__icon { color: var(--status-error-default); }

.ds-toast__msg {
  flex: 1;
  min-width: 0;
  word-break: break-word;
}

.ds-toast__close {
  flex-shrink: 0;
  width: 20px;
  height: 20px;
  /* 与图标同样贴齐首行（20px 按钮对 18px 行高） */
  margin-top: calc(-1 * var(--spacer-1));
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
}
</style>
