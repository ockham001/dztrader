import { defineStore } from 'pinia'
import { ref } from 'vue'
import { marketSourcesApi } from '@/api/marketSources'
import type { StartMarketSourceBody } from '@/api/marketSources'
import type { ProcessStatusPayload } from '@/types/api'
import { usePending } from '@/composables/usePending'

// 设计 §5.2：进程状态/控制领域（marketSources 进程部分拆出）
// - statuses: process_status 帧镜像（name → ProcessStatusPayload，后到覆盖先到）
// - configs:  process_config 帧镜像（name → config，全量覆盖——覆盖天然含删除，契约 process：
//   条目消失 = 进程已移除）
// - start/stop/removeSource: pending 迁移 usePending（key: source:{id}:{op}，op ∈ start/stop/remove）
//   pending 语义与 marketSources.ts 现有实现逐项一致（拆 store 不得改变清理触发点）：
//     HTTP 成功不清 pending（keepPendingOnSuccess 默认 true，等 process_status 带 event 清，
//     契约 process；原 rtn_process_control 消息后端已不推送，P6 迁移）
//     HTTP 失败清 pending + 返回 false
//     超时兜底（进程操作 30s，usePending 超时清 + toast）
//   超时 toast 经 distinguishTimeout + opLabel 输出可读文案；超时返回 true
//   symbol（PENDING_TIMEOUT 转真值）→ 聚合层 runOp 的 !ok 分支不命中，不设 error（§7.2 双 toast 去重）
// - applyProcessStatus: 解析 event 字段清对应 pending（成功 resolve / 失败 fail, 不弹 toast）；
//   RemoveSucceeded 写 removedNames（聚合层据此移除卡片）
const PROCESS_OP_TIMEOUT_MS = 30_000

