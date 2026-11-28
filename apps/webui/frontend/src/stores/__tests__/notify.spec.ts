import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useNotifyStore } from '../notify'
import { useMarketSourcesStore } from '../marketSources'

// mock toast store：断言 push 的 toast 分发调用（error/warning/info/persistent）
const toastMocks = vi.hoisted(() => ({
  error: vi.fn(),
  warning: vi.fn(),
  info: vi.fn(),
  persistent: vi.fn(),
  show: vi.fn(),
  success: vi.fn(),
  dismiss: vi.fn(),
  clear: vi.fn(),
  items: [] as unknown[],
}))
vi.mock('../toast', () => ({
  useToastStore: () => toastMocks,
}))

describe('useNotifyStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    toastMocks.error.mockClear()
    toastMocks.warning.mockClear()
    toastMocks.info.mockClear()
    toastMocks.persistent.mockClear()
  })

  afterEach(() => {
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  it('push 缓存条目（含 source/level/timestamp/count）', () => {
    const store = useNotifyStore()
    store.push('订阅失败', 'dzmd_ctp', 'error')
    expect(store.items).toHaveLength(1)
    expect(store.items[0]).toMatchObject({
      source: 'dzmd_ctp',
      message: '订阅失败',
      level: 'error',
      count: 1,
    })
    expect(typeof store.items[0].timestamp).toBe('number')
  })

  it('push 按 level 分发到对应 toast 方法', () => {
    const store = useNotifyStore()
    store.push('错误', undefined, 'error')
    expect(toastMocks.error).toHaveBeenCalledWith('错误')

    store.push('警告', undefined, 'warning')
    expect(toastMocks.warning).toHaveBeenCalledWith('警告')

    store.push('信息', undefined, 'info')
    expect(toastMocks.info).toHaveBeenCalledWith('信息')
  })

  it('level 缺失时默认 error', () => {
    const store = useNotifyStore()
    store.push('未分级消息')
    expect(toastMocks.error).toHaveBeenCalledWith('未分级消息')
    expect(store.items[0].level).toBe('error')
  })

  it('空消息不弹 toast 不缓存', () => {
    const store = useNotifyStore()
    store.push('', 'dzmd_ctp', 'error')
    expect(store.items).toHaveLength(0)
    expect(toastMocks.error).not.toHaveBeenCalled()
  })

  it('缓存只保留最近 100 条', () => {
    const store = useNotifyStore()
    for (let i = 0; i < 105; i++) {
      store.push(`消息 ${i}`, undefined, 'info')
    }
    expect(store.items).toHaveLength(100)
    expect(store.items[0].message).toBe('消息 5')
    expect(store.items[99].message).toBe('消息 104')
  })

  it('clear 清空缓存', () => {
    const store = useNotifyStore()
    store.push('a')
    store.push('b')
    store.clear()
    expect(store.items).toHaveLength(0)
  })

  // ADR 0060 决策 3：popup=true 走**常驻** toast（不遮挡、不打断、无 popup 队列）
  it('popup=true 走常驻 toast，不自动消失', () => {
    const store = useNotifyStore()
    store.push('交易登录失败', 'dztd_ctp', 'error', true)
    expect(toastMocks.persistent).toHaveBeenCalledWith('交易登录失败', 'error')
    // 不调用普通 error（那会 10s 自动消失）
    expect(toastMocks.error).not.toHaveBeenCalled()
  })

  it('popup=false 走普通分级 toast', () => {
    const store = useNotifyStore()
    store.push('下单被拒', 'dztd_ctp', 'warning', false)
    expect(toastMocks.warning).toHaveBeenCalledWith('下单被拒')
    expect(toastMocks.persistent).not.toHaveBeenCalled()
  })

  // ADR 0060 决策 4：去重在 dzweb，前端只做「同指纹就地更新，不新增行」
  it('同指纹消息就地更新 count，不新增行、不重复弹 toast', () => {
    const store = useNotifyStore()
    store.push('平今仓位不足', 'dztd_ctp', 'warning', false, 100, 1)
    expect(store.items).toHaveLength(1)
    toastMocks.warning.mockClear()

    // 后端折叠后的第二轮：同指纹、count=3
    store.push('平今仓位不足', 'dztd_ctp', 'warning', false, 130, 3)
    expect(store.items).toHaveLength(1) // 不新增行
    expect(store.items[0].count).toBe(3) // 计数更新
    expect(store.items[0].timestamp).toBe(130 * 1000) // 最后发生时间更新
    expect(toastMocks.warning).not.toHaveBeenCalled() // 不重复提示
  })

  it('不同指纹各自成行', () => {
    const store = useNotifyStore()
    store.push('错误 A', 'dztd_ctp', 'error')
    store.push('错误 B', 'dztd_ctp', 'error')
    expect(store.items).toHaveLength(2)
  })

  it('同文本不同来源不折叠（指纹含来源）', () => {
    const store = useNotifyStore()
    store.push('连接失败', 'dztd_ctp', 'error')
    store.push('连接失败', 'dzmd_ctp', 'error')
    expect(store.items).toHaveLength(2)
  })

  it('同文本不同级别不折叠（指纹含级别）', () => {
    const store = useNotifyStore()
    store.push('连接异常', 'dztd_ctp', 'warning')
    store.push('连接异常', 'dztd_ctp', 'error')
    expect(store.items).toHaveLength(2)
  })

  // 未读与已读（纯前端内存）
  it('unreadCount 随新消息增长，markAllRead 后归零', () => {
    const store = useNotifyStore()
    expect(store.unreadCount).toBe(0)
    store.push('消息 1', undefined, 'info')
    store.push('消息 2', undefined, 'info')
    expect(store.unreadCount).toBe(2)
    store.markAllRead()
    expect(store.unreadCount).toBe(0)
  })

  // 审查发现：时间戳水位会被折叠「复活」——折叠把旧条目的 timestamp 更新成
  // 比列表末元素更新，水位式判定让老消息重新变未读、徽标卡死且清不掉。
  // 改按 id 记已读后，折叠不影响已读状态。
  it('已读按 id 记：折叠更新旧条目时间戳不会让它重新变未读', () => {
    const store = useNotifyStore()
    store.push('平今仓位不足', 'dztd_ctp', 'warning', false, 100, 1)
    store.markAllRead()
    expect(store.unreadCount).toBe(0)

    // 后端折叠后第二轮：同指纹、timestamp 更新到更晚、count 累加
    store.push('平今仓位不足', 'dztd_ctp', 'warning', false, 500, 2)
    // 折叠只改已有条目的 count/timestamp，id 不变 → 仍是已读
    expect(store.unreadCount).toBe(0)
  })

  it('count>1 的同一轮折叠取较大值（防帧乱序回退）', () => {
    const store = useNotifyStore()
    store.push('持久化失败', 'dztd_ctp', 'error', false, 100, 5)
    expect(store.items[0].count).toBe(5)
    // 同一 epoch 内先到的 count=3 不把已见过的 5 覆盖回去
    store.push('持久化失败', 'dztd_ctp', 'error', false, 130, 3)
    expect(store.items).toHaveLength(1)
    expect(store.items[0].count).toBe(5)
  })

  // 审查 blocker B2 / #530：冷却后后端以 count=1 发的是**新条目**，前端不得按指纹
  // 永久折叠，否则复发事件不弹提示、不计未读（ADR 0060 否决的「彻底静默」）。
  it('count=1 的同文案事件是冷却后新条目：新增行、重新提示、计入未读', () => {
    const store = useNotifyStore()
    store.push('持久化失败', 'dztd_ctp', 'error', true, 100, 5)
    store.markAllRead()
    toastMocks.persistent.mockClear()
    expect(store.items).toHaveLength(1)
    expect(store.unreadCount).toBe(0)

    // 冷却到点：后端新建条目并以 count=1 广播同文案
    store.push('持久化失败', 'dztd_ctp', 'error', true, 700, 1)

    expect(store.items).toHaveLength(2)                     // 新增行，不并进旧行
    expect(store.items[1].count).toBe(1)                    // 新 epoch 的计数从 1 开始
    expect(store.items[1].timestamp).toBe(700 * 1000)
    expect(toastMocks.persistent).toHaveBeenCalledWith('持久化失败', 'error')
    expect(store.unreadCount).toBe(1)
  })

  it('同文案跨 epoch 两行并存时，count>1 折叠更新的是最新一行', () => {
    const store = useNotifyStore()
    store.push('持久化失败', 'dztd_ctp', 'error', false, 100, 5)
    store.push('持久化失败', 'dztd_ctp', 'error', false, 700, 1) // 冷却后新 epoch
    expect(store.items).toHaveLength(2)

    store.push('持久化失败', 'dztd_ctp', 'error', false, 730, 2) // 新 epoch 被折叠
    expect(store.items[0].count).toBe(5) // 旧 epoch 不动
    expect(store.items[1].count).toBe(2) // 最新 epoch 计数更新
    expect(store.items).toHaveLength(2)
  })

  it('clear 清空列表与已读状态（登出时不残留上一会话通知）', () => {
    const store = useNotifyStore()
    store.push('消息 A', 'dztd_ctp', 'error')
    store.markAllRead()
    store.clear()
    expect(store.items).toHaveLength(0)
    expect(store.unreadCount).toBe(0)
  })

  it('applyBaseline 重建列表并把历史视为已读', () => {
    const store = useNotifyStore()
    store.push('旧消息', undefined, 'info') // 制造脏状态
    store.applyBaseline([
      { source: 'dztd_ctp', level: 'error', message: '历史错误', timestamp: 1700000000, popup: false },
      { source: 'dzmd_ctp', level: 'warning', message: '历史警告', timestamp: 1700000100, count: 5 },
    ])
    expect(store.items).toHaveLength(2)
    expect(store.items[0].message).toBe('历史错误')
    expect(store.items[1].count).toBe(5) // count 透传
    expect(store.unreadCount).toBe(0) // 基线视为已读
  })

  // 双 toast 修复核心断言：push 绝不写任何其他 store 的 error.value
  it('push 不触碰 marketSources.error.value（双 toast 修复）', () => {
    const ms = useMarketSourcesStore()
    const notify = useNotifyStore()
    expect(ms.error).toBeNull()

    notify.push('错误消息', 'dzmd_ctp', 'error')
    notify.push('警告消息', 'dzmd_ctp', 'warning')
    notify.push('信息消息', 'dzmd_ctp', 'info')

    expect(ms.error).toBeNull()
    expect(toastMocks.error).toHaveBeenCalledTimes(1)
    expect(toastMocks.warning).toHaveBeenCalledTimes(1)
    expect(toastMocks.info).toHaveBeenCalledTimes(1)
  })

  // 契约 notify-ui: payload 为 Unix 秒级 timestamp；store 统一换算为毫秒，缺省回退本地毫秒
  it('timestamp 统一毫秒：payload 秒级 ×1000，缺省为固定墙钟的精确毫秒', () => {
    vi.useFakeTimers()
    vi.setSystemTime(new Date('2026-10-19T12:00:00Z'))
    const store = useNotifyStore()
    const tsSec = 1755300000
    store.push('透传消息', 'dzmd_ctp', 'error', false, tsSec)
    expect(store.items[0].timestamp).toBe(tsSec * 1000)
    store.push('本地时间消息')
    expect(store.items[1].timestamp).toBe(Date.parse('2026-10-19T12:00:00Z'))
  })
})
