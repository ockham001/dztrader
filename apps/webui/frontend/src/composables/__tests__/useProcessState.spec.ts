import { describe, it, expect } from 'vitest'
import { processStateText, processStateColor, isStateIdle } from '../useProcessState'

describe('processStateText', () => {
  it('PascalCase key 映射为中文文案（契约 04 ChildState）', () => {
    expect(processStateText('Starting')).toBe('启动中')
    expect(processStateText('Running')).toBe('运行中')
    expect(processStateText('Stopping')).toBe('停止中')
    expect(processStateText('Stopped')).toBe('已停止')
    expect(processStateText('Crashed')).toBe('已崩溃')
  })
  it('未映射值原样返回（现有行为）', () => {
    expect(processStateText('unknown_state')).toBe('unknown_state')
  })
})

describe('processStateColor', () => {
  it('PascalCase 映射为 CSS 变量（现有行为）', () => {
    expect(processStateColor('Running')).toBe('var(--status-success-default)')
    expect(processStateColor('Crashed')).toBe('var(--status-error-default)')
    expect(processStateColor('Starting')).toBe('var(--status-alert-default)')
    expect(processStateColor('Stopping')).toBe('var(--status-alert-default)')
    expect(processStateColor('Stopped')).toBe('var(--text-tertiary)')
    expect(processStateColor('')).toBe('var(--text-tertiary)')
  })
})

describe('isStateIdle', () => {
  it('Running + offline 为 Idle（契约 F-C4）', () => {
    expect(isStateIdle('Running', 'offline')).toBe(true)
    expect(isStateIdle('Running', 'online')).toBe(false)
    expect(isStateIdle('Stopped', 'offline')).toBe(false)
    expect(isStateIdle('', '')).toBe(false)
  })
})
