import { describe, it, expect } from 'vitest'
import { mergeTdRows } from '../tdListStore'
import type { TdListRow } from '../tdListStore'

/**
 * 有界列表合并的**唯一实现**（契约 webui-ws §5 第 8/9 条 + ADR 0043 决策 7 有界窗口）。
 *
 * 委托与成交的 `mergeOrderRows` / `mergeTradeRows` 都是本函数的薄委托，差别只在 `keyOf`。
 * 本套钉住通用口径本身；两份配置的行为一致性由各自既有 spec 继续保证。
 */

/** 最小行形态：合并只读 seq/sort_key 与 keyOf 用到的字段；`id` 是纯数值业务键 */
interface Row extends TdListRow {
  id: number
  tag: string
}

function row(id: number, overrides: Partial<Row> = {}): Row {
  return {
    id,
    account_id: 'acc_a',
    date: '20260808',
    seq: id,
    sort_key: `k-${String(id).padStart(6, '0')}`,
    tag: `t${id}`,
    ...overrides,
  }
}

/** 委托口径：数值业务键 */
const numKey = (r: Row): number => r.id
/** 成交口径：字符串业务键 */
const strKey = (r: Row): string => r.tag

describe('mergeTdRows · 去重与 seq 大者胜（契约 webui-ws §5 第 8 条）', () => {
  it('同业务键只在 seq 更大时覆盖，与到达顺序无关', () => {
    const newer = row(1, { seq: 20, tag: 'newer' })
    const older = row(1, { seq: 10, tag: 'older' })

    expect(mergeTdRows([newer], [older], 100, numKey)[0].tag).toBe('newer')
    expect(mergeTdRows([older], [newer], 100, numKey)[0].tag).toBe('newer')
  })

  it('seq 相等视为重复消息，保留先到者', () => {
    const first = row(1, { seq: 7, tag: 'first' })
    const second = row(1, { seq: 7, tag: 'second' })
    expect(mergeTdRows([first], [second], 100, numKey)[0].tag).toBe('first')
  })

  it('业务键不同则两条都保留（去重只按 keyOf，不看内容）', () => {
    expect(mergeTdRows([row(1)], [row(2)], 100, numKey)).toHaveLength(2)
  })

  it('keyOf 决定去重粒度：不同 id 但 keyOf 相同 → 合并成一条', () => {
    const sameKey = (): string => 'same'
    const merged = mergeTdRows([row(1)], [row(2, { seq: 99 })], 100, sameKey)
    expect(merged).toHaveLength(1)
    expect(merged[0].seq).toBe(99)
  })

  it('空增量直接返回 existing 副本（不共享引用）', () => {
    const existing = [row(1), row(2)]
    const merged = mergeTdRows(existing, [], 100, numKey)
    expect(merged).toEqual(existing)
    expect(merged).not.toBe(existing)
  })

  it('纯函数：不修改入参数组', () => {
    const existing = [row(1)]
    const incoming = [row(2)]
    mergeTdRows(existing, incoming, 100, numKey)
    expect(existing).toHaveLength(1)
    expect(incoming).toHaveLength(1)
  })
})

describe('mergeTdRows · sort_key 字符串降序（契约 rest §2.9.6）', () => {
  it('结果按 sort_key 字符串降序（首项最大），不是简单前插', () => {
    const merged = mergeTdRows(
      [row(10, { sort_key: 'b' }), row(5, { sort_key: 'a' })],
      [row(8, { sort_key: 'a5' })],
      100,
      numKey,
    )
    expect(merged.map(r => r.sort_key)).toEqual(['b', 'a5', 'a'])
  })
})

describe('mergeTdRows · sort_key 相同时按业务键降序（tie-break 类型分派）', () => {
  it('keyOf 返回 number → 数值降序（委托口径：order_id 大者在前）', () => {
    const merged = mergeTdRows(
      [row(1, { sort_key: 'k' }), row(3, { sort_key: 'k' })],
      [row(2, { sort_key: 'k' })],
      100,
      numKey,
    )
    expect(merged.map(r => r.id)).toEqual([3, 2, 1])
  })

  it('keyOf 返回 string → 字典降序（成交口径：复合业务键大者在前）', () => {
    const merged = mergeTdRows(
      [row(1, { sort_key: 'k', tag: 'a' }), row(3, { sort_key: 'k', tag: 'c' })],
      [row(2, { sort_key: 'k', tag: 'b' })],
      100,
      strKey,
    )
    expect(merged.map(r => r.tag)).toEqual(['c', 'b', 'a'])
  })

  it('字符串键按字典序比较而非数值长度（"t9" > "t10"）', () => {
    const merged = mergeTdRows(
      [row(1, { sort_key: 'k', tag: 't10' })],
      [row(2, { sort_key: 'k', tag: 't9' })],
      100,
      strKey,
    )
    expect(merged.map(r => r.tag)).toEqual(['t9', 't10'])
  })

  it('数值键按数值而非字典序（"10" 字典序小于 "9"，数值 10 更大）', () => {
    // 若实现误用 String() 比较，会得到 [10, 9]，本用例即失败
    const merged = mergeTdRows(
      [row(9, { sort_key: 'k' })],
      [row(10, { sort_key: 'k' })],
      100,
      numKey,
    )
    expect(merged.map(r => r.id)).toEqual([10, 9])
  })
})

describe('mergeTdRows · 有界窗口截断（ADR 0043 决策 7）', () => {
  it('超出容量时丢弃 sort_key 最小的行（= 最旧）', () => {
    const existing = Array.from({ length: 5 }, (_, i) => row(i + 1))
    const merged = mergeTdRows(existing, [row(99)], 5, numKey)

    expect(merged).toHaveLength(5)
    expect(merged[0]).toEqual(row(99))
    expect(merged.some(r => r.id === 1)).toBe(false)
  })

  it('正好等于容量时不截断（边界：不是 >= 就删）', () => {
    const existing = Array.from({ length: 4 }, (_, i) => row(i + 1))
    expect(mergeTdRows(existing, [row(99)], 5, numKey)).toHaveLength(5)
  })
})
