// ===== 快照镜像（dzweb MirrorStore 全量）=====
// snapshot 帧 data 结构: Record<instance_id, Partial<DomainPayloads>>
// P3 Task 1: 建立领域载荷骨架
// 载荷细化为 unknown 是终态（设计 §5.5 实现偏差：判别联合预留至交易数据扩展前实现）

export interface LogConfigPayload { level: string; flush_on: string }

export interface ProgressPayload { min: number; max: number; current: number; desc?: string }

export interface AutoLoginPayload { enabled: boolean; schedules: { login_time: string; logout_time: string }[] }

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
