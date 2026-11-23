/**
 * 指标不可用原因的展示文案（纯函数；契约 sysmon §2.5 reason 建议取值的展示映射）
 *
 * 为什么单独成模块：这张映射原先住在 `composables/monitorLedger.ts`（#363 台账）里，台账随
 * #450 的存储 / 网络 / 时钟三块落地整体删除，但「缺数据怎么说」这句话仍被 CPU / 内存 / 存储 /
 * 网络 / 时钟各块共用。把它留在视图里会重新长出第二条 if 链（review-checklist §2.4：同一事实
 * 两处定义必然在改口径时漂移），因此拆成独立的纯函数模块——本模块不读 store、不含渲染。
 *
 * 三条约定：
 * 1. `reason` 是**自由文案**（契约 §2.5），不认识的原因原样显示而不是归并成「不可用」——
 *    把 `some_new_reason` 抹成「不可用」会让新增的采集失败原因永远不可见。
 * 2. `disk_io` 的 `no_permission` 有可操作的专属说明（Windows 性能计数器需特权或未开启）：
 *    与通用的「权限不足」并列会让用户去查权限而不是去开计数器。这是**指标语境**下的文案
 *    细化，通用映射仍是一份（本函数），视图不再另写 if 链。
 * 3. `disabled_by_config` 与「不可用」分开：它是唯一可以自助恢复的一类（去配置页打开），
 *    与「平台不支持」「采集失败」混在一起会让用户去查文档 / 查进程而不是改配置。
 */

/** 配置关闭的统一文案（指标空态与卷原因共用，避免两处字面量漂移） */
export const DISABLED_BY_CONFIG_TEXT = '已在配置中关闭'

/**
 * 不可用原因 → 中文文案。
 *
 * @param reason 契约 §2.5 的 `availability.metrics[key].reason`；`undefined` 表示映射缺失
 *        （按不可用处理，与 store 同口径）
 * @param metric 指标键（目前只有 `disk_io` 会改变文案；其余走通用映射）
 */
export function unavailableReasonText(reason: string | undefined, metric?: string): string {
  if (!reason) return '当前快照未提供该指标'
  if (reason === 'not_implemented') return '该指标将在后续版本接入'
  if (reason === 'collect_failed') return '采集失败，暂不可用'
  if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
  if (reason === 'no_permission') {
    return metric === 'disk_io'
      ? '磁盘性能计数器不可用（需管理员权限或性能计数器未开启）'
      : '权限不足，无法读取该指标'
  }
  return reason
}
