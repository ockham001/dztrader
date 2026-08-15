import { describe, it, expect } from 'vitest'
import { getIconUrl } from '@/composables/useIcon'

// 图标 URL 工具函数测试
describe('getIconUrl', () => {
  // 验证图标 URL 的拼接格式
  it('应该根据图标名生成正确的 URL', () => {
    const url = getIconUrl('settings')
    expect(url).toBe('/icons/builtin/settings.svg')
  })

  // 验证不同图标名生成不同 URL
  it('应该为不同图标名生成不同 URL', () => {
    expect(getIconUrl('home')).not.toBe(getIconUrl('user'))
  })

  // 验证空字符串也能生成合法路径
  it('应该处理空字符串图标名', () => {
    expect(getIconUrl('')).toBe('/icons/builtin/.svg')
  })
})
