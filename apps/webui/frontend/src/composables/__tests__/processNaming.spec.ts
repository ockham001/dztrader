import { describe, it, expect } from 'vitest'
import { processCategory } from '../processNaming'

describe('processCategory', () => {
  it('按进程名前缀/精确名归类（契约总则 §5 命名规则）', () => {
    expect(processCategory('dztraderd')).toBe('master')
    expect(processCategory('dzmd_ctp')).toBe('md')
    expect(processCategory('dzmd_xtp')).toBe('md')
    expect(processCategory('dztd_ctp')).toBe('td')
    expect(processCategory('dzsysmon')).toBe('sysmon')
    expect(processCategory('stg_demo')).toBe('strategy')
  })

  it('只按名字开头判定，前缀出现在中间不算（避免 includes 式误判）', () => {
    expect(processCategory('my_dzmd_ctp')).toBe('unknown')
    expect(processCategory('x-stg_demo')).toBe('unknown')
  })

  it('dzweb（可重命名）与无法识别的进程名返回 unknown，由调用方按 self 身份判定 WebUI', () => {
    expect(processCategory('dzweb')).toBe('unknown')
    expect(processCategory('some_tool')).toBe('unknown')
    expect(processCategory('')).toBe('unknown')
  })
})