export const useProcessStore = defineStore('process', () => {
  const statuses = ref<Record<string, ProcessStatusPayload>>({})
  const configs = ref<Record<string, Record<string, unknown>>>({})
  // process_config 全量镜像是否已初始化（首次 applyProcessConfig 后为 true）。
  // 用于区分"启动初期 configs 尚空（放行，防顺序竞态）"与
  // "全部进程已移除后的空 map（过滤已移除进程的晚到 status）"。
  const configsInitialized = ref(false)

  // 进程名 → source id 映射：start/stop/removeSource 调用时记录，
  // RTN 回调（仅带进程名 target）据此定位 pending key（source:{id}:{op}）
  const nameToId = ref<Record<string, number>>({})

  // Remove RTN 成功 ack（target → 时间戳）：本 store 不持有 sources 列表，
  // 卡片移除由聚合层（marketSources）watch 本字段执行（对照现有 setProcessControlRsp
  // Remove 成功时 filter sources 语义，P4 Task 5 单写化后迁移至此）
  const removedNames = ref<Record<string, number>>({})

  const { pending, run, resolve, fail } = usePending()

  function opKey(id: number, op: 'start' | 'stop' | 'remove'): string {
    return `source:${id}:${op}`
  }

  // process_status 帧：单条完整覆盖（后到覆盖先到），非法 payload 忽略不写
  // 契约 process: 带 event 的帧是 REQUEST_PROCESS_CONTROL 的响应——据此清对应 pending
  // （StartSucceeded/StartFailed→start、StopSucceeded/StopFailed→stop、
  //  RemoveSucceeded/RemoveFailed→remove；失败仅清 pending（反馈由 NOTIFY_UI 弹窗承载））；
  // event 缺失 = 自发状态变化（崩溃/重启/快照），不清 pending。
  // P6 修复: 原 rtn_process_control 消息后端已不推送（帧 114 已删），
  // pending 清理唯一入口迁移至此（契约 webui-ws §5 已知差异关闭）。
  function applyProcessStatus(payload: ProcessStatusPayload): void {
    if (!payload || typeof payload !== 'object' || !payload.name) return
    // 契约 process: 对已移除进程（process_config 已初始化且不含该进程）后续到达的
    // process_status 忽略（不重建镜像）。configs 未初始化时放行（快照/启动初期
    // 的顺序竞态：process_status 可能先于 process_config 到达）。
    if (configsInitialized.value && !(payload.name in configs.value)) {
      // 过滤只作用于镜像写入；event 副作用（清 pending）必须执行——
      // Remove 流程先推 RTN_PROCESS_CONFIG（条目消失）再推 RemoveSucceeded，
      // 此处不执行则 remove pending 悬挂至超时
      handleProcessEvent(payload)
      return
    }
    statuses.value[payload.name] = payload
    handleProcessEvent(payload)
  }

  // process_status 帧的 event 字段处理：清 pending + Remove 成功 ack（失败不弹 toast, 见下）
  function handleProcessEvent(payload: ProcessStatusPayload): void {
    const { name, event } = payload
    if (!event || typeof event !== 'string') return  // 自发状态变化, 不清 pending
    const sourceId = nameToId.value[name]
    if (sourceId === undefined) return  // 未经过本 store 的操作：pending 不存在, 忽略
    const opMap: Record<string, 'start' | 'stop' | 'remove'> = {
      StartSucceeded: 'start', StartFailed: 'start',
      StopSucceeded: 'stop', StopFailed: 'stop',
      RemoveSucceeded: 'remove', RemoveFailed: 'remove',
    }
    const op = opMap[event]
    if (!op) return
    const key = opKey(sourceId, op)
    if (event.endsWith('Failed')) {
      // 只清 pending, 不弹 toast: 失败反馈由 NOTIFY_UI 错误级别弹窗承载
      // （契约 notify-ui 要求失败路径必推 NOTIFY_UI popup=true, master 已实现）,
      // 此处再 toast 会造成双提示（弹窗 + 小 toast 并存, P4 §5.4 单出口原则）
      fail(key)
    } else {
      resolve(key)
      // Remove 成功 ack：聚合层据此移除卡片（B1 语义：Remove 含 Stop，
      // stopPending 聚合表达式 pending[stop] || pending[remove] 同步消失）
      if (event === 'RemoveSucceeded') {
        removedNames.value[name] = Date.now()
      }
    }
  }

  // process_config 帧：全量覆盖本地配置镜像（覆盖天然含删除——契约 process）
  // 防御：非对象 / 数组 payload 忽略（不写不抛）
  function applyProcessConfig(payload: Record<string, unknown>): void {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return
    const next: Record<string, Record<string, unknown>> = {}
    for (const [name, cfg] of Object.entries(payload)) {
      if (cfg && typeof cfg === 'object') {
        next[name] = cfg as Record<string, unknown>
      }
    }
    configs.value = next
    configsInitialized.value = true
  }

  // 进程控制操作：pending 语义与 marketSources.ts 现状逐项一致
  // 返回 true = HTTP 成功（pending 保持挂起，等 process_status event 清）；false = 失败/超时/已在进行
  // data（可选）透传给 start API：addAndStartSource 传入 display_name
  //（对照现状 addAndStartSource 的 start 调用，P4 Task 5 聚合层接入）
  async function start(sourceId: number, sourceName: string, data?: StartMarketSourceBody): Promise<boolean> {
    const key = opKey(sourceId, 'start')
    if (pending[key]) return false  // 防重入（现状: if (src.startPending) return）
    nameToId.value[sourceName] = sourceId
    const result = await run(key, () => marketSourcesApi.start(sourceId, data), {
      timeout: PROCESS_OP_TIMEOUT_MS,
      opLabel: '行情源启动',          // 超时 toast 文案（原为裸 key）
      distinguishTimeout: true,       // 超时 resolve PENDING_TIMEOUT（truthy），下方判断按"跳过 error"处理
    })
    // 超时返回 true（PENDING_TIMEOUT 转真值）→ 聚合层 runOp 的 !ok 分支不命中，不设 error，无双 toast
    return result !== undefined
  }

  async function stop(sourceId: number, sourceName: string): Promise<boolean> {
    const key = opKey(sourceId, 'stop')
    if (pending[key]) return false
    nameToId.value[sourceName] = sourceId
    const result = await run(key, () => marketSourcesApi.stop(sourceId), {
      timeout: PROCESS_OP_TIMEOUT_MS,
      opLabel: '行情源停止',          // 超时 toast 文案（原为裸 key）
      distinguishTimeout: true,       // 超时 resolve PENDING_TIMEOUT（truthy），下方判断按"跳过 error"处理
    })
    // 超时返回 true（PENDING_TIMEOUT 转真值）→ 聚合层 runOp 的 !ok 分支不命中，不设 error，无双 toast
    return result !== undefined
  }

  // 删除进程: 只 run source:{id}:remove 一个 pending（B1 的"Remove 同时清 stop"是
  // marketSources 卡片层语义, 由 Task 5 聚合回填处理; 本 store 仅迁移 pending 清理）
  async function removeSource(sourceId: number, sourceName: string): Promise<boolean> {
    const key = opKey(sourceId, 'remove')
    if (pending[key]) return false
    nameToId.value[sourceName] = sourceId
    const result = await run(key, () => marketSourcesApi.remove(sourceId), {
      timeout: PROCESS_OP_TIMEOUT_MS,
      opLabel: '行情源删除',          // 超时 toast 文案（原为裸 key）
      distinguishTimeout: true,       // 超时 resolve PENDING_TIMEOUT（truthy），下方判断按"跳过 error"处理
    })
    // 超时返回 true（PENDING_TIMEOUT 转真值）→ 聚合层 runOp 的 !ok 分支不命中，不设 error，无双 toast
    return result !== undefined
  }

  return {
    statuses,
    configs,
    removedNames,
    applyProcessStatus,
    applyProcessConfig,
    start,
    stop,
    removeSource,
  }
})
