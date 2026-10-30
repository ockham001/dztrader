import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { nextTick } from 'vue'
import { setActivePinia, createPinia } from 'pinia'
import { useLogsStore, logPending } from '../logs'
import { useSystemStore } from '@/stores/system'
import { useToastStore } from '@/stores/toast'
import * as logsApi from '@/api/logs'
import type { LogFile, LogStats, LogAggregate, TimelineBucket } from '@/types/api'

const mkFile = (i: number): LogFile => ({
  name: `f${i}.log`, logger: 'dztraderd', size: 1, mtime: '', path: `/logs/f${i}.log`,
})

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
    // logPending 领域独立实例（pending + timers）——每个用例前重置
    logPending.__resetForTests()
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
    const { pending } = logPending
    const p = store.setProcessLevel('dztraderd', 'warning')
    const ok = await p
    expect(ok).toBe(true)
    // 实参锚定：单目标只发本进程（契约 rest POST /api/logs/level）
    expect(logsApi.logsApi.setLevel).toHaveBeenCalledWith(['dztraderd'], 'warning')
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
    const { pending } = logPending
    const ok = await store.setProcessLevel('dztraderd', 'warning')
    expect(ok).toBe(false)
    expect(store.processes[0].level).toBe('info')
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
  })

  it('setProcessLevel HTTP reject: 返回 false、清 pending、级别不乐观更新', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockRejectedValue(new Error('503'))
    const store = useLogsStore()
    store.processes = [{ name: 'dztraderd', type: '主进程', level: 'info' }]
    const { pending } = logPending
    const ok = await store.setProcessLevel('dztraderd', 'warning')
    expect(ok).toBe(false)
    expect(store.processes[0].level).toBe('info')
    // minPendingMs=300 防闪烁：pending 在满 300ms 后清除
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
  })

  it('setProcessLevel 超时兜底（10s）：pending 清、级别不变、toast 一次', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockImplementation(() => new Promise(() => {}))
    const store = useLogsStore()
    store.processes = [{ name: 'dztraderd', type: '主进程', level: 'info' }]
    const { pending } = logPending
    const p = store.setProcessLevel('dztraderd', 'warning')
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    await vi.advanceTimersByTimeAsync(10_001)
    expect(await p).toBe(false)  // 超时无 RTN，按失败返回
    expect(pending['logs:dztraderd:set_level']).toBe(false)
    expect(store.processes[0].level).toBe('info')
    const toast = useToastStore()
    expect(toast.items).toHaveLength(1)
    expect(toast.items[0].level).toBe('error')
    expect(toast.items[0].message).toBe('dztraderd 日志级别设置超时')
  })

  it('flushProcess 直接返回结果，不设 pending（契约 log：FLUSH 无反馈）', async () => {
    vi.mocked(logsApi.logsApi.flush).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: true }],
    })
    const store = useLogsStore()
    const { pending } = logPending
    const ok = await store.flushProcess('dztraderd')
    expect(ok).toBe(true)
    expect(logsApi.logsApi.flush).toHaveBeenCalledWith(['dztraderd'])
    expect(pending['logs:dztraderd:flush']).toBeUndefined()  // 不设 pending
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
    const { pending } = logPending
    const p = store.batchSetLevel()
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    expect(pending['logs:dzmd_ctp:set_level']).toBe(true)
    const res = await p
    expect(res).toEqual({ ok: 2, fail: 0 })
    // 一次 HTTP 承载全部选中目标与批量级别（契约 rest）
    expect(logsApi.logsApi.setLevel).toHaveBeenCalledWith(['dztraderd', 'dzmd_ctp'], 'warning')
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

  it('batchFlush 返回计数，不设 pending（契约 log：FLUSH 无反馈）', async () => {
    vi.mocked(logsApi.logsApi.flush).mockResolvedValue({
      results: [
        { name: 'dztraderd', ok: true },
        { name: 'dzmd_ctp', ok: false },
      ],
    })
    const store = useLogsStore()
    const { pending } = logPending
    store.selectedProcesses = new Set(['dztraderd', 'dzmd_ctp'])
    const res = await store.batchFlush()
    expect(res).toEqual({ ok: 1, fail: 1 })
    expect(logsApi.logsApi.flush).toHaveBeenCalledWith(['dztraderd', 'dzmd_ctp'])
    expect(pending['logs:dztraderd:flush']).toBeUndefined()
    expect(pending['logs:dzmd_ctp:flush']).toBeUndefined()
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

  it('applySnapshot 清除挂起的 set_level pending（契约 webui-ws §6 并清 pending）', async () => {
    vi.mocked(logsApi.logsApi.setLevel).mockResolvedValue({
      results: [{ name: 'dztraderd', ok: true, old: 'info', new: 'warning' }],
    })
    const store = useLogsStore()
    const { pending } = logPending
    await store.setProcessLevel('dztraderd', 'warning')  // ok=true → pending 保持等 RTN
    expect(pending['logs:dztraderd:set_level']).toBe(true)
    // 重连快照到达（断连期间 RTN 已被 dzweb 镜像吸收）：重建列表 + 清 pending
    store.applySnapshot({ dztraderd: { log_config: { level: 'warning' } } })
    expect(store.processes[0].level).toBe('warning')
    await vi.advanceTimersByTimeAsync(300)
    expect(pending['logs:dztraderd:set_level']).toBe(false)
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

  describe('进程类型推导（#207 A2）', () => {
    it('覆盖全部进程类别：主进程 / 行情源 / 交易 / 系统监控 / WebUI / 策略', () => {
      const system = useSystemStore()
      system.processName = 'dzweb'
      const store = useLogsStore()
      store.applySnapshot({
        dztraderd: { log_config: { level: 'info' } },
        dzmd_ctp: { log_config: { level: 'info' } },
        dzmd_ctp_2: { log_config: { level: 'info' } },
        dztd_ctp: { log_config: { level: 'info' } },
        dzsysmon: { log_config: { level: 'info' } },
        dzweb: { log_config: { level: 'info' } },
        stg_demo: { log_config: { level: 'info' } },
      })
      expect(store.processes.map(p => [p.name, p.type])).toEqual([
        ['dztraderd', '主进程'],
        ['dzmd_ctp', '行情源'],
        ['dzmd_ctp_2', '行情源'],
        ['dztd_ctp', '交易'],
        ['dzsysmon', '系统监控'],
        ['dzweb', 'WebUI'],
        ['stg_demo', '策略'],
      ])
    })

    it('无法识别的进程名回退「未知」', () => {
      const store = useLogsStore()
      store.applySnapshot({ some_tool: { log_config: { level: 'info' } } })
      expect(store.processes[0].type).toBe('未知')
    })

    it('竞态：快照先到（后端进程名未就绪）→ system-info 到达后重算既有列表类型', async () => {
      const system = useSystemStore()
      system.processName = ''  // /api/system/info 尚未返回
      const store = useLogsStore()
      store.applySnapshot({ dzweb: { log_config: { level: 'info' } } })
      expect(store.processes[0].type).toBe('未知')

      // system.init() 完成：进程名就绪 → 既有列表重算，无需刷新/重连
      system.processName = 'dzweb'
      await nextTick()
      expect(store.processes[0].type).toBe('WebUI')
      // 增量推送只更新 level，不得把类型改坏
      store.applyLogConfig('dzweb', { level: 'debug' })
      expect(store.processes[0]).toEqual({ name: 'dzweb', type: 'WebUI', level: 'debug' })
    })
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

  describe('loadFiles 分页 / 失败 / refreshFiles', () => {
    // 本组使用 once 队列；每例显式 reset，避免未消费的 once 值跨用例泄漏
    beforeEach(() => {
      vi.mocked(logsApi.logsApi.files).mockReset()
    })

    it('loadFiles(false) 追加下一页并推进 offset 与 filesHasMore', async () => {
      const first = Array.from({ length: 30 }, (_, i) => mkFile(i))
      const second = [mkFile(30), mkFile(31)]
      vi.mocked(logsApi.logsApi.files)
        .mockResolvedValueOnce(first)
        .mockResolvedValueOnce(second)
      const store = useLogsStore()
      await store.loadFiles(true)
      expect(store.files).toHaveLength(30)
      expect(store.filesHasMore).toBe(true)
      await store.loadFiles(false)
      expect(store.files).toHaveLength(32)
      expect(store.filesHasMore).toBe(false)  // 2 < FILE_PAGE_SIZE
      expect(logsApi.logsApi.files).toHaveBeenNthCalledWith(1, {
        logger: undefined, date: undefined, limit: 30, offset: 0,
      })
      expect(logsApi.logsApi.files).toHaveBeenNthCalledWith(2, {
        logger: undefined, date: undefined, limit: 30, offset: 30,
      })
      // 已无更多：追加不再发请求
      await store.loadFiles(false)
      expect(logsApi.logsApi.files).toHaveBeenCalledTimes(2)
    })

    it('loadFiles 失败：reset 清空列表、追加失败保留原列表', async () => {
      const store = useLogsStore()
      vi.mocked(logsApi.logsApi.files).mockRejectedValueOnce(new Error('503'))
      await store.loadFiles(true)
      expect(store.files).toEqual([])
      expect(store.filesLoading).toBe(false)

      vi.mocked(logsApi.logsApi.files).mockResolvedValueOnce([mkFile(1), mkFile(2)])
      await store.loadFiles(true)
      expect(store.files).toHaveLength(2)
      store.filesHasMore = true  // 2 条不足一页；显式允许追加以触达失败分支
      vi.mocked(logsApi.logsApi.files).mockRejectedValueOnce(new Error('503'))
      await store.loadFiles(false)
      expect(store.files).toHaveLength(2)  // 追加失败不清已有
      expect(store.filesLoading).toBe(false)
    })

    it('refreshFiles：选中文件不在新列表时清空选中', async () => {
      const store = useLogsStore()
      vi.mocked(logsApi.logsApi.files)
        .mockResolvedValueOnce([mkFile(1)])   // loadFiles
        .mockResolvedValueOnce([mkFile(2)])   // refreshFiles
      await store.loadFiles(true)
      expect(store.files.map(f => f.path)).toEqual(['/logs/f1.log'])
      store.selectedFile = '/logs/f1.log'
      store.selectedLogger = 'dztraderd'
      await store.refreshFiles()
      expect(store.files.map(f => f.path)).toEqual(['/logs/f2.log'])
      expect(store.selectedFile).toBe('')
      expect(store.selectedLogger).toBe('')
    })

    it('refreshFiles：失败保持原列表与选中', async () => {
      const store = useLogsStore()
      vi.mocked(logsApi.logsApi.files).mockResolvedValueOnce([mkFile(1)])
      await store.loadFiles(true)
      store.selectedFile = '/logs/f1.log'
      store.selectedLogger = 'dztraderd'
      vi.mocked(logsApi.logsApi.files).mockRejectedValueOnce(new Error('503'))
      await store.refreshFiles()
      expect(store.files).toHaveLength(1)
      expect(store.selectedFile).toBe('/logs/f1.log')
      expect(store.selectedLogger).toBe('dztraderd')
      expect(store.filesLoading).toBe(false)
    })
  })

  describe('分析数据（loadStats / loadAggregate / loadTimeline / loadAnalysisData）', () => {
    const statsView: LogStats = { by_level: { info: 1 }, total: 1, timespan: '1m' }
    const aggregateView: LogAggregate[] = [
      { msg_pattern: 'boom', count: 2, first_ts: 'a', last_ts: 'b', samples: [] },
    ]
    const timelineView: TimelineBucket[] = [
      { ts: '2026-01-01T00:00:00', counts: { info: 1 } },
    ]

    it('三个查询成功写入并按 file/bucket 查询', async () => {
      vi.mocked(logsApi.logsApi.stats).mockResolvedValue(statsView)
      vi.mocked(logsApi.logsApi.aggregate).mockResolvedValue(aggregateView)
      vi.mocked(logsApi.logsApi.timeline).mockResolvedValue(timelineView)
      const store = useLogsStore()
      store.selectedFile = 'a.log'
      await store.loadStats()
      await store.loadAggregate()
      await store.loadTimeline()
      expect(store.stats).toEqual(statsView)
      expect(store.aggregate).toEqual(aggregateView)
      expect(store.timeline).toEqual(timelineView)
      expect(store.statsLoading).toBe(false)
      expect(store.aggregateLoading).toBe(false)
      expect(store.timelineLoading).toBe(false)
      expect(logsApi.logsApi.stats).toHaveBeenCalledWith({ file: 'a.log' })
      expect(logsApi.logsApi.aggregate).toHaveBeenCalledWith({ file: 'a.log' })
      expect(logsApi.logsApi.timeline).toHaveBeenCalledWith({ file: 'a.log', bucket: 'minute' })
    })

    it('三个查询失败回退（null / []）并复位 loading', async () => {
      vi.mocked(logsApi.logsApi.stats).mockRejectedValue(new Error('503'))
      vi.mocked(logsApi.logsApi.aggregate).mockRejectedValue(new Error('503'))
      vi.mocked(logsApi.logsApi.timeline).mockRejectedValue(new Error('503'))
      const store = useLogsStore()
      store.selectedFile = 'a.log'
      store.stats = statsView
      store.aggregate = aggregateView
      store.timeline = timelineView
      await store.loadStats()
      await store.loadAggregate()
      await store.loadTimeline()
      expect(store.stats).toBeNull()
      expect(store.aggregate).toEqual([])
      expect(store.timeline).toEqual([])
      expect(store.statsLoading).toBe(false)
      expect(store.aggregateLoading).toBe(false)
      expect(store.timelineLoading).toBe(false)
    })

    it('loadAnalysisData 并发触发三者（全部在途后才写入）', async () => {
      let resolveStats!: (v: LogStats) => void
      let resolveAggregate!: (v: LogAggregate[]) => void
      let resolveTimeline!: (v: TimelineBucket[]) => void
      vi.mocked(logsApi.logsApi.stats).mockImplementation(
        () => new Promise<LogStats>(res => { resolveStats = res }))
      vi.mocked(logsApi.logsApi.aggregate).mockImplementation(
        () => new Promise<LogAggregate[]>(res => { resolveAggregate = res }))
      vi.mocked(logsApi.logsApi.timeline).mockImplementation(
        () => new Promise<TimelineBucket[]>(res => { resolveTimeline = res }))
      const store = useLogsStore()
      store.selectedFile = 'a.log'
      const p = store.loadAnalysisData()
      await Promise.resolve()
      expect(logsApi.logsApi.stats).toHaveBeenCalledTimes(1)
      expect(logsApi.logsApi.aggregate).toHaveBeenCalledTimes(1)
      expect(logsApi.logsApi.timeline).toHaveBeenCalledTimes(1)
      expect(store.statsLoading).toBe(true)
      expect(store.aggregateLoading).toBe(true)
      expect(store.timelineLoading).toBe(true)
      resolveStats(statsView)
      resolveAggregate(aggregateView)
      resolveTimeline(timelineView)
      await p
      expect(store.stats).toEqual(statsView)
      expect(store.aggregate).toEqual(aggregateView)
      expect(store.timeline).toEqual(timelineView)
    })
  })
})
