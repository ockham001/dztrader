/**
 * 剪贴板写入（#327）：复制历史序列 CSV 用。
 *
 * 全仓此前没有 clipboard 先例；这里只做一件事——把「浏览器不支持」与「权限被拒」收敛成
 * `false`，由调用方给出**可见**提示（toast），不静默失败、不抛错打断事件循环，也不弹原生
 * `alert`（与全仓 toast 提示范式一致）。
 */
export async function copyTextToClipboard(text: string): Promise<boolean> {
  try {
    const clipboard = globalThis.navigator?.clipboard
    if (!clipboard || typeof clipboard.writeText !== 'function') {
      return false
    }
    await clipboard.writeText(text)
    return true
  } catch {
    // 权限拒绝（NotAllowedError）/ 非安全上下文 / 其它实现差异：统一按失败处理
    return false
  }
}
