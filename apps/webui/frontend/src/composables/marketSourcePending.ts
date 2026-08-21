import { usePending } from './usePending'
import type { UsePending } from './usePending'

// 行情源领域共享 pending 实例（P3 交互范式固化）。
//
// 背景：process / mdConfig / marketSources 三个 store 围绕**同一批行情源操作**
// 的 pending（key 统一 `source:{id}:{op}`）：
//   - process store `run`（start/stop/remove 等进程控制，写 SHM 帧）；
//   - mdConfig store 在 RTN 到达时 `clearByPrefix(source:{id}:{op})` 清配置类 pending，
//     并在 snapshot 的 process_status/process_config 分发清挂起 pending；
//   - marketSources store 只读 `pending[keyOf(...)]` 映射为 UI 字段。
// 三者必须共享同一空间——若各自 usePending() 实例化，process run 的 key 在 mdConfig
// 的 RTN 清理时不可见，pending 将悬挂至超时。
//
// wsHandlers 的 snapshot 处理还需跨这些 store 统一 `clearByPrefix('source:')`
// （契约 webui-ws §6：快照分发后清进程类挂起 pending）。
//
// 故此模块持有**唯一实例**并导出，作为行情源领域的物理 pending 空间：
// 取代全局单例 + "source:" 前缀约定，防止误伤同前缀的其他领域。

/** 行情源领域共享 pending 实例（模块单例，全应用唯一） */
export const marketSourcePending: UsePending = usePending()

export const { clearByPrefix, resolve, fail, __resetForTests } = marketSourcePending