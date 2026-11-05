// ===== 快照镜像（dzweb MirrorStore 全量）=====
// snapshot 帧 data 结构: Record<instance_id, Partial<DomainPayloads>>
// P3 Task 1: 建立领域载荷骨架
// 载荷细化为 unknown 是终态（设计 §5.5 实现偏差：判别联合预留至交易数据扩展前实现）

// LogConfigPayload / AutoLoginPayload / NotifyUiPayload 由契约单源生成（schema → generated.ts），禁止手写
import type { LogConfigPayload, AutoLoginPayload, AutoLoginSchedule, NotifyUiPayload, ShmConfigView, SysmonSnapshotPayload, SysmonConfigPayload, TdConfigPayload, TdStatusPayload } from './generated'
export type { AutoLoginPayload, AutoLoginSchedule, LogConfigPayload, NotifyUiPayload, ShmConfigView, SysmonSnapshotPayload, SysmonConfigPayload, TdConfigPayload, TdStatusPayload }

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
  // 契约 sysmon: 快照域与配置域固定挂载实例 dzsysmon，进快照（新连接立即拿到当前值）
  sysmon_snapshot?: SysmonSnapshotPayload
  sysmon_config?: SysmonConfigPayload
  // 契约 td-config §8 / 契约 webui-ws §2.1：交易网关配置镜像，key = 网关进程名（如 dztd_ctp）。
  // 与 md_config 同型——全量覆盖、后到覆盖先到、进快照（账户配置页基线由 snapshot + 增量构成，
  // 不需要额外 REST 基线拉取）。
  td_config?: TdConfigPayload
  // 契约 td-status §3 / 契约 webui-ws §2.1：账户级细状态镜像，key = 网关名:账户ID
  //（如 dztd_ctp:account_001）。前端按 instance_id 关联账户行，全量覆盖更新。
  td_status?: TdStatusPayload
}

export type MirrorSnapshot = Record<string, Partial<DomainPayloads>>
