import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { makeSource } from '@/composables/marketSourceView'
import { useMarketSourcesStore, type MarketSourceView } from '@/stores/marketSources'
import LoginPanel from '../LoginPanel.vue'

// LoginPanel — 登录/登出按钮可用性 + 自动登录开关。
// 契约 md-status 前端义务③：失败态（last_error 非 null 且未登录）允许直接重试登录，
// 不要求先登出——失败后 RTN_PROGRESS 仍为 current=2（与 Connected 同映射），
// 不能再用 loginState === 'pending' 一刀切禁用登录按钮。
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

describe('LoginPanel 登录按钮可用性（契约 md-status 前端义务③）', () => {
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

  it('失败态（loginState=pending、lastLoginError 且进度处于已连接档 current=2）时允许直接重试', () => {
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'pending',
          loginPending: false,
          // 契约 md-status 失败语义: 失败态 RTN_PROGRESS 映射与 Connected 相同（current=2）
          progressView: { current: 2, min: 0, max: 4, desc: '登录失败' },
          lastLoginError: { code: 'login_rejected', message: 'CTP:用户密码错误', errorId: 3, timestamp: 1_700_000_000 },
        }),
      },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeUndefined()
  })

  it('断线自动重连中（current=1，last_error 按契约保留）不得启用重试', () => {
    // 契约 md-status: last_error 在 Disconnected 期间保留；此时 md 进程拒绝重连请求，
    // 若启用按钮会静默拒绝并让 pending 悬挂到 45s 兜底。
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

  it('失败态点击登录直接下发 REQUEST_MD_CONNECT（不要求先登出）', async () => {
    const w = mount(LoginPanel, {
      props: {
        source: sourceWith({
          loginState: 'pending',
          progressView: { current: 2, min: 0, max: 4 },
          lastLoginError: { code: 'login_timeout', message: '', errorId: 0, timestamp: 1_700_000_000 },
        }),
      },
      global: { stubs },
    })
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'login').mockResolvedValue()
    await loginButton(w).trigger('click')
    expect(spy).toHaveBeenCalledWith(1)
  })

  it('进程未运行时禁用（保持现状）', () => {
    const w = mount(LoginPanel, {
      props: { source: sourceWith({ process_state: 'Stopped' }) },
      global: { stubs },
    })
    expect(loginButton(w).attributes('disabled')).toBeDefined()
  })
})
