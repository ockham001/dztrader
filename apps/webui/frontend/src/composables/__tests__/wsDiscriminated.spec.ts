import { describe, it, expectTypeOf } from 'vitest'
import type { WsDataByType, WsServerMessage } from '@/types/ws'
import type {
  ProcessStatusPayload, ProgressStatus, ShmConfigView, MdRtnConfigPayload, NotifyUiPayload,
} from '@/types/generated'

// 判别联合编译期保障：WsDataByType 的 type→payload 映射必须与契约单源（generated）一致。
// 若 schema/契约增改字段后未同步，或某键与 generated 类型漂移，本套断言在 type-check 阶段即失败。
describe('WsDataByType 判别联合（契约单源一致性）', () => {
  it('process_status → ProcessStatusPayload（含契约 event 字段）', () => {
    expectTypeOf<WsDataByType['process_status']>().toEqualTypeOf<ProcessStatusPayload>()
  })

  it('progress → ProgressStatus（min/max/current 必填）', () => {
    expectTypeOf<WsDataByType['progress']>().toEqualTypeOf<ProgressStatus>()
  })

  it('log_config → LogConfigPayload 同源（level/flush_on）', () => {
    expectTypeOf<WsDataByType['log_config']>().toMatchTypeOf<{ level: string; flush_on: string }>()
  })

  it('notify_ui → NotifyUiPayload（含 level 枚举）', () => {
    expectTypeOf<WsDataByType['notify_ui']>().toEqualTypeOf<NotifyUiPayload>()
  })

  it('md_shm_config / event_shm_config → ShmConfigView（含 preload_points map）', () => {
    expectTypeOf<WsDataByType['md_shm_config']>().toEqualTypeOf<ShmConfigView>()
    expectTypeOf<WsDataByType['event_shm_config']>().toEqualTypeOf<ShmConfigView>()
  })

  it('md_rtn_config → MdRtnConfigPayload（source + config）', () => {
    expectTypeOf<WsDataByType['md_rtn_config']>().toEqualTypeOf<MdRtnConfigPayload>()
  })

  it('WsServerMessage 按 type 判别 data 类型（data 可选）', () => {
    expectTypeOf<WsServerMessage<'process_status'>['data']>().toEqualTypeOf<ProcessStatusPayload | undefined>()
  })
})