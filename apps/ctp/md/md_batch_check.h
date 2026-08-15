#ifndef DZTRADER_CTP_MD_BATCH_CHECK_H_
#define DZTRADER_CTP_MD_BATCH_CHECK_H_

namespace dztrader::ctp {

// md_batch_check.h: on_batch_complete 决策 (纯声明, 无 CTP 头依赖)
// 独立头文件: 枚举值无法 forward declare 使用, 测试目标 (md_api_pure_test)
// 需 include 本头而不能 include md_api.h (会拉入 ThostFtdcMdApi.h)。

/// on_batch_complete 的决策动作。
enum class BatchCheckAction {
    ContinueSend,  ///< 队列有滞留批次 (已入队未发送): 继续发送链, 不计重试
    Done,          ///< 无 Pending 合约: 补订完成, 复位链路状态
    Retry,         ///< 有 Pending 且未超限: 重新入队重试
    GiveUp,        ///< 有 Pending 且超限: 放弃 (reset Pending 为 NotRequested)
};

/// on_batch_complete 决策 (纯函数, 实现在 md_api_pure.cpp, 供单元测试)。
/// queue_nonempty 优先于重试判定: 滞留批次是"已入队未发送"而非"已发未确认",
/// 继续发送链即可, 不得消耗重试次数 (否则订阅洪峰场景连续命中检查窗口会
/// 误判失败, 超限后 reset_pending_state 静默放弃订阅 -> 少订)。
/// @param queue_nonempty pending_batches_ 是否有滞留批次
/// @param has_pending 是否存在 Pending 合约
/// @param retry_count 本次检查前已累计的重试次数
/// @param max_retry 最大重试次数 (retry_count + 1 > max_retry 时 GiveUp, 与既有语义一致)
BatchCheckAction decide_batch_check_action(bool queue_nonempty, bool has_pending,
                                           int retry_count, int max_retry);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_MD_BATCH_CHECK_H_
