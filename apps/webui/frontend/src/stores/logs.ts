import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { logsApi } from '@/api/logs'
import { useSystemStore } from '@/stores/system'
import { usePending } from '@/composables/usePending'
import type {
  LogFile, LogLine, LogStats, LogAggregate,
  TimelineBucket, LogProcess, LogLevel,
} from '@/types/api'

// 进程控制 pending 迁移 usePending（设计 §5.3，契约 log/webui-ws §5）：
// - key: logs:{name}:set_level / logs:{name}:flush（按进程名，行级按钮独立 spin）
// - minPendingMs: 300 防闪烁
// - set_level: RTN 驱动 --SET 后 pending，以对应实例的 log_config 推送
//   （applyLogConfig resolve）清除；HTTP ok=false 无 RTN 会来，立即 fail
// - flush: 无 RTN，keepPendingOnSuccess: false（HTTP 同步语义，成功即清，纯视觉防双击）
// - timeout: 10_000 兜底（超时清 + toast）
const MIN_PENDING_MS = 300
const CONTROL_TIMEOUT_MS = 10_000

export const LOG_LEVELS: LogLevel[] = ['trace', 'debug', 'info', 'warning', 'error', 'critical', 'off']
const FILE_PAGE_SIZE = 30

export const useLogsStore = defineStore('logs', () => {
  // === Tab state ===
  const browserTab = ref<'viewer' | 'analysis'>('viewer')

  // === File list ===
  const files = ref<LogFile[]>([])
  const filesLoading = ref(false)
  const selectedFile = ref<string>('')
  const selectedLogger = ref<string>('')
  const fileLoggerFilter = ref<string>('')
  const fileDateFilter = ref<string>('')
  const fileOffset = ref(0)
  const filesHasMore = ref(true)

  // === Content ===
  const lines = ref<LogLine[]>([])
  const totalLines = ref(0)
  const contentLoading = ref(false)
  const levelFilter = ref<string>('info')
  const keyword = ref('')
  const tailEnabled = ref(false)

  // === Stats ===
  const stats = ref<LogStats | null>(null)
  const statsLoading = ref(false)

  // === Aggregate ===
  const aggregate = ref<LogAggregate[]>([])
  const aggregateLoading = ref(false)
  const expandedPatterns = ref<Set<string>>(new Set())

  // === Timeline ===
  const timeline = ref<TimelineBucket[]>([])
  const timelineLoading = ref(false)
  const timelineBucket = ref<'minute' | 'hour' | 'day'>('minute')

  // === Process control ===
  const processes = ref<LogProcess[]>([])
  const selectedProcesses = ref<Set<string>>(new Set())
  const batchLevel = ref<string>('info')

  const { pending, run, resolve, fail } = usePending()

  // === Computed ===
  const loggerOptions = computed(() => {
    const set = new Set(files.value.map(f => f.logger))
    return Array.from(set)
  })

  // === Actions ===
  async function loadFiles(reset = false): Promise<void> {
    if (reset) {
      fileOffset.value = 0
      filesHasMore.value = true
    }
    if (!filesHasMore.value && !reset) return
    filesLoading.value = true
    try {
      const newFiles = await logsApi.files({
        logger: fileLoggerFilter.value || undefined,
        date: fileDateFilter.value || undefined,
        limit: FILE_PAGE_SIZE,
        offset: fileOffset.value,
      })
      if (reset) {
        files.value = newFiles
      } else {
        files.value = [...files.value, ...newFiles]
      }
      filesHasMore.value = newFiles.length >= FILE_PAGE_SIZE
      fileOffset.value += newFiles.length
    } catch {
      if (reset) files.value = []
    } finally {
      filesLoading.value = false
    }
  }

  /// 刷新文件列表，保持当前已加载数量和选中状态
  async function refreshFiles(): Promise<void> {
    const savedSelected = selectedFile.value
    const savedOffset = fileOffset.value
    const fetchLimit = Math.max(FILE_PAGE_SIZE, savedOffset)
    filesLoading.value = true
    try {
      const newFiles = await logsApi.files({
        logger: fileLoggerFilter.value || undefined,
        date: fileDateFilter.value || undefined,
        limit: fetchLimit,
        offset: 0,
      })
      files.value = newFiles
      fileOffset.value = newFiles.length
      filesHasMore.value = newFiles.length >= fetchLimit
      // 选中文件不在列表中则清空选中（右侧内容保持不动）
      if (savedSelected && !newFiles.some(f => f.path === savedSelected)) {
        selectedFile.value = ''
        selectedLogger.value = ''
      }
    } catch {
      // 保持原有列表不变
    } finally {
      filesLoading.value = false
    }
  }

  async function selectFile(path: string, logger: string): Promise<void> {
    selectedFile.value = path
    selectedLogger.value = logger
    await loadContent()
  }

  async function loadContent(): Promise<void> {
    if (!selectedFile.value) return
    contentLoading.value = true
    try {
      const content = await logsApi.content({
        file: selectedFile.value,
        limit: 500,
        from_end: true,
        level: levelFilter.value || undefined,
        keyword: keyword.value || undefined,
      })
      lines.value = content.lines
      totalLines.value = content.total
    } catch {
      lines.value = []
      totalLines.value = 0
    } finally {
      contentLoading.value = false
    }
  }

  async function loadStats(): Promise<void> {
    if (!selectedFile.value) return
    statsLoading.value = true
    try {
      stats.value = await logsApi.stats({ file: selectedFile.value })
    } catch {
      stats.value = null
    } finally {
      statsLoading.value = false
    }
  }

  async function loadAggregate(): Promise<void> {
    if (!selectedFile.value) return
    aggregateLoading.value = true
    try {
      aggregate.value = await logsApi.aggregate({ file: selectedFile.value })
    } catch {
      aggregate.value = []
    } finally {
      aggregateLoading.value = false
    }
  }

  async function loadTimeline(): Promise<void> {
    if (!selectedFile.value) return
    timelineLoading.value = true
    try {
      timeline.value = await logsApi.timeline({
        file: selectedFile.value,
        bucket: timelineBucket.value,
      })
    } catch {
      timeline.value = []
    } finally {
      timelineLoading.value = false
    }
  }

  async function loadAnalysisData(): Promise<void> {
    await Promise.all([loadStats(), loadAggregate(), loadTimeline()])
  }

  /// 按进程名前缀推导类型（与后端旧 GET /api/logs/processes 的 type 逻辑一致）
  function deriveType(name: string): string {
    if (name === 'dztraderd') return '主进程'
    if (name.startsWith('dzmd')) return '行情源'
    if (name.startsWith('dztd')) return '交易'
    if (useSystemStore().isSelf(name)) return 'WebUI'
    if (name.includes('strategy')) return '策略'
    return '未知'
  }

  /// 从 WS 全量镜像快照重建进程列表（连接/重连时由后端推送）
  /// mirror 结构: { instance_id: { log_config: { level, flush_on } } }
  function applySnapshot(mirror: Record<string, { log_config?: { level?: string } }>): void {
    const list: LogProcess[] = []
    for (const [name, inst] of Object.entries(mirror)) {
      list.push({
        name,
        type: deriveType(name),
        level: inst.log_config?.level ?? '',
      })
    }
    processes.value = list
  }

  /// 单实例 log_config 增量更新（RTN_LOG_CONFIG → WS log_config 推送）
  function applyLogConfig(instanceId: string, config: { level?: string }): void {
    const idx = processes.value.findIndex(p => p.name === instanceId)
    const level = config.level ?? ''
    if (idx !== -1) {
      processes.value[idx] = { ...processes.value[idx], level }
    } else {
      // 快照之后新出现的进程：补一行
      processes.value.push({ name: instanceId, type: deriveType(instanceId), level })
    }
    // 契约 log: log_config 推送到达 = 生效信号，清 set_level pending
    resolve(`logs:${instanceId}:set_level`)
  }

  /// Append a new line from WebSocket log_line push (real-time tail)
  function appendLogLine(line: LogLine): void {
    lines.value.push(line)
    if (lines.value.length > 10000) {
      lines.value.splice(0, lines.value.length - 10000)
    }
    totalLines.value++
  }

  /// 契约 log: SET 后 pending，以对应实例的 log_config 推送（applyLogConfig）清除；
  /// HTTP ok=false（未写入事件通道）时无 RTN 会来，立即 fail；超时 10s 兜底 toast。
  /// 级别显示不乐观更新，由推送驱动（RTN 成功=新值/失败=回滚旧值）。
  async function setProcessLevel(name: string, level: string): Promise<boolean> {
    const key = `logs:${name}:set_level`
    const resp = await run(
      key,
      () => logsApi.setLevel([name], level),
      { minPendingMs: MIN_PENDING_MS, timeout: CONTROL_TIMEOUT_MS, opLabel: `${name} 日志级别设置` },
    )
    if (!resp) return false  // HTTP 失败/超时：usePending 已清 pending
    const result = resp.results.find(r => r.name === name)
    if (!result?.ok) {
      fail(key)  // 未写入事件通道：无 RTN，立即清
      return false
    }
    return true
  }

  async function flushProcess(name: string): Promise<boolean> {
    const key = `logs:${name}:flush`
    const resp = await run(
      key,
      () => logsApi.flush([name]),
      { minPendingMs: MIN_PENDING_MS, timeout: CONTROL_TIMEOUT_MS, keepPendingOnSuccess: false },
    )
    if (!resp) return false
    const result = resp.results.find(r => r.name === name)
    return result?.ok ?? false
  }

  async function batchSetLevel(): Promise<{ ok: number; fail: number }> {
    const targets = Array.from(selectedProcesses.value)
    if (targets.length === 0) return { ok: 0, fail: 0 }
    // 每个 target 一个 pending key（行级按钮独立 spin，对照现有 processPending Set 语义）；
    // 共享一次 HTTP 请求（fn 防重入：shared promise 首次构造后复用）；
    // pending 由各 target 的 applyLogConfig（log_config 推送）清除
    let shared: ReturnType<typeof logsApi.setLevel> | undefined
    const results = await Promise.all(targets.map(t =>
      run(
        `logs:${t}:set_level`,
        async () => {
          shared ??= logsApi.setLevel(targets, batchLevel.value)
          return shared
        },
        { minPendingMs: MIN_PENDING_MS, timeout: CONTROL_TIMEOUT_MS, opLabel: `${t} 日志级别设置` },
      ),
    ))
    const resp = results[0]
    if (!resp) return { ok: 0, fail: targets.length }
    let okCount = 0
    let failCount = 0
    for (const result of resp.results) {
      if (result.ok) {
        okCount++  // 已下发; 级别与 pending 由各 target 的 applyLogConfig 驱动
      } else {
        failCount++
        fail(`logs:${result.name}:set_level`)
      }
    }
    return { ok: okCount, fail: failCount }
  }

  async function batchFlush(): Promise<{ ok: number; fail: number }> {
    const targets = Array.from(selectedProcesses.value)
    if (targets.length === 0) return { ok: 0, fail: 0 }
    let shared: ReturnType<typeof logsApi.flush> | undefined
    const results = await Promise.all(targets.map(t =>
      run(
        `logs:${t}:flush`,
        async () => {
          shared ??= logsApi.flush(targets)
          return shared
        },
        { minPendingMs: MIN_PENDING_MS, timeout: CONTROL_TIMEOUT_MS, keepPendingOnSuccess: false },
      ),
    ))
    const resp = results[0]
    if (!resp) return { ok: 0, fail: targets.length }
    let okCount = 0
    let failCount = 0
    for (const result of resp.results) {
      if (result.ok) okCount++
      else failCount++
    }
    return { ok: okCount, fail: failCount }
  }

  function toggleProcessSelection(name: string): void {
    if (selectedProcesses.value.has(name)) {
      selectedProcesses.value.delete(name)
    } else {
      selectedProcesses.value.add(name)
    }
  }

  function selectAllProcesses(): void {
    selectedProcesses.value = new Set(processes.value.map(p => p.name))
  }

  function clearProcessSelection(): void {
    selectedProcesses.value.clear()
  }

  function togglePatternExpanded(pattern: string): void {
    if (expandedPatterns.value.has(pattern)) {
      expandedPatterns.value.delete(pattern)
    } else {
      expandedPatterns.value.add(pattern)
    }
  }

  function setTailEnabled(enabled: boolean): void {
    tailEnabled.value = enabled
  }

  function isProcessPending(name: string): boolean {
    // 迁移 usePending 后：set_level 与 flush 任一挂起即视为 pending（行级按钮 spin/禁用）
    return !!pending[`logs:${name}:set_level`] || !!pending[`logs:${name}:flush`]
  }

  return {
    // State
    browserTab, files, filesLoading, selectedFile, selectedLogger, fileLoggerFilter,
    fileDateFilter,
    filesHasMore, lines, totalLines, contentLoading, levelFilter,
    keyword, tailEnabled, stats, statsLoading, aggregate, aggregateLoading,
    expandedPatterns, timeline, timelineLoading, timelineBucket,
    processes, selectedProcesses, batchLevel,
    // Computed
    loggerOptions,
    // Actions
    loadFiles, refreshFiles, selectFile, loadContent, loadStats, loadAggregate,
    loadTimeline, loadAnalysisData, applySnapshot, applyLogConfig, appendLogLine,
    setProcessLevel, flushProcess, batchSetLevel, batchFlush,
    toggleProcessSelection, selectAllProcesses, clearProcessSelection,
    togglePatternExpanded, setTailEnabled, isProcessPending,
  }
})
