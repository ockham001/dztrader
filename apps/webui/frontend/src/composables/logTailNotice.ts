/**
 * 日志实时跟随的禁用/退订说明文案（#207 C2）
 *
 * 同一句用户可见文案有两个触发点：
 * 1. `LogViewerTab.vue`：选中 WebUI 自身日志时开关被禁用，展示原因（可见文案 + title）；
 * 2. `wsHandlers.ts`：后端 `log_tail_unsubscribed{reason=self_logs_cannot_be_tailed}`
 *    到达时转成一次性 toast。
 *
 * 两处必须是同一句话（否则用户会以为遇到了两个不同问题），因此文案收在本模块导出常量，
 * 禁止在别处再写字面量——这是「同一事实两处定义」的收敛点（review-checklist §2.4）。
 *
 * 文案内容对应契约 webui-ws §2.3 的 `self_logs_cannot_be_tailed` 原因：
 * 后端禁止订阅 dzweb 自身日志，避免日志 tail 推送再写日志形成反馈循环。
 */

/** WebUI 自身日志不可实时跟随的原因（可见文案与 toast 共用） */
export const SELF_LOG_TAIL_DISABLED_TEXT = 'WebUI 自身日志不支持实时跟随（避免日志反馈循环）'

/** 实时推送连续失败被服务端自动退订（契约 webui-ws §2.3 `send failures`） */
export const LOG_TAIL_SEND_FAILURES_TEXT = '实时日志推送连续失败，已自动停止跟随'
