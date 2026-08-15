/**
 * 黑白名单添加方式（sourceType）展示辅助函数
 * 设计 §5.4：userManagement 4 处判断抽为共享函数
 */
export function sourceTypeText(src: 'auto' | 'manual'): string {
  return src === 'auto' ? '自动' : '手动'
}

export function sourceTypeClass(src: 'auto' | 'manual'): string {
  return src === 'auto' ? 'ds-tag ds-tag--warning' : 'ds-tag'
}