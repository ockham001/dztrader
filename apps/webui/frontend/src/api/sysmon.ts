import { api } from './client'
import type { SysmonNetworkConfigPayload, SysmonSnapshotPayload } from '@/types/generated'

/**
 * 系统监控配置 merge patch（契约 sysmon §3 / 契约 rest §2.8）。
 * RFC 7386：嵌套对象按键合并、数组整体替换；任意位置出现 null 由 dzsysmon 判为非法。
 * 当前前端只用它切换 network.hide_virtual，完整配置编辑随 #81。
 */
export interface SysmonConfigPatch {
  network?: Partial<SysmonNetworkConfigPayload>
}

// REST 响应仅表示「已写事件通道」，**不是**生效信号（契约 rest §2.8）；
// 生效信号是 WS `sysmon_config`（RTN_SYSMON_CONFIG 全量回推，失败时回推旧值）。
export const sysmonApi = {
  // 契约 rest §2.8：读取 dzweb 镜像中的当前系统监控快照。
  // dzsysmon 尚未上报（镜像未就绪）时后端回 503 —— 调用方按「快照尚未到达」处理。
  getSnapshot: () => api.get<SysmonSnapshotPayload>('/api/sysmon/snapshot'),
  // 契约 rest §2.8：运行期修改系统监控配置（admin）；shm 未就绪时 503。
  setConfig: (patch: SysmonConfigPatch) =>
    api.put<{ ok: boolean; message?: string }>('/api/sysmon/config', patch),
}
