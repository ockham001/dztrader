// 由 scripts/gen-types.mjs 从 schema/domain-payloads.schema.json 生成 — 请勿手改
// 真源：docs/frame_contracts/*.md 与 libs/platform 头文件；改 schema 后重跑 npm run gen:types

// 进程状态（契约 process）
export type ProcessState = 'Starting' | 'Running' | 'Stopping' | 'Stopped' | 'Crashed'

// 进程操作结果事件（契约 process）
export type ProcessEvent = 'StartSucceeded' | 'StartFailed' | 'StopSucceeded' | 'StopFailed' | 'RemoveSucceeded' | 'RemoveFailed'

export interface ProcessStatusPayload {
    /** 进程名 */
    name: string
    state: ProcessState
    /** 进程 PID，未运行时为 0 */
    pid: number
    /** 用户可读显示名；缺省为空串 */
    display_name?: string
    /** 状态说明/失败原因；缺省为空串 */
    message?: string
    /** 操作结果事件；缺失=自发状态变化 */
    event?: ProcessEvent
}

export interface ProgressStatus {
    min: number
    /** max <= min（含 max==0）表示不确定进度 */
    max: number
    current: number
    /** 简短文本说明；空串/省略=无文本 */
    desc?: string
}
