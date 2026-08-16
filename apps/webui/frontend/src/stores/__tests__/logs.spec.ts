import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useLogsStore } from '../logs'
import { usePending, __resetForTests } from '@/composables/usePending'
import * as logsApi from '@/api/logs'

// Mock the API module
vi.mock('@/api/logs', () => ({
  logsApi: {
    files: vi.fn(),
    content: vi.fn(),
    stats: vi.fn(),
    aggregate: vi.fn(),
    timeline: vi.fn(),
    setLevel: vi.fn(),
    flush: vi.fn(),
  },
}))

describe('useLogsStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    // usePending 模块级状态（pending + timers Map）——每个用例前重置
    __resetForTests()
    vi.useFakeTimers()
  })
  afterEach(() => {
    vi.useRealTimers()
  })

  it('starts with empty state', () => {
    const store = useLogsStore()
    expect(store.files).toEqual([])
    expect(store.lines).toEqual([])
    expect(store.selectedFile).toBe('')
    expect(store.tailEnabled).toBe(false)
    expect(store.browserTab).toBe('viewer')
  })

  it('loadFiles fetches and stores files', async () => {
    const mockFiles = [
      { name: 'dztraderd.log', logger: 'dztraderd', size: 1024, mtime: '2026-07-13T14:00:00', path: '/tmp/dztraderd.log' },
    ]
    vi.mocked(logsApi.logsApi.files).mockResolvedValue(mockFiles)

    const store = useLogsStore()
    await store.loadFiles(true)

    expect(store.files).toEqual(mockFiles)
    expect(store.filesLoading).toBe(false)
  })

  it('loadContent fetches and stores content', async () => {
    const mockContent = {
      lines: [
        { n: 1, ts: '2026-07-13T14:23:45.000000000+08:00', level: 'info', logger: 'dztraderd', func: 'main', file: 'main.cpp', line: 42, pid: '1', tid: '2', msg: 'hello', raw: 'raw line', parsed: true },
      ],
      total: 1,
    }
    vi.mocked(logsApi.logsApi.content).mockResolvedValue(mockContent)

    const store = useLogsStore()
    store.selectedFile = 'dztraderd.log'
    await store.loadContent()

    expect(store.lines).toHaveLength(1)
    expect(store.lines[0].level).toBe('info')
    expect(store.totalLines).toBe(1)
  })

  it('appendLogLine adds a line for real-time tail', () => {
    const store = useLogsStore()
    const line = { n: 99, ts: '', level: 'warning', logger: '', func: '', file: '', line: 0, pid: '', tid: '', msg: 'tail', raw: 'tail', parsed: true }
    store.appendLogLine(line)
    expect(store.lines).toHaveLength(1)
    expect(store.totalLines).toBe(1)
  })

  it('appendLogLine caps at 10000 entries', () => {
    const store = useLogsStore()
    // Add 10005 lines
    for (let i = 0; i < 10005; i++) {
      store.appendLogLine({
        n: i, ts: '', level: 'info', logger: '', func: '', file: '',
        line: 0, pid: '', tid: '', msg: `line ${i}`, raw: '', parsed: true,
      })
    }
    // Array should be capped at 10000
    expect(store.lines.length).toBe(10000)
    // Oldest 5 should be removed (first entry is now n=5)
    expect(store.lines[0].n).toBe(5)
    // totalLines tracks total received, not array size
    expect(store.totalLines).toBe(10005)
  })

  it('setProcessLevel dispatch: pending 保持直到 log_config 推送（契约 log）', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: true, old: 'info', new: 'warning' }],
    })
    const store = useLogsStore()
    store.processes = [{ name: 'dztraderd', type: '主进程', level: 'info' }]
    const { pending } = usePending()
    const p = store.setProcessLevel('dztraderd', 'warning')
    const ok = await p
    expect(ok).toBe(true)
    // HTTP 成功≠已生效: pending 保持（keepPendingOnSuccess），级别不乐观更新
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    expect(store.processes[0].level).toBe('info')
    // log_config WS 推送（RTN_LOG_CONFIG）到达: 更新级别 + 清 pending
    store.applyLogConfig('dztraderd', { level: 'warning' })
    expect(store.processes[0].level).toBe('warning')
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
  })

  it('setProcessLevel ok=false 立即清 pending（无 RTN 会来）', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: false, old: 'info', new: 'warning' }],
    })
    const store = useLogsStore()
    store.processes = [{ name: 'dztraderd', type: '主进程', level: 'info' }]
    const { pending } = usePending()
    const ok = await store.setProcessLevel('dztraderd', 'warning')
    expect(ok).toBe(false)
    expect(store.processes[0].level).toBe('info')
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
  })

  it('setProcessLevel returns false on failure and clears pending', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: false, old: 'info', new: 'warning' }],
    })
    const store = useLogsStore()
    store.processes = [{ name: 'dztraderd', type: '主进程', level: 'info' }]
    const { pending } = usePending()
    const ok = await store.setProcessLevel('dztraderd', 'warning')
    expect(ok).toBe(false)
    expect(store.processes[0].level).toBe('info')
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
  })

  it('flushProcess pending uses logs:{name}:flush key and clears on success', async () => {
    vi.mocked(logsApi.logsApi.flush).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: true }],
    })
    const store = useLogsStore()
    const { pending } = usePending()
    const p = store.flushProcess('dztraderd')
    expect(pending['logs:dztraderd:flush']).toBe(true)
    const ok = await p
    expect(ok).toBe(true)
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:flush']).toBe(false)
  })

  it('batchSetLevel marks every target pending and clears after success', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockResolvedValue({
      results: [
        { name: 'dztraderd', ok: true, old: 'info', new: 'warning' },
        { name: 'dzmd_ctp', ok: true, old: 'debug', new: 'warning' },
      ],
    })
    const store = useLogsStore()
    store.processes = [
      { name: 'dztraderd', type: '主进程', level: 'info' },
      { name: 'dzmd_ctp', type: '行情源', level: 'info' },
    ]
    store.selectedProcesses = new Set(['dztraderd', 'dzmd_ctp'])
    store.batchLevel = 'warning'
    const { pending } = usePending()
    const p = store.batchSetLevel()
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    expect(pending['logs:dzmd_ctp:set_level']).toBe(true)
    const res = await p
    expect(res).toEqual({ ok: 2, fail: 0 })
    // 不乐观更新: 级别由 log_config 推送更新
    expect(store.processes.every(proc => proc.level === 'info')).toBe(true)
    // pending 保持, 由各 target 的 applyLogConfig 清除
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    store.applyLogConfig('dztraderd', { level: 'warning' })
    store.applyLogConfig('dzmd_ctp', { level: 'warning' })
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
    expect(pending['logs:dzmd_ctp:set_level']).toBe(false)
  })

  it('batchFlush returns counts and clears pending', async () => {
    vi.mocked(logsApi.logsApi.flush).mockResolvedValue({
      results: [
        { name: 'dztraderd', ok: true },
        { name: 'dzmd_ctp', ok: false },
      ],
    })
    const store = useLogsStore()
    store.selectedProcesses = new Set(['dztraderd', 'dzmd_ctp'])
    const { pending } = usePending()
    const res = await store.batchFlush()
    expect(res).toEqual({ ok: 1, fail: 1 })
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:flush']).toBe(false)
    expect(pending['logs:dzmd_ctp:flush']).toBe(false)
  })

  it('applySnapshot builds process list from WS mirror', () => {
    const store = useLogsStore()
    store.applySnapshot({
      dztraderd: { log_config: { level: 'info' } },
      dzmd_ctp: { log_config: { level: 'debug' } },
    })
    expect(store.processes).toHaveLength(2)
    expect(store.processes[0]).toEqual({ name: 'dztraderd', type: '主进程', level: 'info' })
    expect(store.processes[1]).toEqual({ name: 'dzmd_ctp', type: '行情源', level: 'debug' })
  })

  it('applyLogConfig updates existing process level', () => {
    const store = useLogsStore()
    store.applySnapshot({ dztraderd: { log_config: { level: 'info' } } })
    store.applyLogConfig('dztraderd', { level: 'warning' })
    expect(store.processes[0].level).toBe('warning')
  })

  it('applyLogConfig appends a new process not in snapshot', () => {
    const store = useLogsStore()
    store.applySnapshot({})
    store.applyLogConfig('dzmd_ctp', { level: 'debug' })
    expect(store.processes).toHaveLength(1)
    expect(store.processes[0]).toEqual({ name: 'dzmd_ctp', type: '行情源', level: 'debug' })
  })

  it('toggleProcessSelection adds and removes', () => {
    const store = useLogsStore()
    store.toggleProcessSelection('dztraderd')
    expect(store.selectedProcesses.has('dztraderd')).toBe(true)
    store.toggleProcessSelection('dztraderd')
    expect(store.selectedProcesses.has('dztraderd')).toBe(false)
  })

  it('selectFile stores path and logger', async () => {
    vi.mocked(logsApi.logsApi.content).mockResolvedValue({ lines: [], total: 0 })
    const store = useLogsStore()
    await store.selectFile('dzweb/dzweb_2026-07-16.log', 'dzweb')
    expect(store.selectedFile).toBe('dzweb/dzweb_2026-07-16.log')
    expect(store.selectedLogger).toBe('dzweb')
  })

  it('loadContent sends from_end=true', async () => {
    vi.mocked(logsApi.logsApi.content).mockResolvedValue({ lines: [], total: 0 })
    const store = useLogsStore()
    store.selectedFile = 'dztraderd.log'
    await store.loadContent()
    expect(logsApi.logsApi.content).toHaveBeenCalledWith(
      expect.objectContaining({ from_end: true })
    )
  })
})
