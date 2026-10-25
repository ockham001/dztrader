import { api } from './client'
import type {
  SysmonConfigPayload,
  SysmonDiskConfigPayload,
  SysmonHistoryConfigPayload,
  SysmonNetworkConfigPayload,
  SysmonRealtimeConfigPayload,
  SysmonSamplingConfigPayload,
  SysmonSnapshotPayload,
  SysmonThresholdsConfigPayload,
} from '@/types/generated'
import type { SysmonHistoryQueryParams, SysmonHistoryResponse } from '@/types/api'

/**
 * 系统监控配置 merge patch（契约 sysmon §3 / 契约 rest §2.8）。
 * RFC 7386：嵌套对象按键合并、数组整体替换；任意位置出现 null 由 dzsysmon 判为非法。
 *
 * 各段是「全量配置中该段」的深度 partial：只带要改的键，未出现的键保留生效值。
 * 数组字段（disk.watch_paths / disk.devices）只要出现即整体替换，不接受按元素拼接。
 * `sampling.groups` 按键合并（缺省键按 true 处理，契约 sysmon §3.1）。
 */
export interface SysmonConfigPatch {
  sampling?: Partial<SysmonSamplingConfigPayload>
  disk?: Partial<SysmonDiskConfigPayload>
  network?: Partial<SysmonNetworkConfigPayload>
  history?: Partial<SysmonHistoryConfigPayload>
  realtime?: Partial<SysmonRealtimeConfigPayload>
  thresholds?: Partial<SysmonThresholdsConfigPayload>
}

/** 把历史查询参数拼成 query string（省略 undefined/空串，后端按「未给」处理）。 */
function toHistoryQueryString(params: SysmonHistoryQueryParams): string {
  const search = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value === undefined || value === null || value === '') continue
    search.set(key, String(value))
  }
  const text = search.toString()
  return text ? `?${text}` : ''
}

// REST 响应仅表示「已写事件通道」，**不是**生效信号（契约 rest §2.8）；
// 生效信号是 WS `sysmon_config`（RTN_SYSMON_CONFIG 全量回推，失败时回推旧值）。
export const sysmonApi = {
  // 契约 rest §2.8：读取 dzweb 镜像中的当前系统监控快照。
  // dzsysmon 尚未上报（镜像未就绪）时后端回 503 —— 调用方按「快照尚未到达」处理。
  getSnapshot: () => api.get<SysmonSnapshotPayload>('/api/sysmon/snapshot'),
  // 契约 rest §2.8：读取 dzweb 镜像中的当前生效系统监控配置（任意已认证用户）。
  // 镜像未就绪（dzsysmon 未上报）时后端回 503 —— 调用方按「配置尚未到达」处理，不弹错。
  getConfig: () => api.get<SysmonConfigPayload>('/api/sysmon/config'),
  // 契约 rest §2.8：运行期修改系统监控配置（admin）；shm 未就绪时 503。
  setConfig: (patch: SysmonConfigPatch) =>
    api.put<{ ok: boolean; message?: string }>('/api/sysmon/config', patch),
  /**
   * 《帧契约：系统监控》§6.1 / 《帧契约：REST》§2.8：历史曲线查询。
   *
   * 历史库不可用时**不**回 5xx —— 而是 200 + `{available:false, reason}`（库缺失 / 版本不符 /
   * 文件损坏三种降级由后端判定）。因此调用方必须判断 `available`，不能只看 HTTP 是否成功。
   * 参数非法才是 400（后端不静默钳制）。
   */
  getHistory: (params: SysmonHistoryQueryParams = {}) =>
    api.get<SysmonHistoryResponse>(`/api/sysmon/history${toHistoryQueryString(params)}`),
}
