import { describe, it, expect } from 'vitest'
import {
  formatDepositWithdraw,
  formatFundDateLabel,
  formatTdMoney,
  fundStatusText,
  isTdAccountOnline,
} from '../tdFundView'

/**
 * 资金展示纯函数（契约 rest §2.9.4，契约 webui-ws §5 第 12/13 条）。
 *
 * 这里固定的是「无数据不是 0」的展示口径：
 * - 金额固定 2 位小数 + 千分位；null / 非有限值显示占位 `—`；
 * - 出入金两侧分别格式化，0 是真实值（显示 0）而不是占位；
 * - 数据日期标签去重升序后全部列出（多账户锚点日可能不同）。
 */
describe('formatTdMoney（金额：2 位小数 + 千分位，null 用占位）', () => {
  it('千分位与固定 2 位小数', () => {
    expect(formatTdMoney(1_234_567.89)).toBe('1,234,567.89')
    expect(formatTdMoney(1000)).toBe('1,000.00')
    expect(formatTdMoney(1234.5)).toBe('1,234.50')
    expect(formatTdMoney(0.5)).toBe('0.50')
  })

  it('第三位小数四舍五入（toFixed 口径）', () => {
    expect(formatTdMoney(1234.567)).toBe('1,234.57')
    expect(formatTdMoney(0.004)).toBe('0.00')
  })

  it('0 是合法真实值，显示 0.00 而不是占位', () => {
    expect(formatTdMoney(0)).toBe('0.00')
  })

  it('负金额保留负号并按千分位格式化', () => {
    expect(formatTdMoney(-1234.5)).toBe('-1,234.50')
  })

  it('null / NaN / Infinity 显示占位（不得用 0 冒充无数据）', () => {
    expect(formatTdMoney(null)).toBe('—')
    expect(formatTdMoney(Number.NaN)).toBe('—')
    expect(formatTdMoney(Number.POSITIVE_INFINITY)).toBe('—')
    expect(formatTdMoney(Number.NEGATIVE_INFINITY)).toBe('—')
  })
})

describe('formatDepositWithdraw（出入金：入 +X / 出 -Y）', () => {
  it('两侧都有金额时按「入 +… / 出 -…」拼接', () => {
    expect(formatDepositWithdraw(50_000, 1234.5)).toBe('入 +50,000.00 / 出 -1,234.50')
  })

  it('0 显示为 0 而不是占位（当日无出入金是真实值）', () => {
    expect(formatDepositWithdraw(0, 0)).toBe('入 +0 / 出 -0')
  })

  it('任一为 null 时该段显示占位', () => {
    expect(formatDepositWithdraw(null, 1234.5)).toBe('入 — / 出 -1,234.50')
    expect(formatDepositWithdraw(1000, null)).toBe('入 +1,000.00 / 出 —')
    expect(formatDepositWithdraw(null, null)).toBe('入 — / 出 —')
  })
})

describe('formatFundDateLabel（面板级数据日期标签）', () => {
  it('去重升序后全部列出（多账户锚点日不同不得只标一个）', () => {
    expect(formatFundDateLabel(['20260808', '20260807', '20260808']))
      .toBe('数据日期 2026-08-07 / 2026-08-08')
  })

  it('单日期', () => {
    expect(formatFundDateLabel(['20260808'])).toBe('数据日期 2026-08-08')
  })

  it('空集合返回空串（调用方据此不渲染标签）', () => {
    expect(formatFundDateLabel([])).toBe('')
  })
})

describe('fundStatusText / isTdAccountOnline（账户在线状态，唯一谓词）', () => {
  it('在 online_accounts 中 = 在线，否则 = 离线', () => {
    expect(fundStatusText('acc_a', ['acc_a', 'acc_b'])).toBe('在线')
    expect(fundStatusText('acc_c', ['acc_a', 'acc_b'])).toBe('离线')
  })

  it('无在线账户时全部离线', () => {
    expect(fundStatusText('acc_a', [])).toBe('离线')
  })

  it('在线谓词与状态文案同源：同一账户的判定与文案始终一致', () => {
    const onlineAccounts = ['acc_a']
    // 状态标签样式（DashboardView）消费谓词、文案消费 fundStatusText：两者必须给出同一结论，
    // 否则会出现「文案=在线、样式=离线」这种自相矛盾的单元格。
    expect(isTdAccountOnline('acc_a', onlineAccounts)).toBe(true)
    expect(fundStatusText('acc_a', onlineAccounts)).toBe('在线')
    expect(isTdAccountOnline('acc_b', onlineAccounts)).toBe(false)
    expect(fundStatusText('acc_b', onlineAccounts)).toBe('离线')
  })
})
