import { describe, it, expect, vi, afterEach } from 'vitest'
import { copyTextToClipboard } from '../clipboard'
import { installClipboardStub, restoreClipboard } from '@/test-utils/clipboard'

// #327：剪贴板工具（全仓首个 clipboard 用法）——只把失败收敛为 false，由调用方提示。
afterEach(() => {
  restoreClipboard()
})

describe('copyTextToClipboard（#327）', () => {
  it('可用时写入并返回 true', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined)
    installClipboardStub({ writeText })

    await expect(copyTextToClipboard('a,b\r\n1,2\r\n')).resolves.toBe(true)
    expect(writeText).toHaveBeenCalledWith('a,b\r\n1,2\r\n')
  })

  it('浏览器不支持（无 clipboard / 无 writeText）返回 false，不抛错', async () => {
    installClipboardStub(undefined)
    await expect(copyTextToClipboard('x')).resolves.toBe(false)

    installClipboardStub({})
    await expect(copyTextToClipboard('x')).resolves.toBe(false)
  })

  it('权限拒绝（NotAllowedError）返回 false，不抛出', async () => {
    installClipboardStub({ writeText: vi.fn().mockRejectedValue(new Error('NotAllowedError')) })
    await expect(copyTextToClipboard('x')).resolves.toBe(false)
  })
})
