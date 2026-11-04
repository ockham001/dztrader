import { describe, it, expect } from 'vitest'
import {
  mergeOrderRows,
  formatTdPrice,
  formatTdAvgPrice,
  formatTdTime,
  tdDirectionText,
  tdPositionEffectText,
  tdOrderStatusText,
  tdPriceTypeText,
} from '../tdOrderView'
import type { TdOrderRow } from '@/types/api'

/** 构造委托行（只覆盖断言关心的字段，其余给稳定默认值） */
function row(overrides: Partial<TdOrderRow> = {}): TdOrderRow {
  const orderId = overrides.order_id ?? 1
  return {
    account_id: 'account_001',
    date: '20260808',
    order_id: orderId,
    strategy_id: 'stg_demo',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    position_effect: 'Open',
    price_type: 'Limit',
    status: 'PartTraded',
    price: 3512.5,
    volume: 2,
    volume_traded: 1,
    remark: '',
    time: 32400,
    avg_price: 3512.3,
    price_decimals: 1,
    avg_price_decimals: 3,
    seq: 1,
    // 默认给一个与 order_id 一致、可直接比较的 sort_key（真实实现由服务端产生）
    sort_key: `k-${String(orderId).padStart(5, '0')}`,
    ...overrides,
  }
}

describe('tdOrderView · 精度格式化（契约 rest §2.9.6 / webui-ws §5 第 12 条）', () => {
  it('委托价按行内 price_decimals 做 toFixed（不自行推断小数位）', () => {
    expect(formatTdPrice(3512.5, 1)).toBe('3512.5')
    expect(formatTdPrice(3512.5, 3)).toBe('3512.500')
    expect(formatTdPrice(3512.5, 0)).toBe('3513')
    expect(formatTdPrice(0.00001, 5)).toBe('0.00001')
  })

  it('成交均价按 avg_price_decimals 做 toFixed', () => {
    expect(formatTdAvgPrice(3512.3, 3)).toBe('3512.300')
    expect(formatTdAvgPrice(3512.3, 2)).toBe('3512.30')
  })

  it('avg_price 为 null（无成交）显示占位破折号，不显示 0', () => {
    expect(formatTdAvgPrice(null, 3)).toBe('—')
  })

  it('非有限值显示占位（不用 NaN / Infinity 冒充价格）', () => {
    expect(formatTdPrice(Number.NaN, 2)).toBe('—')
    expect(formatTdPrice(Number.POSITIVE_INFINITY, 2)).toBe('—')
    expect(formatTdAvgPrice(Number.NaN, 2)).toBe('—')
  })
})

describe('tdOrderView · 时间格式化（契约 rest §2.9.3 time = 距午夜秒数）', () => {
  it('0–86399 秒格式化为 HH:MM:SS（不做时区转换）', () => {
    expect(formatTdTime(0)).toBe('00:00:00')
    expect(formatTdTime(32400)).toBe('09:00:00')
    expect(formatTdTime(32405)).toBe('09:00:05')
    expect(formatTdTime(86399)).toBe('23:59:59')
  })

  it('越界 / 非整数 / 非有限值显示占位（不猜测）', () => {
    expect(formatTdTime(-1)).toBe('—')
    expect(formatTdTime(86400)).toBe('—')
    expect(formatTdTime(Number.NaN)).toBe('—')
    expect(formatTdTime(1.5)).toBe('—')
  })
})

