// ===== 快照镜像（dzweb MirrorStore 全量）=====
// snapshot 帧 data 结构: Record<instance_id, Partial<DomainPayloads>>
// P3 Task 1: 建立领域载荷骨架
// 载荷细化为 unknown 是终态（设计 §5.5 实现偏差：判别联合预留至交易数据扩展前实现）

// LogConfigPayload / AutoLoginPayload / NotifyUiPayload 由契约单源生成（schema → generated.ts），禁止手写
import type { LogConfigPayload, AutoLoginPayload, AutoLoginSchedule, NotifyUiPayload, ShmConfigView } from './generated'
export type { AutoLoginPayload, AutoLoginSchedule, LogConfigPayload, NotifyUiPayload, ShmConfigView }

export interface ProgressPayload { min: number; max: number; current: number; desc?: string }

// 契约 shm ShmConfigView 已由契约单源（generated）提供，见顶部转发

export interface DomainPayloads {
  log_config?: LogConfigPayload
  process_status?: import('./api').ProcessStatusPayload
  process_config?: unknown        // P1 后端 process_config 全量帧镜像（unknown 为终态，见文件头说明）
  md_config?: unknown          // 脱敏 MdConfig（unknown 为终态，见文件头说明）
  md_status?: unknown
  md_shm_config?: unknown
  event_shm_config?: unknown
  auto_login?: AutoLoginPayload
  progress?: ProgressPayload
}

export type MirrorSnapshot = Record<string, Partial<DomainPayloads>>
