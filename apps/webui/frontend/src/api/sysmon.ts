import { api } from './client'
import type { SysmonSnapshotPayload } from '@/types/generated'

// 契约 rest §2.8：读取 dzweb 镜像中的当前系统监控快照。
// dzsysmon 尚未上报（镜像未就绪）时后端回 503 —— 调用方按「快照尚未到达」处理。
export const sysmonApi = {
  getSnapshot: () => api.get<SysmonSnapshotPayload>('/api/sysmon/snapshot'),
}
