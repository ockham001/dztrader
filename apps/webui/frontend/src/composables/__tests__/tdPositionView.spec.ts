import { describe, it, expect } from 'vitest'
import {
  TD_POSITION_DETAIL_COLUMNS,
  comparePositionRows,
  formatSignedVolume,
  formatTdDate,
  groupPositionRows,
  mergePositionRows,
  positionDetailTexts,
  reconcilePositionBaseline,
  tdPositionBusinessKey,
  tdPositionDirectionText,
} from '../tdPositionView'
import type { TdPositionRow } from '@/types/api'

/**
 * 持仓展示/合并纯函数（契约 rest §2.9.2/§2.9.4/§2.9.6，契约 webui-ws §5 第 8/12 条）。
 *
 * 这里固定的是「持仓为什么不能复用列表工厂的合并」的具体差异：
 * 无 sort_key 的服务端顺序、绝对态替换、无容量截断、按 (账户, 合约) 分组的净仓口径。
 */
function position(overrides: Partial<TdPositionRow> = {}): TdPositionRow {
  return {
    account_id: 'acc_a',
    date: '20260808',
    instrument_id: 'rb2601',
    exchange_id: 'SHFE',
    direction: 'Long',
    volume: 2,
    frozen_volume: 0,
    today_volume: 1,
    yd_volume: 1,
    price: 3512.35,
    price_decimals: 1,
    seq: 10,
    ...overrides,
  }
}

describe('tdPositionBusinessKey / comparePositionRows（契约 rest §2.9.2/§2.9.6）', () => {
  it('业务键 = account|instrument|direction（同合约多空不合并）', () => {
    const long = position({ direction: 'Long' })
    const short = position({ direction: 'Short' })
    expect(tdPositionBusinessKey(long)).toBe('acc_a|rb2601|Long')
    expect(tdPositionBusinessKey(short)).toBe('acc_a|rb2601|Short')
    expect(tdPositionBusinessKey(long)).not.toBe(tdPositionBusinessKey(short))
  })

  it('顺序 = (account, instrument, direction) 升序，direction 全序 Short < Net < Long', () => {
    const rows = [
      position({ account_id: 'b', instrument_id: 'a', direction: 'Long' }),
      position({ account_id: 'a', instrument_id: 'b', direction: 'Short' }),
      position({ account_id: 'a', instrument_id: 'a', direction: 'Long' }),
      position({ account_id: 'a', instrument_id: 'a', direction: 'Net' }),
      position({ account_id: 'a', instrument_id: 'a', direction: 'Short' }),
    ]
    const sorted = [...rows].sort(comparePositionRows)
    expect(sorted.map(r => `${r.account_id}/${r.instrument_id}/${r.direction}`)).toEqual([
      'a/a/Short',
      'a/a/Net',
      'a/a/Long',
      'a/b/Short',
      'b/a/Long',
    ])
  })
})

describe('mergePositionRows（契约 webui-ws §5 第 8 条 + 绝对态）', () => {
  it('业务键去重 + 同键 seq 大者胜（与到达顺序无关）', () => {
    const merged = mergePositionRows(
      [position({ seq: 10, volume: 2 })],
      [position({ seq: 12, volume: 5 }), position({ seq: 8, volume: 99 })],
    )
    expect(merged).toHaveLength(1)
    expect(merged[0].seq).toBe(12)
    expect(merged[0].volume).toBe(5)
  })

  it('结果按服务端顺序排序（不是到达顺序、不是 sort_key）', () => {
    const merged = mergePositionRows([], [
      position({ instrument_id: 'rb2601' }),
      position({ instrument_id: 'ag2601' }),
    ])
    expect(merged.map(r => r.instrument_id)).toEqual(['ag2601', 'rb2601'])
  })

  it('不做容量截断：绝对态丢行就是假空仓', () => {
    const many = Array.from({ length: 5000 }, (_, i) =>
      position({ instrument_id: `i${String(i).padStart(5, '0')}` }))
    expect(mergePositionRows([], many)).toHaveLength(5000)
  })

  it('纯函数：不修改入参', () => {
    const existing = [position({ seq: 10 })]
    const incoming = [position({ seq: 20, volume: 9 })]
    mergePositionRows(existing, incoming)
    expect(existing[0].seq).toBe(10)
    expect(incoming[0].seq).toBe(20)
  })
})

