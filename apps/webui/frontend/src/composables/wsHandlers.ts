// ===== WS 业务消息处理器注册表 =====
// 注册表分发（模块级副作用：import 即注册）。
// error/pong 为连接协议消息，留在 wsClient.ts 内部处理（不注册）。
import { registerHandler } from './wsClient'
import { clearByPrefix } from './usePending'
import { useUserManagementStore } from '@/stores/userManagement'
import { useLogsStore } from '@/stores/logs'
import { useAuthStore } from '@/stores/auth'
import { useMdConfigStore } from '@/stores/mdConfig'
import { useNotifyStore } from '@/stores/notify'
import { useProcessStore } from '@/stores/process'
import { useProgressStore } from '@/stores/progress'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useSettingsStore } from '@/stores/settings'
import type { LogLine, ProcessStatusPayload, MdConfigPayload, MdRtnConfigPayload, MdRtnStatusPayload } from '@/types/api'

registerHandler('data_changed', (payload) => {
  // 多设备同步：后端数据变更时推送通知，前端按 scope 重新 REST 刷新
  const scope = (payload as { scope?: string } | undefined)?.scope
  if (!scope) return
  if (scope === 'market_sources') {
    // 行情源列表 DB 真相源（契约 rest §2.3/§3）：条目增删改后重新拉取
    // 失败经 store.error → View watch toast 呈现（loadSources 内部已捕获不 rethrow）
    useMarketSourcesStore().loadSources()
    return
  }
  const userManagement = useUserManagementStore()
  userManagement.refreshByScope(scope)
})

registerHandler('log_line', (payload) => {
  const data = payload as { file?: string; line?: LogLine } | undefined
  if (data?.line) {
    const logs = useLogsStore()
    logs.appendLogLine(data.line)
  }
})

registerHandler('log_tail_unsubscribed', (payload) => {
  const logs = useLogsStore()
  logs.setTailEnabled(false)
  if ((payload as { file?: string; reason?: string } | undefined)?.reason === 'self_logs_cannot_be_tailed') {
    console.info('self logs cannot be tailed')
  }
})

registerHandler('snapshot', (payload) => {
  // 连接/重连后后端推送的全量镜像快照。结构：{ [instance_id]: { [domain]: payload } }。
  // P6 重连补拉简化（设计 §5.1）：snapshot 领域分发复用与增量帧相同的 apply 函数
  // （process_config/process_status/md_config/md_status/progress），幂等覆盖——
  // md_config/md_status 的 apply 承担"清 pending"副作用（onopen 不再 refreshConfigs/
  // QUERY_ALL 补拉，断线重连期间的挂起 pending 靠这里清除，否则悬挂至超时）。
  // log_config：日志进程列表经 applySnapshot 整体重建（随快照增删，保持列表一致），
  // 并兜底清 set_level pending（契约 webui-ws §6）；md_shm_config 已消费（契约 shm SHM 配置镜像，
  // 分发复用增量帧 apply）；event_shm_config 已消费（契约 shm 事件通道配置镜像，分发复用增量帧 apply）；
  // auto_login 已消费（契约 auto-login 排程镜像）。
  const mirror = (payload ?? {}) as Record<string, Record<string, unknown>>
  const logs = useLogsStore()
  logs.applySnapshot(mirror as Record<string, { log_config?: { level?: string } }>)
  const processStore = useProcessStore()
  const mdConfigStore = useMdConfigStore()
  const progressStore = useProgressStore()
  for (const [instanceId, domains] of Object.entries(mirror)) {
    if (!domains || typeof domains !== 'object') continue
    for (const [domain, value] of Object.entries(domains)) {
      switch (domain) {
        case 'process_status':
          processStore.applyProcessStatus(value as ProcessStatusPayload)
          break
        case 'process_config':
          processStore.applyProcessConfig(value as Record<string, unknown>)
          break
        case 'md_config':
          mdConfigStore.applyMdConfig({ source: instanceId, config: value as MdConfigPayload })
          break
        case 'md_status':
          mdConfigStore.applyMdStatus({ source: instanceId, status: value as Record<string, unknown> })
          break
        case 'auto_login':
          // 契约 auto-login: snapshot 分发复用增量帧 apply（重连后恢复排程/自动登录镜像）
          mdConfigStore.applyAutoLogin(instanceId, value)
          break
        case 'md_shm_config':
          // 契约 shm: snapshot 分发复用增量帧 apply（展示镜像 + 清悬挂 pending）
          mdConfigStore.applyMdShmConfig(instanceId, value)
          break
        case 'event_shm_config':
          // 契约 shm: snapshot 分发复用增量帧 apply (挂 dztraderd, 系统设置页展示)
          useSettingsStore().applyEventShmConfig(value)
          break
        case 'progress':
          progressStore.applyProgress(instanceId, value)
          break
        // log_config 已由上方 applySnapshot 整体重建并兜底清 set_level pending
      }
    }
  }
  // 契约 webui-ws §6: 快照分发后清进程类 pending（断连期间挂起的 start/stop/remove
  // 无法配对 event——快照是权威状态，悬挂 pending 只等 30s 超时兜底；
  // 此处按 source: 前缀统一清除，含 timer）。配置类 pending 由 md_config/auto_login
  // apply 承担（见各 store）。
  clearByPrefix('source:')
})

