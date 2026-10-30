/**
 * 跨平台语义提示的中文展示（#207 B2；契约 sysmon §2.5）
 *
 * 契约只规定 `availability.semantic_hints` 是「key → 文案」的映射，**没有规定语言**；
 * 发送方（dzsysmon）用英文小写、无句号的风格（与 `reason` 文案一致）。中文界面原样展示
 * 英文长句（「语义提示：latency is the average…」）对用户不可读。
 *
 * 因此展示层按**稳定 key** 映射中文文案，后端原文作为未知 key 的 fallback 与 tooltip
 * 保留——不改进协议、不引入前端 i18n 框架（本仓目前没有）。
 *
 * 约束：
 * - key 是契约里定义的指标键（`cpu.load_average` / `disk_io`），不是文案。发送方改文案、
 *   新增平台时中文映射仍然有效；新增 key 时前端未命中即回退原文（不吞提示）。
 * - 中文文案必须表达**同一口径事实**，不得比原文多也不得少（原文是可以被对照的真相）。
 * - **已知边界**：本模块只按 key 索引，因此假设「同一 key 在同一时期只有一种口径」。
 *   当前成立——发送方只在 `semantic=processor_queue` 时携带 `cpu.load_average` 提示
 *   （`apps/sysmon/snapshot_builder.cpp`），Linux 原生负载不携带；`disk_io` 两平台同口径。
 *   若将来同一 key 需要按平台/`semantic` 给出不同中文口径，须改成 key+semantic 双键查表，
 *   并同步 `apps/sysmon/snapshot_builder.cpp` 的 key 约定与契约 sysmon §2.5。
 */

/** key → 中文文案（与 dzsysmon 发送的英文原文语义一一对应） */
export const SEMANTIC_HINT_TEXT: Record<string, string> = {
  // apps/sysmon/snapshot_builder.cpp 的 kProcessorQueueHint：本平台不提供原生 load average，
  // 以处理器队列长度做指数加权移动平均代理，与 Unix load average 不是同一个量。
  // 文案不写死 OS 名（契约按 key 而非平台承载提示，发送方只在代理量语义下携带它）。
  'cpu.load_average':
    '本平台不提供 Unix 负载均值，此处为处理器队列长度的指数加权移动平均代理量；' +
    '与 Unix load average 不是同一个量，不要同轴比较',
  // 同文件的 kDiskIoHint：延迟是每完成请求的平均驻留时间（含排队），
  // 队列深度是采样时刻的在途请求数（不是区间平均）。
  disk_io:
    '延迟是每个已完成请求的平均驻留时间（含排队等待）；' +
    '队列深度是采样时刻的在途请求数，不是区间平均值',
}

/**
 * 取语义提示的展示文案。
 *
 * @param key 契约里的指标键（如 `disk_io`）
 * @param raw 发送方原始文案；为空串表示该 key 无提示
 * @returns 已命中 key 时返回中文；未命中时回退发送方原文（不吞提示）；两者都空时返回空串
 */
export function semanticHintText(key: string, raw: string): string {
  const text = raw.trim()
  if (text === '') return ''
  return SEMANTIC_HINT_TEXT[key] ?? text
}