describe('reconcilePositionBaseline（绝对态替换式合并）', () => {
  it('基线没有返回的业务键被丢弃（幽灵持仓不保留）', () => {
    const baseline = [position({ instrument_id: 'rb2601', seq: 10 })]
    const live = [
      position({ instrument_id: 'rb2601', seq: 10 }),
      // 旧快照里已有的行：seq 不高于基线水位 → 基线没返回说明它已消失
      position({ instrument_id: 'ag2601', seq: 5 }),
    ]
    const merged = reconcilePositionBaseline(baseline, live)
    expect(merged.map(r => r.instrument_id)).toEqual(['rb2601'])
  })

  it('本地行比基线新（seq 更高）时保留本地版本', () => {
    const baseline = [position({ instrument_id: 'rb2601', seq: 10, volume: 2 })]
    const live = [position({ instrument_id: 'rb2601', seq: 30, volume: 7 })]
    const merged = reconcilePositionBaseline(baseline, live)
    expect(merged[0].seq).toBe(30)
    expect(merged[0].volume).toBe(7)
  })

  it('本地新键但 seq 不高于该账户基线水位 → 丢弃（基线查询时它还不存在）', () => {
    const baseline = [position({ instrument_id: 'rb2601', seq: 50 })]
    const live = [
      position({ instrument_id: 'rb2601', seq: 50 }),
      position({ instrument_id: 'ag2601', seq: 20 }),
    ]
    const merged = reconcilePositionBaseline(baseline, live)
    expect(merged.map(r => r.instrument_id)).toEqual(['rb2601'])
  })

  it('本地新键且 seq 高于基线水位 → 保留（基线查询先于它提交）', () => {
    const baseline = [position({ instrument_id: 'rb2601', seq: 10 })]
    const live = [position({ instrument_id: 'ag2601', seq: 11 })]
    const merged = reconcilePositionBaseline(baseline, live)
    expect(merged.map(r => r.instrument_id)).toEqual(['ag2601', 'rb2601'])
  })

  it('基线为空且无本地行 → 空数组（真空仓）', () => {
    expect(reconcilePositionBaseline([], [])).toEqual([])
  })
})

