import { describe, it, expect } from 'vitest'
import { mergeTradeRows, tdTradeBusinessKey } from '../tdTradeView'
import type { TdTradeRow } from '@/types/api'

/** 构造成交行（只覆盖断言关心的字段，其余给稳定默认值） */
function trade(overrides: Partial<TdTradeRow> = {}): TdTradeRow {
  const tradeId = overrides.trade_id ?? 't1'
  return {
    account_id: 'account_001',
    date: '20260808',
    trade_id: tradeId,
    order_id: 10231,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price: 3512.3,
    volume: 1,
    time: 32405,
    price_decimals: 1,
    seq: 1,
    sort_key: `k-${tradeId}`,
    ...overrides,
  }
}

describe('tdTradeView · 业务键（契约 rest §2.9.6：account_id/date/exchange_id/trade_id）', () => {
  it('业务键由四个字段以 "|" 拼接（与委托的 order_id 同口径）', () => {
    expect(tdTradeBusinessKey(trade())).toBe('account_001|20260808|SHFE|t1')
  })

  it('多交易所同 trade_id 不合并（交易所是业务键的一部）', () => {
    const shfe = trade({ trade_id: '1', exchange_id: 'SHFE' })
    const czce = trade({ trade_id: '1', exchange_id: 'CZCE' })
    expect(tdTradeBusinessKey(shfe)).not.toBe(tdTradeBusinessKey(czce))

    const merged = mergeTradeRows([shfe], [czce])
    expect(merged).toHaveLength(2)
  })

  it('不同账户同 trade_id 不合并', () => {
    const a = trade({ account_id: 'account_001' })
    const b = trade({ account_id: 'account_002' })
    expect(tdTradeBusinessKey(a)).not.toBe(tdTradeBusinessKey(b))
    expect(mergeTradeRows([a], [b])).toHaveLength(2)
  })

  it('不同交易日不合并（日切后的行不与上一交易日混同）', () => {
    expect(tdTradeBusinessKey(trade({ date: '20260808' })))
      .not.toBe(tdTradeBusinessKey(trade({ date: '20260809' })))
  })
})

describe('tdTradeView · 合并（契约 webui-ws §5 第 8/9 条）', () => {
  it('同业务键去重：seq 大者胜，且与到达顺序无关', () => {
    const newer = trade({ seq: 20, volume: 2 })
    const older = trade({ seq: 10, volume: 1 })

    expect(mergeTradeRows([newer], [older])[0].seq).toBe(20)
    expect(mergeTradeRows([older], [newer])[0].seq).toBe(20)
    expect(mergeTradeRows([newer], [older])[0].volume).toBe(2)
  })

  it('同键 seq 相等视为重复消息，保留先到者（不反复替换）', () => {
    const first = trade({ seq: 7, volume: 1 })
    const second = trade({ seq: 7, volume: 9 })
    expect(mergeTradeRows([first], [second])[0].volume).toBe(1)
  })

  it('结果始终按 sort_key 字符串降序（增量行按 sort_key 插入，不是简单前插）', () => {
    const existing = [trade({ trade_id: 't10', sort_key: 'b' }), trade({ trade_id: 't5', sort_key: 'a' })]
    const merged = mergeTradeRows(existing, [trade({ trade_id: 't7', sort_key: 'a5' })])
    expect(merged.map(r => r.sort_key)).toEqual(['b', 'a5', 'a'])
  })

  it('sort_key 相同时按业务键降序，顺序稳定可测', () => {
    const merged = mergeTradeRows(
      [trade({ trade_id: 't1', sort_key: 'k' })],
      [trade({ trade_id: 't3', sort_key: 'k' }), trade({ trade_id: 't2', sort_key: 'k' })],
    )
    expect(merged.map(r => r.trade_id)).toEqual(['t3', 't2', 't1'])
  })

  it('有界窗口：超出容量时丢弃 sort_key 最小的行（最旧）', () => {
    const existing = Array.from({ length: 5 }, (_, i) =>
      trade({ trade_id: `t${i + 1}`, sort_key: `k${String(i + 1).padStart(2, '0')}` }))
    const merged = mergeTradeRows(existing, [trade({ trade_id: 't99', sort_key: 'k99' })], 5)

    expect(merged).toHaveLength(5)
    expect(merged[0].trade_id).toBe('t99')
    expect(merged.some(r => r.trade_id === 't1')).toBe(false)
  })

  it('空增量直接返回 existing 副本（不触发重排）', () => {
    const existing = [trade({ trade_id: 't1', sort_key: 'a' }), trade({ trade_id: 't2', sort_key: 'b' })]
    const merged = mergeTradeRows(existing, [])
    expect(merged).toEqual(existing)
    expect(merged).not.toBe(existing)
  })

  it('合并不修改入参（纯函数，store 安全替换）', () => {
    const existing = [trade({ trade_id: 't1', sort_key: 'a' })]
    const incoming = [trade({ trade_id: 't2', sort_key: 'b' })]
    mergeTradeRows(existing, incoming)
    expect(existing).toHaveLength(1)
    expect(incoming).toHaveLength(1)
  })
})
