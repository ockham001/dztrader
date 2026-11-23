import { describe, it, expect } from 'vitest'
import {
  DISABLED_BY_CONFIG_TEXT,
  unavailableReasonText,
} from '@/composables/monitorAvailability'

// #450：不可用原因文案（从已删除的 monitorLedger 拆出的纯函数）。
// 判据是**契约 §2.5 的 reason 语义**：四类空态不得混用、未知原因不得被抹成「不可用」，
// 且 disk_io 的 no_permission 必须有可操作的专属说明（去开性能计数器，而不是去查权限）。
describe('monitorAvailability 不可用原因文案（契约 sysmon §2.5）', () => {
  it('映射缺失：给出中性说明（不是空串、不是 0）', () => {
    expect(unavailableReasonText(undefined)).toBe('当前快照未提供该指标')
  })

  it('已知 reason 各自给出可读文案，且 disabled_by_config 与不可用分开', () => {
    expect(unavailableReasonText('not_implemented')).toBe('该指标将在后续版本接入')
    expect(unavailableReasonText('collect_failed')).toBe('采集失败，暂不可用')
    expect(unavailableReasonText('disabled_by_config')).toBe(DISABLED_BY_CONFIG_TEXT)
    expect(unavailableReasonText('disabled_by_config')).not.toBe(unavailableReasonText('collect_failed'))
  })

  it('no_permission 按指标语境细化：disk_io 指路性能计数器，其余给通用权限说明', () => {
    expect(unavailableReasonText('no_permission', 'disk_io'))
      .toBe('磁盘性能计数器不可用（需管理员权限或性能计数器未开启）')
    expect(unavailableReasonText('no_permission'))
      .toBe('权限不足，无法读取该指标')
  })

  it('未知 reason 原样透出（不吞诊断信息）', () => {
    expect(unavailableReasonText('some_new_reason')).toBe('some_new_reason')
    expect(unavailableReasonText('some_new_reason', 'disk_io')).toBe('some_new_reason')
  })
})
