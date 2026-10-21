import { describe, it, expectTypeOf } from 'vitest'
import type { WsDataByType, WsServerMessage, WsServerMessageType } from '@/types/ws'
import type {
  ProcessStatusPayload, ProgressStatus, ShmConfigView, MdRtnConfigPayload, MdRtnStatusPayload,
  NotifyUiPayload, AutoLoginPayload, LogConfigPayload, LogLine,
  SysmonSnapshotPayload, SysmonConfigPayload,
} from '@/types/generated'

// 判别联合编译期保障：WsDataByType 的 type→payload 映射必须与契约单源（generated）一致。
// 若 schema/契约增改字段后未同步，或某键与 generated 类型漂移，本套断言在 type-check 阶段即失败。
describe('WsDataByType 判别联合（契约单源一致性）', () => {
  it('type 全集 = 契约 webui-ws §2 S2C 清单（领域消息 + 控制消息 + 5 个 ack）', () => {
    // 清单真源：docs/frame_contracts/webui-ws.md §2.2 / §2.3 / §2.4
    type ContractS2CTypes =
      | 'snapshot'
      | 'process_status'
      | 'process_config'
      | 'md_rtn_config'
      | 'md_rtn_status'
      | 'md_rtn_subscriptions'
      | 'notify_ui'
      | 'log_config'
      | 'event_shm_config'
      | 'md_shm_config'
      | 'auto_login'
      | 'progress'
      | 'sysmon_snapshot'
      | 'sysmon_config'
      | 'log_line'
      | 'log_tail_unsubscribed'
      | 'data_changed'
      | 'default_password_warning'
      | 'error'
      | 'pong'
      | 'md_connect_ack'
      | 'md_disconnect_ack'
      | 'subscribe_log_ack'
      | 'unsubscribe_log_ack'
      | 'query_md_subscriptions_ack'
    expectTypeOf<WsServerMessageType>().toEqualTypeOf<ContractS2CTypes>()
  })

  it('process_status → ProcessStatusPayload（含契约 event 字段）', () => {
    expectTypeOf<WsDataByType['process_status']>().toEqualTypeOf<ProcessStatusPayload>()
  })

  it('progress → ProgressStatus（min/max/current 必填）', () => {
    expectTypeOf<WsDataByType['progress']>().toEqualTypeOf<ProgressStatus>()
  })

  it('log_config → LogConfigPayload 同源（level/flush_on）', () => {
    expectTypeOf<WsDataByType['log_config']>().toEqualTypeOf<LogConfigPayload>()
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

  it('sysmon_snapshot / sysmon_config 与 generated 同源（契约 sysmon）', () => {
    expectTypeOf<WsDataByType['sysmon_snapshot']>().toEqualTypeOf<SysmonSnapshotPayload>()
    expectTypeOf<WsDataByType['sysmon_config']>().toEqualTypeOf<SysmonConfigPayload>()
  })

  it('md_rtn_status / auto_login 与 generated 同源', () => {
    expectTypeOf<WsDataByType['md_rtn_status']>().toEqualTypeOf<MdRtnStatusPayload>()
    expectTypeOf<WsDataByType['auto_login']>().toEqualTypeOf<AutoLoginPayload>()
  })

  it('其余契约键逐个锚定（未迁移域显式声明为 unknown）', () => {
    expectTypeOf<WsDataByType['snapshot']>().toBeUnknown()
    expectTypeOf<WsDataByType['process_config']>().toEqualTypeOf<Record<string, unknown>>()
    expectTypeOf<WsDataByType['md_rtn_subscriptions']>().toBeUnknown()
    expectTypeOf<WsDataByType['log_line']>().toEqualTypeOf<{ file?: string; line?: LogLine }>()
    expectTypeOf<WsDataByType['log_tail_unsubscribed']>().toBeUnknown()
    expectTypeOf<WsDataByType['data_changed']>().toBeUnknown()
    expectTypeOf<WsDataByType['default_password_warning']>().toBeUnknown()
    expectTypeOf<WsDataByType['error']>().toEqualTypeOf<{ message: string }>()
    expectTypeOf<WsDataByType['pong']>().toEqualTypeOf<void>()
  })

  it('ack 载荷与契约 webui-ws §2.4 一致（控制消息，payload 键）', () => {
    expectTypeOf<WsDataByType['md_connect_ack']>()
      .toEqualTypeOf<{ source: string; ok: boolean }>()
    expectTypeOf<WsDataByType['md_disconnect_ack']>()
      .toEqualTypeOf<{ source: string; ok: boolean }>()
    expectTypeOf<WsDataByType['subscribe_log_ack']>().toEqualTypeOf<{ file: string }>()
    expectTypeOf<WsDataByType['unsubscribe_log_ack']>().toEqualTypeOf<Record<string, never>>()
    expectTypeOf<WsDataByType['query_md_subscriptions_ack']>()
      .toEqualTypeOf<{ source: string; ok: boolean }>()
  })

  it('WsServerMessage 按 type 判别 data 类型（显式实例化）', () => {
    expectTypeOf<WsServerMessage<'process_status'>['data']>()
      .toEqualTypeOf<ProcessStatusPayload | undefined>()
  })

  it('值层面按 type 收窄：unknown 键不吸收其他分支的 data 类型', () => {
    const msg = {
      type: 'process_status',
      data: { name: 'dzmd_ctp', state: 'Running', pid: 1, message: '' },
    } as unknown as WsServerMessage
    if (msg.type === 'process_status') {
      // 若 WsServerMessage 退化为非分布式 interface，这里会变成 unknown
      expectTypeOf(msg.data).toEqualTypeOf<ProcessStatusPayload | undefined>()
    }

    const ack = { type: 'md_connect_ack', payload: { source: 'dzmd_ctp', ok: true } } as unknown as WsServerMessage
    if (ack.type === 'md_connect_ack') {
      expectTypeOf(ack.data).toEqualTypeOf<{ source: string; ok: boolean } | undefined>()
    }
  })
})