describe('groupPositionRows（一行 = 账户 × 合约，用户确认口径）', () => {
  it('同一合约多空合成一行，净仓 = 多 − 空', () => {
    const groups = groupPositionRows([
      position({ direction: 'Long', volume: 5 }),
      position({ direction: 'Short', volume: 2 }),
    ])
    expect(groups).toHaveLength(1)
    expect(groups[0].netVolume).toBe(3)
    expect(groups[0].long?.volume).toBe(5)
    expect(groups[0].short?.volume).toBe(2)
  })

  it('不同账户的同一合约是两行（行 key 含账户）', () => {
    const groups = groupPositionRows([
      position({ account_id: 'acc_a', volume: 1 }),
      position({ account_id: 'acc_b', volume: 2 }),
    ])
    expect(groups.map(g => g.key)).toEqual(['acc_a|rb2601', 'acc_b|rb2601'])
  })

  it('不同合约是两行；输出顺序沿用输入的持仓排序（面板稳定）', () => {
    const groups = groupPositionRows([
      position({ account_id: 'acc_a', instrument_id: 'ag2601' }),
      position({ account_id: 'acc_a', instrument_id: 'rb2601' }),
    ])
    expect(groups.map(g => g.instrument_id)).toEqual(['ag2601', 'rb2601'])
  })

  it('只有 Net 方向时净仓取该值并标记 hasNetDirection（不做多空拆分）', () => {
    const groups = groupPositionRows([position({ direction: 'Net', volume: 4 })])
    expect(groups[0].netVolume).toBe(4)
    expect(groups[0].hasNetDirection).toBe(true)
    expect(groups[0].long).toBeNull()
    expect(groups[0].short).toBeNull()
  })

  it('Net 与多空明细同时出现属异常数据：明细优先，不相加放大净仓', () => {
    const groups = groupPositionRows([
      position({ direction: 'Net', volume: 4 }),
      position({ direction: 'Long', volume: 6 }),
    ])
    expect(groups[0].netVolume).toBe(6)
    expect(groups[0].hasNetDirection).toBe(true)
    expect(groups[0].long?.volume).toBe(6)
    expect(groups[0].short).toBeNull()
  })

  it('数据日期取多空两条中较大者（同账户锚点日通常相同）', () => {
    const groups = groupPositionRows([
      position({ direction: 'Long', date: '20260807' }),
      position({ direction: 'Short', date: '20260808' }),
    ])
    expect(groups[0].date).toBe('20260808')
  })

  it('同方向异常重复行取 seq 较大者（与合并规则一致）', () => {
    const groups = groupPositionRows([
      position({ direction: 'Long', seq: 10, volume: 2 }),
      position({ direction: 'Long', seq: 20, volume: 7 }),
    ])
    // 净仓是两行之和的口径只对"多空各一条"成立；异常重复不制造双计：取大者后净仓 = 7
    expect(groups[0].netVolume).toBe(7)
    expect(groups[0].long?.volume).toBe(7)
  })
})

describe('详情列与格式化（契约 webui-ws §5 第 12 条精度口径）', () => {
  it('详情列固定 10 列：多方 5 + 空方 5', () => {
    expect(TD_POSITION_DETAIL_COLUMNS).toEqual([
      '多总', '多今', '多昨', '多冻结', '多均价',
      '空总', '空今', '空昨', '空冻结', '空均价',
    ])
  })

  it('两侧都有持仓时输出 10 个值，均价按行内 price_decimals 格式化', () => {
    const [group] = groupPositionRows([
      position({
        direction: 'Long', volume: 5, today_volume: 2, yd_volume: 3, frozen_volume: 1,
        price: 3512.35, price_decimals: 1,
      }),
      position({
        direction: 'Short', volume: 2, today_volume: 1, yd_volume: 1, frozen_volume: 0,
        price: 3500, price_decimals: 0,
      }),
    ])
    expect(positionDetailTexts(group)).toEqual([
      '5', '2', '3', '1', '3512.3',
      '2', '1', '1', '0', '3500',
    ])
  })

  it('缺一侧时该侧 5 列全部显示占位（不显示 0 冒充明细）', () => {
    const [group] = groupPositionRows([position({ direction: 'Short', volume: 4 })])
    expect(positionDetailTexts(group).slice(0, 5)).toEqual(['—', '—', '—', '—', '—'])
    expect(positionDetailTexts(group)[5]).toBe('4')
  })

  it('data 日期展示为 YYYY-MM-DD，非法值显示占位', () => {
    expect(formatTdDate('20260808')).toBe('2026-08-08')
    expect(formatTdDate('')).toBe('—')
    expect(formatTdDate('2026-08-08')).toBe('—')
  })

  it('净仓符号：正数带 +、负数原样、零为 0（零是合法真实值）', () => {
    expect(formatSignedVolume(3)).toBe('+3')
    expect(formatSignedVolume(-2)).toBe('-2')
    expect(formatSignedVolume(0)).toBe('0')
  })

  it('方向文案：Long / Short / Net → 多 / 空 / 净', () => {
    expect(tdPositionDirectionText('Long')).toBe('多')
    expect(tdPositionDirectionText('Short')).toBe('空')
    expect(tdPositionDirectionText('Net')).toBe('净')
  })
})
