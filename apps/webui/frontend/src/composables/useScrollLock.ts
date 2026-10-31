// 模态对话框的 body 滚动锁（#206 A）。
//
// 为什么需要引用计数：同一时刻可能有多个模态同时打开（强制改密弹窗 + notify 弹窗、
// 或路由切换时新旧视图短暂共存）。若每个实例各自写 `body.style.overflow = ''`，
// 后关闭的那个会把仍在打开的那个的锁一起解掉——背景恢复滚动，且没有任何报错。
// 这里用计数器把「最后一个关闭者才解锁」变成唯一出口。
import { onBeforeUnmount, watch, type Ref } from 'vue'

let lockCount = 0
// 记录进入锁定前的内联值，退出时精确还原（而不是无条件清空）
let savedOverflow = ''

/** 申请滚动锁（幂等配对的唯一入口是 useScrollLock，请勿直接调用） */
function acquire(): void {
  if (lockCount === 0) {
    savedOverflow = document.body.style.overflow
    document.body.style.overflow = 'hidden'
  }
  lockCount += 1
}

/** 释放滚动锁；只有计数归零（最后一个持有者）才真正还原 */
function release(): void {
  if (lockCount === 0) return
  lockCount -= 1
  if (lockCount === 0) {
    document.body.style.overflow = savedOverflow
  }
}

/**
 * 按 `active` 自动申请/释放 body 滚动锁。
 *
 * - `immediate` 兜住「挂载时已打开」：只监听变化会让首帧漏锁；
 *   且 immediate 分支只处理 active===true，不会在挂载即关闭时误放别人的锁。
 * - 卸载时若仍持有锁必须释放，否则路由切换会让整站无法滚动。
 */
export function useScrollLock(active: Ref<boolean>): void {
  let held = false

  function sync(isActive: boolean): void {
    if (isActive && !held) {
      acquire()
      held = true
      return
    }
    if (!isActive && held) {
      release()
      held = false
    }
  }

  watch(active, sync, { immediate: true })

  onBeforeUnmount(() => {
    if (held) {
      release()
      held = false
    }
  })
}
