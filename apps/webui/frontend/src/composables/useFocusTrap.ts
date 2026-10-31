// 焦点陷阱：模态对话框打开时把焦点关进面板、Tab/Shift+Tab 在面板内循环、
// 关闭后把焦点还原到打开它的触发元素（#206 A）。
//
// 为什么不自引第三方 focus-trap：本仓只有 Modal 与 App.vue 通知弹窗两处需要，
// 共用不到 100 行；此前「Esc 关闭 + body 锁定」已在两处各写一套并漂移过。
import { nextTick, onUnmounted, watch, type Ref } from 'vue'

// 可聚焦候选：与浏览器 Tab 序列口径一致（排除 disabled / 负 tabindex）
const FOCUSABLE_SELECTOR = [
  'a[href]',
  'button:not([disabled])',
  'input:not([disabled])',
  'select:not([disabled])',
  'textarea:not([disabled])',
  '[tabindex]:not([tabindex="-1"])',
].join(',')

/** 活动陷阱栈：只有栈顶（最后打开的）对话框响应 Tab，避免嵌套对话框互相抢焦点 */
const activeTraps: symbol[] = []

function isVisible(el: HTMLElement): boolean {
  // jsdom 不做布局（offsetParent 恒为 null），用 hidden 祖先与计算样式兜底，
  // 让测试环境与真实浏览器对「能不能 Tab 到」给出一致结论
  if (el.closest('[hidden]')) return false
  const style = el.ownerDocument.defaultView?.getComputedStyle(el)
  if (!style) return true
  return style.display !== 'none' && style.visibility !== 'hidden'
}

/** 收集容器内当前可聚焦元素，DOM 顺序即 Tab 顺序 */
export function focusableWithin(container: HTMLElement): HTMLElement[] {
  return Array.from(container.querySelectorAll<HTMLElement>(FOCUSABLE_SELECTOR)).filter(isVisible)
}

export interface FocusTrapOptions {
  /** 陷阱启用状态（通常就是 dialog 的 open） */
  active: Ref<boolean>
  /** 对话框面板元素（陷阱边界） */
  container: Ref<HTMLElement | null>
  /** 打开时优先聚焦的元素；未提供或未渲染时退化为容器内第一个可聚焦元素 */
  initialFocus?: Ref<HTMLElement | null>
}

export interface FocusTrapHandle {
  /** 手动把焦点移入容器（打开后由 watch 自动调用；测试可直接调用） */
  focusInitial: () => void
}

/**
 * 安装焦点陷阱。
 *
 * 触发元素的捕获时机：`active` 翻成 true 的那一刻取 `document.activeElement`。
 * 调用方若是「先关菜单再开弹窗」，应在置 open=true 之前把焦点落回触发按钮，
 * 否则还原目标会变成被关掉的菜单项（不可见元素不还原，见 restoreFocus）。
 */
export function useFocusTrap(options: FocusTrapOptions): FocusTrapHandle {
  const { active, container, initialFocus } = options
  const token = Symbol('focus-trap')
  let trigger: HTMLElement | null = null

  function focusInitial(): void {
    const box = container.value
    if (!box) return
    const target = initialFocus?.value ?? focusableWithin(box)[0] ?? box
    target.focus()
  }

  function restoreFocus(): void {
    const el = trigger
    trigger = null
    // 触发器已从 DOM 摘掉（如菜单项所属菜单被销毁）时不强行还原，避免焦点丢到 body
    if (!el || !el.isConnected) return
    el.focus()
  }

  /** Tab / Shift+Tab 在容器内循环；栈顶之外的活动陷阱不响应 */
  function trapTab(e: KeyboardEvent): void {
    if (e.key !== 'Tab' || activeTraps[activeTraps.length - 1] !== token) return
    const box = container.value
    if (!box) return
    const items = focusableWithin(box)
    if (items.length === 0) {
      // 无可聚焦元素时把焦点留在容器本身（容器带 tabindex="-1"）
      e.preventDefault()
      box.focus()
      return
    }
    const first = items[0]
    const last = items[items.length - 1]
    const current = document.activeElement
    if (e.shiftKey) {
      if (current === first || !box.contains(current)) {
        e.preventDefault()
        last.focus()
      }
      return
    }
    if (current === last || !box.contains(current)) {
      e.preventDefault()
      first.focus()
    }
  }

  // active 是唯一的焦点接管入口：打开→记触发器、移入焦点、挂 keydown；
  // 关闭→摘栈、解绑、还原。nextTick 是因为 v-if 渲染的面板在同一 tick 内拿不到 DOM。
  // immediate 兜住「挂载即已打开」：父组件一开始就把 open 置 true 时也要接管焦点，
  // 否则嵌套页面（强制改密弹窗）首帧键盘用户仍能从背景 Tab 出去。
  watch(active, async (isOpen) => {
    if (isOpen) {
      const el = document.activeElement
      // body 作 activeElement 时没有可还原目标，置空避免焦点丢回 body
      trigger = el instanceof HTMLElement && el !== document.body ? el : null
      activeTraps.push(token)
      await nextTick()
      document.addEventListener('keydown', trapTab, true)
      focusInitial()
      return
    }
    const idx = activeTraps.indexOf(token)
    if (idx !== -1) activeTraps.splice(idx, 1)
    document.removeEventListener('keydown', trapTab, true)
    await nextTick()
    restoreFocus()
  }, { immediate: true })

  onUnmounted(() => {
    const idx = activeTraps.indexOf(token)
    if (idx !== -1) activeTraps.splice(idx, 1)
    document.removeEventListener('keydown', trapTab, true)
  })

  return { focusInitial }
}
