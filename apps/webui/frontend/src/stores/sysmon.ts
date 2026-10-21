import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { sysmonApi } from '@/api/sysmon'
import type {
  SysmonAvailabilityEntry,
  SysmonCpuCore,
  SysmonLoadAverage,
  SysmonSnapshotPayload,
} from '@/types/generated'

/** 有限数字校验：null / undefined / NaN / Infinity 一律视为不可用（不得用 0 冒充） */
function toFiniteNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

/**
 * 系统监控 store：契约 sysmon 快照域（固定实例 dzsysmon）的前端唯一写入点。
 *
 * 数据流：dzsysmon → 事件通道 → dzweb 镜像 → WS `sysmon_snapshot`（增量 / snapshot 快照域）。
 * 首次挂载时经 REST `GET /api/sysmon/snapshot` 补拉一次镜像（503 = dzsysmon 尚未上报）；
 * 之后只由 WS 推送刷新 —— 刷新基线由发送方 `realtime.push_interval_sec`（默认 5s）决定，
 * 前端不轮询、不额外发帧（契约 sysmon §4、契约 rest §2.8）。
 */
export const useSysmonStore = defineStore('sysmon', () => {
  const snapshot = ref<SysmonSnapshotPayload | null>(null)
  const loaded = ref(false)
  const error = ref<string | null>(null)

  /** 首次挂载补拉镜像；已加载过则跳过（WS 推送负责后续更新） */
  async function init(): Promise<void> {
    if (loaded.value) return
    try {
      snapshot.value = await sysmonApi.getSnapshot()
      error.value = null
    } catch (e) {
      // 503（未上报）/网络失败：保持无快照态，页面显示等待空态，后续 WS 推送会补上
      error.value = e instanceof Error ? e.message : String(e)
    } finally {
      loaded.value = true
    }
  }

  /**
   * WS `sysmon_snapshot` 增量与 `snapshot` 快照域共用（幂等全量覆盖）。
   * 非法 payload 忽略，不写不抛（契约 sysmon §5：dzweb 已校验，前端仍做防御）。
   */
  function applySnapshot(payload: unknown): void {
    const p = payload as Partial<SysmonSnapshotPayload> | null | undefined
    if (!p || typeof p !== 'object') return
    if (typeof p.timestamp !== 'number') return
    if (!p.availability || typeof p.availability !== 'object') return
    if (!p.availability.metrics || typeof p.availability.metrics !== 'object') return
    snapshot.value = p as SysmonSnapshotPayload
    loaded.value = true
    error.value = null
  }

  /** 指标可用性条目（契约 sysmon §2.5；缺失 = 不可用） */
  function availabilityOf(metric: string): SysmonAvailabilityEntry | undefined {
    return snapshot.value?.availability.metrics[metric]
  }

  /**
   * CPU 总使用率（比率）。仅在可用性映射标记可用且值为有效数字时返回，
   * 否则 null —— 首次采样/计数器不可用一律不得用 0 冒充（契约 sysmon §2.7）。
   */
  const cpuUsageRatio = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.total']?.available !== true) return null
    const ratio = s.cpu.total.usage_ratio
    return typeof ratio === 'number' && Number.isFinite(ratio) ? ratio : null
  })

  /**
   * 每核使用率（契约 sysmon §2.3）。仅当可用性映射标记 cpu.cores 可用时返回数组，
   * 否则空数组 —— 视图据此区分「指标不可用」与「可用但暂无数据」，不得用 0 冒充。
   * 核数随快照变化（热插拔 / 虚拟机缩放），每次按当前快照数组渲染，不跨快照合并。
   */
  const cpuCores = computed<SysmonCpuCore[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.cores']?.available !== true) return []
    const cores = Array.isArray(s.cpu?.cores) ? s.cpu.cores : []
    return cores.map((core, position) => ({
      // index 非法时退化为数组下标，保证列表 key / 展示稳定（正常 payload 不会走到）
      index: typeof core?.index === 'number' && Number.isFinite(core.index) ? core.index : position,
      usage_ratio: toFiniteNumber(core?.usage_ratio),
      saturated: core?.saturated === true,
    }))
  })

  /** 持续饱和核 index 集合（契约 sysmon §2.3）：cpu.cores 不可用时为空数组 */
  const saturatedCores = computed<number[]>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.cores']?.available !== true) return []
    const indices = Array.isArray(s.cpu?.saturated_cores) ? s.cpu.saturated_cores : []
    const unique = new Set<number>()
    for (const index of indices) {
      if (typeof index === 'number' && Number.isInteger(index) && index >= 0) unique.add(index)
    }
    return [...unique].sort((a, b) => a - b)
  })

  /**
   * 系统负载（契约 sysmon §2.3）。仅在可用性映射标记可用且快照携带对象时返回；
   * semantic 说明口径（Linux load_average / Windows processor_queue），前端不做跨口径比较。
   */
  const cpuLoadAverage = computed<SysmonLoadAverage | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.load_average']?.available !== true) return null
    const load = s.cpu?.load_average
    if (!load || typeof load !== 'object') return null
    return {
      one: toFiniteNumber(load.one),
      five: toFiniteNumber(load.five),
      fifteen: toFiniteNumber(load.fifteen),
      semantic: typeof load.semantic === 'string' ? load.semantic : '',
    }
  })

  /**
   * 每秒上下文切换 / 每秒中断（契约 sysmon §2.3、§2.5）。
   * 两者共用可用性键 `cpu.context_switches`；值不可用时返回 null，不得用 0 冒充。
   */
  const cpuContextSwitchesPerSec = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.context_switches']?.available !== true) return null
    return toFiniteNumber(s.cpu?.context_switches_per_sec)
  })

  const cpuInterruptsPerSec = computed<number | null>(() => {
    const s = snapshot.value
    if (!s || s.availability.metrics['cpu.context_switches']?.available !== true) return null
    return toFiniteNumber(s.cpu?.interrupts_per_sec)
  })

  /** 跨平台语义提示（契约 sysmon §2.5）：缺失 / 非字符串返回空串，由视图决定是否展示 */
  function semanticHint(key: string): string {
    const hints = snapshot.value?.availability.semantic_hints
    if (!hints || typeof hints !== 'object') return ''
    const hint = hints[key]
    return typeof hint === 'string' ? hint.trim() : ''
  }

  return {
    snapshot,
    loaded,
    error,
    cpuUsageRatio,
    cpuCores,
    saturatedCores,
    cpuLoadAverage,
    cpuContextSwitchesPerSec,
    cpuInterruptsPerSec,
    semanticHint,
    init,
    applySnapshot,
    availabilityOf,
  }
})
