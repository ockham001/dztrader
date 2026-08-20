import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { settingsApi } from '@/api/settings'
import type { EventShmConfigPatch } from '@/api/settings'
import { usePending, PENDING_TIMEOUT } from '@/composables/usePending'
import type { MasterSettingsView, WebuiSettingsView } from '@/types/api'
import type { ShmConfigView } from '@/types/mirror'

// 系统设置 store: event_shm_config 镜像唯一写入点 (WS snapshot + 增量, 契约 shm),
// master/webui 配置经 REST 拉取。

// 事件通道配置 pending key（导出供 spec 引用, 消除硬编码重复）
export const EVENT_SHM_KEY = 'settings:event_shm_config'
// 配置类操作超时（对齐 mdConfig 的 CONFIG_OP_TIMEOUT_MS）
export const EVENT_SHM_TIMEOUT_MS = 10_000

export const useSettingsStore = defineStore('settings', () => {
  const eventShmConfig = ref<ShmConfigView | null>(null)
  const master = ref<MasterSettingsView | null>(null)
  const webui = ref<WebuiSettingsView | null>(null)

  // 事件通道配置 pending: 提交挂起, RTN event_shm_config 到达经 applyEventShmConfig 清除 (契约 shm)
  const pendingApi = usePending()
  const eventShmPending = computed(() => pendingApi.pending[EVENT_SHM_KEY] ?? false)

  // RTN_EVENT_SHM_CONFIG → WS event_shm_config: 全量覆盖, 非法 payload 忽略 (对齐 applyMdShmConfig)
  function applyEventShmConfig(payload: unknown): void {
    const p = payload as Partial<ShmConfigView> | null | undefined
    if (!p || typeof p !== 'object') return
    const isNum = (v: unknown): boolean => typeof v === 'number' && Number.isFinite(v)
    if (!isNum(p.page_size_mb) || !isNum(p.check_interval_min)
      || !isNum(p.check_pages) || !isNum(p.check_bytes)) return
    if (typeof p.preload_points !== 'object' || p.preload_points === null) return
    for (const v of Object.values(p.preload_points)) {
      const pt = v as { pages?: unknown; bytes?: unknown }
      if (typeof pt !== 'object' || pt === null) return
      if (!isNum(pt.pages) || !isNum(pt.bytes)) return
    }
    eventShmConfig.value = p as ShmConfigView
    pendingApi.resolve(EVENT_SHM_KEY)
  }

  async function setEventShmConfig(patch: EventShmConfigPatch): Promise<boolean | typeof PENDING_TIMEOUT> {
    // 防重入: 已有同 key 提交挂起时直接返回 false, 不发重复 HTTP (对齐 mdConfig 各操作模式)
    if (eventShmPending.value) return false
    // 空提交幂等成功, 不下发 (对齐 mdConfig setShmConfig/setSubscribeParams)
    if (Object.keys(patch).length === 0) return true
    // 成功保持 pending 直至 RTN; HTTP 失败自动清
    // distinguishTimeout=true: 超时返回 PENDING_TIMEOUT(truthy), 视为成功跳过失败 toast,
    // 避免「下发超时」+「下发失败」双 toast (对齐 mdConfig 的 §7.2 防双 toast 约定)
    const ok = await pendingApi.run(EVENT_SHM_KEY, () => settingsApi.setEventShmConfig(patch), {
      opLabel: '事件通道配置下发',
      timeout: EVENT_SHM_TIMEOUT_MS,
      distinguishTimeout: true,
    })
    // 超时: PENDING_TIMEOUT 原样返回(truthy), 不折叠为 true/false, 供调用方去重双 toast;
    // HTTP 失败: run 吞异常返回 undefined → false
    if (ok === PENDING_TIMEOUT) return PENDING_TIMEOUT
    return ok !== undefined
  }

  async function loadMaster(): Promise<void> {
    try {
      master.value = await settingsApi.getMaster()
    } catch {
      master.value = null
    }
  }

  async function loadWebui(): Promise<void> {
    try {
      webui.value = await settingsApi.getWebui()
    } catch {
      webui.value = null
    }
  }

  async function setWebui(body: { token_ttl_sec?: number; notify_cache_size?: number }): Promise<boolean> {
    // 仅 PUT 失败判定为保存失败; 回填镜像失败不影响保存成功
    try {
      await settingsApi.setWebui(body)
    } catch {
      return false
    }
    try {
      await loadWebui()   // 回填镜像, 保存后界面立即刷新为生效值 (token_ttl 热生效, cache 重启生效)
    } catch {
      // 回填失败不影响保存成功
    }
    return true
  }

  return {
    eventShmConfig, master, webui, eventShmPending,
    applyEventShmConfig, setEventShmConfig, loadMaster, loadWebui, setWebui,
  }
})