registerHandler('log_config', (payload, instanceId) => {
  // 单实例日志配置增量推送（RTN_LOG_CONFIG → WS）
  // data: { level, flush_on }，instance_id 标识目标进程
  const logs = useLogsStore()
  const data = payload as { level?: string } | undefined
  if (instanceId && data) {
    logs.applyLogConfig(instanceId, data)
  }
})

registerHandler('default_password_warning', () => {
  const auth = useAuthStore()
  auth.isDefaultPassword = true
})

registerHandler('process_status', (payload) => {
  // P4 Task 5 单写:process store 为 process_status 帧唯一写入点
  // P6: event 字段（契约 process）承担进程操作 pending 清理 + Remove 成功卡片移除
  // (原 rtn_process_control 消息后端已不推送, 见 webui-ws.md §5 已知差异关闭)
  useProcessStore().applyProcessStatus(payload as ProcessStatusPayload)
})

registerHandler('process_config', (payload) => {
  // P4 Task 3: process_config 帧接入镜像（P1 后端已推、前端此前未消费）
  // 全量映射 { [进程名]: config, ... }，整体覆盖（覆盖天然含删除，契约 process）
  useProcessStore().applyProcessConfig((payload ?? {}) as Record<string, unknown>)
})

registerHandler('md_rtn_status', (payload) => {
  // P4 Task 5 单写:mdConfig store 为 md_rtn_status 帧唯一写入点。
  // P6: 契约 md-status 无状态字段——loginState 聚合与 login/logout pending 清除
  // 由 RTN_PROGRESS（契约 progress）驱动, 本消息仅写镜像。
  useMdConfigStore().applyMdStatus(payload as MdRtnStatusPayload)
})

registerHandler('md_shm_config', (payload, instanceId) => {
  // 契约 shm: RTN_MD_SHM_CONFIG → WS md_shm_config（data=ShmConfig 全量, instance_id=行情进程名）
  // 到达清 shm_config pending
  if (instanceId) {
    useMdConfigStore().applyMdShmConfig(instanceId, payload)
  }
})

registerHandler('md_rtn_config', (payload) => {
  // dzmd_ctp 配置变化时推送 (已脱敏, 契约 md-config: brokers/current_broker_name/订阅参数)
  // data: { source: 'dzmd_ctp', config: { brokers: [...], ... } }
  // P4 Task 5 单写:mdConfig store 为 md_rtn_config 帧唯一写入点
  const data = payload as MdRtnConfigPayload | undefined
  if (!data?.source || !data?.config) {
    console.warn('md_rtn_config payload invalid', data)
    return
  }
  useMdConfigStore().applyMdConfig(data)
})

registerHandler('auto_login', (payload, instanceId) => {
  // 契约 auto-login: RTN_AUTO_LOGIN → WS auto_login 消息 (data: {enabled, schedules}, 始终全量)
  // P6 修复: 排程/自动登录单一真相源——前端镜像 + 全量提交 SET_AUTO_LOGIN
  if (instanceId) {
    useMdConfigStore().applyAutoLogin(instanceId, payload)
  }
})

registerHandler('notify_ui', (payload) => {
  // 纯通知帧: 仅显示, 不清 pending（契约 notify-ui）
  // pending 清理由对应 RTN 帧承担:
  //   - 配置类 pending → md_rtn_config / auto_login
  //   - 进程类 pending → process_status 的 event 字段（契约 process）
  //   - 登录类 pending → progress 状态转移（契约 progress）
  // data: { source?: string, message?: string, level?: string, popup?: boolean, timestamp?: number }
  // timestamp 为契约 notify-ui 的 Unix 秒级发送方时间戳，原样透传（单位语义见 notify store push 注释）
  //   level 为字符串：'info' | 'warning' | 'error'（与 log level 规范全称一致）
  //   popup=true 且 error 级别: 契约 notify-ui 前端义务——必须打断用户展示
  //   （NotifyStore 入 popup 队列, App.vue 渲染 modal 逐条确认）
  // 双 toast 修复（设计 §5.4）：notify_ui → useNotifyStore → toast，不写 error.value
  //   （error.value 仅 HTTP 失败链路写入，两通道互斥）
  const data = payload as { source?: string; message?: string; level?: string; popup?: boolean; timestamp?: number } | undefined
  // level 已为字符串, 未匹配/缺失时默认 error
  const rawLevel = data?.level
  const level: 'info' | 'warning' | 'error' =
    rawLevel === 'info' ? 'info' : rawLevel === 'warning' ? 'warning' : 'error'
  useNotifyStore().push(data?.message ?? '', data?.source, level, data?.popup === true,
    typeof data?.timestamp === 'number' ? data.timestamp : undefined)
})

registerHandler('progress', (payload, instanceId) => {
  // 契约 progress 镜像：单条完整状态，后到覆盖先到（P2 后端已推，前端接入）
  // data: { min, max, current, desc? }，instance_id 标识目标进程
  if (instanceId) {
    useProgressStore().applyProgress(instanceId, payload)
  }
})

registerHandler('event_shm_config', (payload) => {
  // 契约 shm: RTN_EVENT_SHM_CONFIG → WS event_shm_config (data=ShmConfig 全量, 挂 dztraderd)
  // 到达清事件通道配置 pending; 系统设置页展示镜像
  useSettingsStore().applyEventShmConfig(payload)
})
