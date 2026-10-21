import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { sysmonApi } from '@/api/sysmon'
import type { SysmonAvailabilityEntry, SysmonSnapshotPayload } from '@/types/generated'

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

  return { snapshot, loaded, error, cpuUsageRatio, init, applySnapshot, availabilityOf }
})