describe('tdOrderView · 枚举中文文案（契约 rest §2.9.3 枚举取值）', () => {
  it('方向', () => {
    expect(tdDirectionText('Long')).toBe('买')
    expect(tdDirectionText('Short')).toBe('卖')
    expect(tdDirectionText('Net')).toBe('净')
  })

  it('开平', () => {
    expect(tdPositionEffectText('Auto')).toBe('自动')
    expect(tdPositionEffectText('Open')).toBe('开仓')
    expect(tdPositionEffectText('Close')).toBe('平仓')
    expect(tdPositionEffectText('CloseToday')).toBe('平今')
    expect(tdPositionEffectText('CloseYesterday')).toBe('平昨')
  })

  it('委托状态', () => {
    expect(tdOrderStatusText('Submitting')).toBe('提交中')
    expect(tdOrderStatusText('NotTraded')).toBe('未成交')
    expect(tdOrderStatusText('PartTraded')).toBe('部分成交')
    expect(tdOrderStatusText('AllTraded')).toBe('全部成交')
    expect(tdOrderStatusText('Cancelled')).toBe('已撤单')
    expect(tdOrderStatusText('Rejected')).toBe('已拒绝')
  })

  it('价格类型', () => {
    expect(tdPriceTypeText('Limit')).toBe('限价')
    expect(tdPriceTypeText('Market')).toBe('市价')
    expect(tdPriceTypeText('Stop')).toBe('止损')
    expect(tdPriceTypeText('Fak')).toBe('FAK')
    expect(tdPriceTypeText('Fok')).toBe('FOK')
    expect(tdPriceTypeText('Rfq')).toBe('询价')
  })

  it('未知取值原样回显（不吞成空白）', () => {
    expect(tdDirectionText('Hedge' as never)).toBe('Hedge')
    expect(tdOrderStatusText('Unknown' as never)).toBe('Unknown')
  })
})

describe('tdOrderView · sort_key 降序插入与合并（契约 rest §2.9.6）', () => {
  it('插入位置由 sort_key 决定，不是简单前插', () => {
    const rows = [row({ order_id: 10, sort_key: 'b' }), row({ order_id: 5, sort_key: 'a' })]
    expect(mergeOrderRows(rows, [row({ order_id: 7, sort_key: 'c' })]).map(r => r.sort_key))
      .toEqual(['c', 'b', 'a'])
    expect(mergeOrderRows(rows, [row({ order_id: 8, sort_key: 'a0' })]).map(r => r.sort_key))
      .toEqual(['b', 'a0', 'a'])
  })

  it('同业务键 order_id 去重：seq 大者胜，且与到达顺序无关', () => {
    const newer = row({ order_id: 100, seq: 20, status: 'AllTraded', volume_traded: 2 })
    const older = row({ order_id: 100, seq: 10, status: 'NotTraded', volume_traded: 1 })

    expect(mergeOrderRows([newer], [older])[0].status).toBe('AllTraded')
    expect(mergeOrderRows([older], [newer])[0].status).toBe('AllTraded')
    expect(mergeOrderRows([older], [older])[0].seq).toBe(10)
  })

  it('合并结果始终按 sort_key 字符串降序（跨批次）', () => {
    const merged = mergeOrderRows(
      [row({ order_id: 1, sort_key: 'k2' }), row({ order_id: 2, sort_key: 'k1' })],
      [row({ order_id: 3, sort_key: 'k4' }), row({ order_id: 4, sort_key: 'k3' })],
    )
    expect(merged.map(r => r.sort_key)).toEqual(['k4', 'k3', 'k2', 'k1'])
  })

  it('有界窗口：超出上限时丢弃 sort_key 最小的行（最旧）', () => {
    const existing = Array.from({ length: 5 }, (_, i) =>
      row({ order_id: i + 1, sort_key: `k${String(i + 1).padStart(2, '0')}` }))
    const merged = mergeOrderRows(existing, [row({ order_id: 99, sort_key: 'k99' })], 5)
    expect(merged).toHaveLength(5)
    expect(merged[0].order_id).toBe(99)
    expect(merged.some(r => r.order_id === 1)).toBe(false)
  })

  it('合并不会修改入参数组（纯函数，便于 store 安全替换）', () => {
    const target = [row({ order_id: 1, sort_key: 'a' })]
    const incoming = [row({ order_id: 2, sort_key: 'b' })]
    mergeOrderRows(target, incoming)
    expect(target).toHaveLength(1)
    expect(incoming).toHaveLength(1)
  })
})
