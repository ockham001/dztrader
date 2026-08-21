import type { UsePending } from './usePending'
import { PENDING_TIMEOUT } from './usePending'

// P3 任务 2: useOperation 统一「防重入 + name→id 映射 + run + 结果映射」机械样板。
//
// 背景：process / mdConfig 两个 store 的每个操作都重复同一段样板——
//   const key = opKey(id, 'op')
//   if (pending[key]) return false            // 防重入
//   nameToId.value[sourceName] = id           // 记录映射（RTN 回调据此定位 key）
//   const result = await run(key, () => api(...),
//     { timeout, opLabel, distinguishTimeout: true })
//   if (result === PENDING_TIMEOUT) return PENDING_TIMEOUT  // mdConfig style
//   return result !== undefined               // boolean style
//
// createOperationRunner 按「领域」构建一次，收敛上述机械部分：
//   - busy: 防重入检查
//   - assign: 记录 sourceName→id
//   - execute: run + 结果映射（成功→true [保持 pending 等 WS RTN 清] / 失败→false /
//     超时→PENDING_TIMEOUT），超时 toast 文案统一经 opLabel
// 各操作**异构的守卫**（broker 是否存在 / auto_login 镜像是否就绪 / 空 patch 幂等 /
// 乐观更新回滚等）仍在调用处原样保留——execute 不吞守卫，不造漏气抽象。
//
// 结果映射两种消费方式：
//   - mdConfig style（返回 boolean | PENDING_TIMEOUT）：直接 return runner.execute(...)
//   - process style（返回 boolean，超时视作成功跳过 error）：return (await execute) !== false

/** 操作结果（P3 任务 2 统一）：成功 true / 失败 false / 超时 PENDING_TIMEOUT（usePending 已 toast） */
export type OperationResult = boolean | typeof PENDING_TIMEOUT

export interface OperationRunnerOptions {
  /** 领域 pending 实例（防重入 / run / 清理共用同一空间） */
  pending: UsePending
  /** 默认超时（ms）：进程控制 30s / 配置类 10s */
  timeout: number
  /** key 生成：source:{id}:{op} */
  opKey(id: number, op: string): string
  /** sourceName→id 映射表（ref），RTN 回调据此定位 pending key */
  nameToId: { value: Record<string, number> }
}

export interface OperationRunner {
  /** 生成 pending key */
  opKey(id: number, op: string): string
  /** 防重入检查：该 key 是否已在进行（true 则调用方短路返回） */
  busy(key: string): boolean
  /** 记录 sourceName→id 映射，供 RTN 回调定位 key */
  assign(id: number, sourceName: string): void
  /** 执行 run + 结果映射（唯一一处 sample 落地） */
  execute(key: string, fn: () => Promise<unknown>, opLabel: string): Promise<OperationResult>
}

export function createOperationRunner(opts: OperationRunnerOptions): OperationRunner {
  const { pending, timeout, opKey, nameToId } = opts
  return {
    opKey,
    busy: (key) => pending.pending[key] ?? false,
    assign: (id, sourceName) => {
      nameToId.value[sourceName] = id
    },
    execute: async (key, fn, opLabel) => {
      const result = await pending.run(key, fn, { timeout, opLabel, distinguishTimeout: true })
      if (result === PENDING_TIMEOUT) return PENDING_TIMEOUT
      return result !== undefined
    },
  }
}