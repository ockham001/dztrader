/**
 * 测试用 `navigator.clipboard` 桩（#327）：两个用例文件（composables/clipboard 与历史视图组件）
 * 共用同一份安装/还原实现，避免「同一事实两处定义」（测试标准 §2.4）。
 */
const ORIGINAL = Object.getOwnPropertyDescriptor(globalThis.navigator, 'clipboard')

/** 用给定值安装 navigator.clipboard（`undefined` = 模拟浏览器不支持）。 */
export function installClipboardStub(value: unknown): void {
  Object.defineProperty(globalThis.navigator, 'clipboard', { value, configurable: true })
}

/** 还原测试前状态（jsdom 可能本来就没有该属性）。 */
export function restoreClipboard(): void {
  if (ORIGINAL) {
    Object.defineProperty(globalThis.navigator, 'clipboard', ORIGINAL)
  } else {
    Reflect.deleteProperty(globalThis.navigator, 'clipboard')
  }
}
