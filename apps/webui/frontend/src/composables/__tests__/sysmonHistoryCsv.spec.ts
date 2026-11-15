import { describe, it, expect } from 'vitest'
import {
  escapeCsvField,
  historyCsv,
  historyCsvFilename,
  SYSMON_HISTORY_CSV_HEADER,
} from '../sysmonHistoryCsv'
import type { SysmonHistoryPoint } from '@/types/api'

// #327：历史序列导出的纯函数判据（内容 = 当前指标 + 当前范围的全部记录窗口）。
function point(overrides: Partial<SysmonHistoryPoint> = {}): SysmonHistoryPoint {
  return { ts: 1_790_000_000, min: 0.1, max: 0.5, avg: 0.3, last: 0.2, n: 5, ...overrides }
}

describe('historyCsv（#327）', () => {
  it('表头取契约 §6.1 桶字段的子集（ts → 导出列名 timestamp；last 不导出）', () => {
    expect(SYSMON_HISTORY_CSV_HEADER).toEqual(['timestamp', 'min', 'avg', 'max', 'n'])
    expect(historyCsv([]).split('\r\n')[0]).toBe('timestamp,min,avg,max,n')
  })

  it('按接口原值导出（不做展示档位四舍五入）', () => {
    const csv = historyCsv([point({ min: 0.123456789, avg: 0.300000001, max: 0.987654321, n: 42 })])
    expect(csv).toContain('1790000000,0.123456789,0.300000001,0.987654321,42')
  })

  it('null（该桶无有效样本）导出为空字段，不写 0（不可用 ≠ 0）', () => {
    const csv = historyCsv([point({ min: null, max: null, avg: null, n: null })])
    expect(csv).toContain('1790000000,,,,\r\n')
    expect(csv).not.toContain(',0,0,0,0')
  })

  it('逐桶导出且顺序与接口一致，行尾 CRLF（Excel 可直接打开）', () => {
    const csv = historyCsv([point({ ts: 1_790_000_000 }), point({ ts: 1_790_000_060 })])
    const lines = csv.split('\r\n')
    // 表头 + 2 行 + 末尾空串（每行以 CRLF 结束）
    expect(lines).toHaveLength(4)
    expect(lines[1]!.startsWith('1790000000,')).toBe(true)
    expect(lines[2]!.startsWith('1790000060,')).toBe(true)
    expect(lines[3]).toBe('')
  })

  it('字段转义遵循 RFC 4180（逗号 / 双引号 / 换行时包裹并把内部双引号翻倍）', () => {
    expect(escapeCsvField('plain')).toBe('plain')
    expect(escapeCsvField('a,b')).toBe('"a,b"')
    expect(escapeCsvField('a"b')).toBe('"a""b"')
    expect(escapeCsvField('a\nb')).toBe('"a\nb"')
  })

  it('文件名带指标 key 与查询区间，且把非法字符折叠为 `-`', () => {
    expect(historyCsvFilename('cpu.usage_ratio', 21600)).toBe(
      'sysmon-history-cpu-usage-ratio-21600s.csv',
    )
    expect(historyCsvFilename('shm_dir.total_bytes', 3600)).toBe(
      'sysmon-history-shm-dir-total-bytes-3600s.csv',
    )
    // 契约 metric key 目前只含字母/数字/._，这里锁住防御性替换（Windows 非法字符）
    expect(historyCsvFilename('weird/key:with*chars?', 60)).toBe(
      'sysmon-history-weird-key-with-chars-60s.csv',
    )
  })
})
