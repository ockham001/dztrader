import { reactive } from 'vue'
import { useToastStore } from '@/stores/toast'

// 统一 pending 样板 composable(设计 §5.3)
// 三选项: timeout / minPendingMs / keepPendingOnSuccess
// timer 生命周期约束: 超时触发/清除/失败三条路径均 timers.delete(key), 防 7×24 泄漏
//
// P3 交互范式固化：usePending 从全局单例改为**可实例化工厂**——每次调用获得
// 独立的 pending/timers 空间（无模块级共享状态）。
// - 领域化：跨 store 共享的 pending（如行情源领域 source:{id}:{op}，process/mdConfig/
//   marketSources 围绕同一批操作、且 snapshot 的 clearByPrefix('source:') 需跨 store 清）
//   由独立领域模块持有**唯一实例**（见 marketSourcePending.ts），物理隔离替代前缀约定；
// - 独立空间：logs/settings 等本无跨 store 交互的领域，直接各自实例化。
// 取消模块级导出 clearByPrefix / __resetForTests（改为实例方法，避免再出现全局状态）。

// 超时哨兵: run 超时且 distinguishTimeout=true 时 resolve 该值。
// 调用方可据此区分「超时」(usePending 已 toast) 与「HTTP 失败」——薄代理
// 对超时不重复弹 error, 避免双 toast (§7.2)。truthy 符号, 现有 `!== undefined`
// 判断在 success/failure 语义上仍成立(超时视为成功跳过 error, 与去重目标一致)。
export const PENDING_TIMEOUT = Symbol('pending.timeout')

export interface RunPendingOptions {
  timeout?: number        // 默认 10_000; 进程操作 30_000
  minPendingMs?: number   // 最短展示时间(logs 300ms 防闪烁), 默认 0
  keepPendingOnSuccess?: boolean  // 默认 true: 成功不清 pending, 等 resolve(WS RTN 清)
  /** 超时 toast 的可读文案(如「行情源登录」), 无则兜底 `${key} 操作超时` */
  opLabel?: string
  /** 默认 false: 超时返回 undefined(现状, logs 等 `!resp` 判断不受影响);
   *  true: 超时返回 PENDING_TIMEOUT, 供薄代理识别去重双 toast */
  distinguishTimeout?: boolean
}

interface PendingEntry {
  timer: ReturnType<typeof setTimeout>
  startTime: number
  minPendingMs: number
}

const DEFAULT_TIMEOUT = 10_000

export interface UsePending {
  pending: Record<string, boolean>
  run<T>(key: string, fn: () => Promise<T>, opts: RunPendingOptions & { distinguishTimeout: true }): Promise<T | undefined | typeof PENDING_TIMEOUT>
  run<T>(key: string, fn: () => Promise<T>, opts?: RunPendingOptions): Promise<T | undefined>
  resolve(key: string): void
  fail(key: string, message?: string): void
  /** M4(设计 §5.2): 按领域前缀批量清 pending + timer（实例方法，仅作用本实例） */
  clearByPrefix(prefix: string): void
  /** 测试专用: 重置本实例状态（pending + timers） */
  __resetForTests(): void
}

export function usePending(): UsePending {
  // 每实例独立的 pending/timers（P3：取代原模块级全局状态）
  const pending = reactive<Record<string, boolean>>({})
  const timers = new Map<string, PendingEntry>()

  function clearPending(key: string): void {
    pending[key] = false
  }

  // M4(设计 §5.2): 按领域前缀批量清 pending + timer（实例方法）
  function clearByPrefix(prefix: string): void {
    for (const key of Object.keys(pending)) {
      if (key.startsWith(prefix)) {
        delete pending[key]
      }
    }
    for (const key of [...timers.keys()]) {
      if (key.startsWith(prefix)) {
        clearTimer(key)
      }
    }
  }

  function clearTimer(key: string): void {
    const entry = timers.get(key)
    if (entry !== undefined) {
      clearTimeout(entry.timer)
      timers.delete(key)
    }
  }

  // 结束路径(keepPendingOnSuccess=false 成功 / resolve / fail):
  // 先清超时 timer; 若 elapsed < minPendingMs 延迟到满时长再清(防闪烁)
  function settle(key: string, startTime: number, minPendingMs: number): void {
    // M2: 仅当 key 已在 pending 中存在才写/删——防未 run 过的 key(resolve/fail
    // 由 WS RTN 回调触发, 可能带任意 key)产生垃圾条目
    if (!(key in pending)) return
    clearTimer(key)
    const elapsed = Date.now() - startTime
    if (elapsed < minPendingMs) {
      const t = setTimeout(() => {
        clearPending(key)
        timers.delete(key)
      }, minPendingMs - elapsed)
      timers.set(key, { timer: t, startTime, minPendingMs })
    } else {
      clearPending(key)
    }
  }

  function run<T>(key: string, fn: () => Promise<T>, opts?: RunPendingOptions): Promise<T | undefined | typeof PENDING_TIMEOUT> {
    const { timeout = DEFAULT_TIMEOUT, minPendingMs = 0, keepPendingOnSuccess = true, opLabel, distinguishTimeout = false } = opts ?? {}

    // 重复 run 同 key: 替换旧 timer(含未决的延迟清除 timer), 不冲突
    clearTimer(key)
    pending[key] = true
    const startTime = Date.now()

    return new Promise<T | undefined | typeof PENDING_TIMEOUT>((resolvePromise) => {
      // 超时 timer: 触发即清 pending + toast + timers.delete(key)
      const t = setTimeout(() => {
        clearPending(key)
        timers.delete(key)
        useToastStore().error(opLabel ? `${opLabel}超时` : `${key} 操作超时`)
        resolvePromise(distinguishTimeout ? PENDING_TIMEOUT as unknown as T : undefined)
      }, timeout)
      timers.set(key, { timer: t, startTime, minPendingMs })

      // T2①: Promise.resolve().then(fn) 包裹——fn 同步 throw 转为 rejection,
      // 走下方 fail 分支(清 pending), 不 reject 调用方 promise
      Promise.resolve()
        .then(fn)
        .then(
          (result) => {
            if (keepPendingOnSuccess) {
              // 成功保持 pending, 等 resolve (WS RTN 清)
              resolvePromise(result)
            } else {
              settle(key, startTime, minPendingMs)
              resolvePromise(result)
            }
          },
          () => {
            // 失败: 吞异常返回 undefined (调用方如需错误信息从 catch 自行处理)
            settle(key, startTime, minPendingMs)
            resolvePromise(undefined)
          },
        )
    })
  }

  // WS RTN 回调调用: 清 timer + pending(受 minPendingMs 约束)
  function resolve(key: string): void {
    const entry = timers.get(key)
    settle(key, entry?.startTime ?? Date.now(), entry?.minPendingMs ?? 0)
  }

  // 清 timer + pending(不弹 toast, 由调用方处理)
  function fail(key: string, _message?: string): void {
    const entry = timers.get(key)
    settle(key, entry?.startTime ?? Date.now(), entry?.minPendingMs ?? 0)
  }

  function __resetForTests(): void {
    for (const entry of timers.values()) {
      clearTimeout(entry.timer)
    }
    timers.clear()
    for (const k of Object.keys(pending)) {
      delete pending[k]
    }
  }

  return { pending, run, resolve, fail, clearByPrefix, __resetForTests }
}