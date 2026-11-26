import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { makeSource } from '@/composables/marketSourceView'
import { useMarketSourcesStore, type MarketSourceView } from '@/stores/marketSources'
import LoginPanel from '../LoginPanel.vue'

// LoginPanel — 登录/登出按钮可用性 + 自动登录开关。
// ADR 0051 统一两段式：登录只在完全未登录（loginState=offline）可用；连接中/登录中/断线
// 重连中/失败态一律禁用（失败后不提供原地重试，恢复 = 先登出再登录）；登出在任意非未登录态
// 可用（= 强制中断）。
const stubs = {
  ProcessStartButton: { template: '<div class="psb" />' },
  SyncSwitch: { props: ['modelValue', 'pending'], emits: ['change'], template: '<button type="button" class="sync-switch" />' },
}

function sourceWith(patch: Partial<MarketSourceView>): MarketSourceView {
  return makeSource({
    id: 1,
    source_type: 'CTP',
    source_name: 'dzmd_ctp',
    display_name: 'CTP',
    process_state: 'Running',
    ...patch,
  })
}

function loginButton(w: ReturnType<typeof mount>) {
  // pending 时按钮文案为「登录中」，用包含匹配
  return w.findAll('button').find(b => b.text().includes('登录'))!
}

describe('LoginPanel 登录按钮可用性（ADR 0051 统一两段式）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('loginPending 时禁用', () => {
    const w = mount(LoginPanel, { props: { source: sourceWith({ loginPending: true }) }, global: { stubs } })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })

  it('登录中（loginState=pending 且无 lastLoginError）时禁用', () => {
    const w = mount(LoginPanel, {
      props: { source: sourceWith({ loginState: 'pending', loginPending: false }) },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })

  it('失败态（current=3）禁用登录：恢复走「先登出、再登录」，不提供原地重试', () => {
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'pending',
          loginPending: false,
          // 契约 md-status 失败语义: 失败态停在登录阶段 3/4（ADR 0059）、不回退到 Connected
          progressView: { current: 3, min: 0, max: 4, desc: '登录失败' },
          lastLoginError: { code: 'login_rejected', message: 'CTP:用户密码错误', errorId: 3, timestamp: 1_700_000_000 },
        }),
      },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })

  it('断线自动重连中（current=1）禁用登录', () => {
    // 契约 md-status: Disconnected 期间 md 进程不接受新的连接请求；ADR 0051 下登录只在
    // 完全未登录可用，重连中一律禁用（登出可用，见下方/td 侧同规则）。
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'pending',
          loginPending: false,
          progressView: { current: 1, min: 0, max: 4, desc: '重连中...' },
          lastLoginError: { code: 'login_timeout', message: '', errorId: 0, timestamp: 1_700_000_000 },
        }),
      },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })

  it('失败态点击登录不下发请求（按钮禁用）', async () => {
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'pending',
          progressView: { current: 3, min: 0, max: 4 },
          lastLoginError: { code: 'login_timeout', message: '', errorId: 0, timestamp: 1_700_000_000 },
        }),
      },
      global: { stubs },
    })
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'login').mockResolvedValue()
    await loginButton(w).trigger('click')
    expect(spy).not.toHaveBeenCalled()
  })

  it('进程未运行时禁用（保持现状）', () => {
    const w = mount(LoginPanel, {
      props: { source: sourceWith({ process_state: 'Stopped' }) },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })

  it('已登录（online）时禁用登录（重新登录需先登出，避免 md 拒绝后 pending 悬挂）', () => {
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'online',
          progressView: { current: 4, min: 0, max: 4 },
        }),
      },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })
})
