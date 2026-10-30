import { describe, it, expect } from 'vitest'
import { SEMANTIC_HINT_TEXT, semanticHintText } from '../sysmonSemanticHint'

// #207 B2：契约只规定 key→文案映射，未规定语言；dzsysmon 发英文，中文界面按稳定 key 映射。
describe('semanticHintText：按稳定 key 映射中文文案（#207 B2）', () => {
  const RAW_LOAD_AVERAGE =
    'processor queue length proxy, not a unix load average; do not compare the two on the same axis'
  const RAW_DISK_IO =
    'latency is the average time per completed request, queue wait included; queue depth is the ' +
    'instantaneous in-flight request count at the measurement tick, not an interval average'

  it('契约定义的两个 key 都映射成中文（不再是英文原文）', () => {
    const load = semanticHintText('cpu.load_average', RAW_LOAD_AVERAGE)
    const disk = semanticHintText('disk_io', RAW_DISK_IO)

    for (const text of [load, disk]) {
      expect(text).not.toBe('')
      // 中文文案：含 CJK 字符、且不残留英文原句
      expect(text).toMatch(/[\u4e00-\u9fa5]/)
    }
    expect(load).not.toBe(RAW_LOAD_AVERAGE)
    expect(disk).not.toBe(RAW_DISK_IO)
  })

  it('中文文案保留原文的口径事实（不是空泛翻译）', () => {
    // Windows 负载代理：必须点明「不是 Unix load average / 不要同轴比较」
    expect(semanticHintText('cpu.load_average', RAW_LOAD_AVERAGE)).toContain('负载')
    expect(semanticHintText('cpu.load_average', RAW_LOAD_AVERAGE)).toMatch(/队列/)
    // disk_io：延迟含排队、队列深度是瞬时值不是区间平均
    expect(semanticHintText('disk_io', RAW_DISK_IO)).toMatch(/延迟/)
    expect(semanticHintText('disk_io', RAW_DISK_IO)).toMatch(/队列深度/)
    expect(semanticHintText('disk_io', RAW_DISK_IO)).toMatch(/不是区间平均/)
  })

  it('未命中 key 回退发送方原文（新 key 不吞提示）', () => {
    expect(semanticHintText('some.new_metric', 'windows counters are per physical disk'))
      .toBe('windows counters are per physical disk')
  })

  it('发送方无提示（空串 / 空白）时返回空串，视图据此不渲染', () => {
    expect(semanticHintText('disk_io', '')).toBe('')
    expect(semanticHintText('disk_io', '   ')).toBe('')
    // 空串不因 key 命中而凭空中文
    expect(semanticHintText('cpu.load_average', '')).toBe('')
  })

  it('映射表覆盖契约当前声明的全部 key（新增 key 时这条会提醒同步中文文案）', () => {
    // 契约 sysmon §2.5 当前版本定义的两个 key
    expect(Object.keys(SEMANTIC_HINT_TEXT).sort()).toEqual(['cpu.load_average', 'disk_io'])
    for (const text of Object.values(SEMANTIC_HINT_TEXT)) {
      expect(text.trim()).not.toBe('')
      expect(text).toMatch(/[\u4e00-\u9fa5]/)
    }
  })
})
