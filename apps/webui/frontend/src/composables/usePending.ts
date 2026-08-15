import { reactive } from 'vue'
import { useToastStore } from '@/stores/toast'

// 统一 pending 样板 composable(设计 §5.3)
// 三选项: timeout / minPendingMs / keepPendingOnSuccess
// timer 生命周期约束: 超时触发/清除/失败三条路径均 timers.delete(key), 防 7×24 泄漏
// 模块级 reactive pending 状态, 跨组件共享 (WS RTN 回调通过 resolve 清除)

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

// 模块级 reactive pending 状态(跨组件共享)
const pending = reactive<Record<string, boolean>>({})

// 模块级 timer Map: 记录超时 timer + 开始时间 + minPendingMs,
// resolve/fail 时据此计算已耗时并决定是否延迟清除(防闪烁)
const timers = new Map<string, PendingEntry>()

function clearPending(key: string): void {
  pending[key] = false
}

// M4(设计 §5.2): 按领域前缀批量清 pending + timer
// 如 setMdConfig 批量操作前清 source 维度全部 pending, 防残留挂起态
export function clearByPrefix(prefix: string): void {
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

// 测试专用: 重置模块级状态(pending + timers)
export function __resetForTests(): void {
  for (const entry of timers.values()) {
    clearTimeout(entry.timer)
  }
  timers.clear()
  for (const k of Object.keys(pending)) {
    delete pending[k]
  }
}

export function usePending(): {
  pending: Record<string, boolean>
  run<T>(key: string, fn: () => Promise<T>, opts: RunPendingOptions & { distinguishTimeout: true }): Promise<T | undefined | typeof PENDING_TIMEOUT>
  run<T>(key: string, fn: () => Promise<T>, opts?: RunPendingOptions): Promise<T | undefined>
  resolve(key: string): void
  fail(key: string, message?: string): void
} {
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

  return { pending, run, resolve, fail }
}
