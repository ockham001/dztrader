#ifndef DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
#define DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include <dztrader/data_type.h>  // DzOrderId

namespace dztrader::ctp {

/// 撤单上下文 (C4: cancel_order 反向查找所需).
/// 主线程在 place_order 成功后 insert, on_rtn_order 收到 CTP 回报时 update front_id/session_id.
struct CancelContext {
    std::string order_ref;   ///< CTP OrderRef (12 位补零格式)
    int32_t front_id = 0;    ///< CTP FrontID (OnRtnOrder 回报填充)
    int32_t session_id = 0;  ///< CTP SessionID (OnRtnOrder 回报填充)
};

/// order_ref / OrderSysID -> DzOrderId 映射表 (设计 §9.3)
///
/// 线程模型: 仅主线程访问, 无锁
/// 生命周期: 进程生命周期内累积, 不清空 (设计 §9.4 RESTART 重传去重)
///
/// order_ref 归一化: 内部统一以 12 位补零格式存储 (与 CTP OrderRef 一致),
/// 调用方传入 "1" 或 "000000000001" 均能匹配, 避免格式不一致导致的查表失败.
class OrderRefMap {
public:
    /// 按 order_ref 查 DzOrderId, 未找到返回 nullptr
    const DzOrderId* find_by_order_ref(const std::string& order_ref) const;
    /// 按 OrderSysID 查 DzOrderId, 未找到返回 nullptr
    const DzOrderId* find_by_sys_id(const std::string& sys_id) const;
    /// 按 DzOrderId 查撤单上下文 (C4), 未找到返回 nullptr
    const CancelContext* find_cancel_context(DzOrderId order_id) const;

    void insert_by_order_ref(const std::string& order_ref, DzOrderId order_id);
    void insert_by_sys_id(const std::string& sys_id, DzOrderId order_id);
    void erase_by_order_ref(const std::string& order_ref);

    /// C4: 登记 DzOrderId -> CancelContext (place_order 成功后调用)
    void insert_cancel_context(DzOrderId order_id, const CancelContext& ctx);
    /// C4: 更新已有 CancelContext 的 front_id/session_id (on_rtn_order 收到回报时调用)
    /// 仅在 ctx 存在且新值非 0 时更新, 避免覆盖有效值.
    void update_cancel_context(DzOrderId order_id, int32_t front_id, int32_t session_id);

    /// 登记 DzOrderId -> 裸策略名 (place_order 时记录, 回报回填 strategy_id 用)
    void insert_strategy(DzOrderId order_id, const std::string& strategy_id);
    /// 按 DzOrderId 查策略名, 未找到 (外部单) 返回 nullptr
    const std::string* find_strategy(DzOrderId order_id) const;
    void erase_strategy(DzOrderId order_id);

    size_t size() const noexcept { return ref_to_id_.size(); }
    void clear() noexcept;

private:
    /// 将 order_ref 归一化为 12 位补零格式 (与 CTP TThostFtdcOrderRefType 一致).
    /// 非数字字符串原样返回 (外部订单可能传入非数字 OrderRef, 不强制归一化).
    static std::string normalize_order_ref(const std::string& order_ref);

    std::unordered_map<std::string, DzOrderId> ref_to_id_;
    std::unordered_map<std::string, DzOrderId> sys_to_id_;
    /// C4: DzOrderId -> CancelContext (撤单反向查找)
    std::unordered_map<DzOrderId, CancelContext> id_to_ctx_;
    /// DzOrderId -> 裸策略名 (回报回填 strategy_id; 外部单无条目)
    std::unordered_map<DzOrderId, std::string> id_to_strategy_;
};

/// order_ref 同步公式 (设计 §9.4): new = max(current+1, ctp_max+1)
/// 解决重连后 CTP 端 MaxOrderRef 大于本地计数器的问题.
int64_t sync_order_ref(int64_t current_ref, int64_t ctp_max) noexcept;

/// 解析 CTP MaxOrderRef 字符串为 int64. 非数字/空返回 0.
int64_t parse_max_order_ref(const char* max_order_ref) noexcept;

/// 未知枚举「通知 + 强制重查」的限频判定 (issue #367 上报口径).
/// 未知枚举可能每条回报都带: 逐条通知会刷爆前端通知环, 逐条强制重查会打满柜台流控.
/// @param now_s 当前单调时钟秒
/// @param last_report_s 上次上报时刻 (<=0 = 尚未上报过)
/// @param interval_s 最小间隔秒
/// @return true = 本次应当通知并触发一次资金/持仓重查
bool unknown_enum_report_due(int64_t now_s, int64_t last_report_s, int64_t interval_s) noexcept;

/// 数据补查重试链的退避间隔 (秒, issue #367 增量复核 B1).
/// 补查失败必须自带重试: 进程级 60s 补查定时器只在「仍有未完整账户」时自续, 一旦停过,
/// 运行期的 force 重查失败就再没有别人来救 —— `data_query_ok_` 恒 false 会永久关闭
/// 周期持仓轮询 (无任何日志). 退避初值 60s (与既有补查同量级), 逐次翻倍, 封顶 5 分钟
/// (与未知枚举上报限频同量级), 避免柜台持续异常时刷日志与刷查询.
/// @param attempt 已失败次数 (从 1 开始; <=1 视为首次)
/// @return 下次重试前应等待的秒数
int64_t resync_retry_delay_s(int attempt) noexcept;

/// 补查重试定时器到期时的三选一 (issue #367 增量复核 B1, 纯判定便于断言).
enum class ResyncRetryAction : uint8_t {
    kResync,  ///< 无冲突且数据仍不完整: 发起补查
    kWait,    ///< 已有查询链在途: 本轮不并发, 继续按退避等待
    kStop,    ///< 未就绪或数据已完整: 收链 (不再排定)
};

/// 补查重试定时器到期的处置判定.
/// @param is_ready 会话是否 Ready (非 Ready 时补查 no-op, 继续排队只是空转)
/// @param data_query_ok 数据是否已完整 (期间可能已被别的路径补全)
/// @param query_chain_busy 是否有查询链在途 (position_query_in_flight_ 或收尾状态机未到 kDone)
/// @return 见 ResyncRetryAction. query_chain_busy 优先于 kResync —— CTP 在途查询流控只有
///   1 笔, 并发双查询链会污染累加器与冻结对账.
ResyncRetryAction decide_resync_retry(bool is_ready,
                                      bool data_query_ok,
                                      bool query_chain_busy) noexcept;

/// 登录响应的会话交易日是否可用 (纯判定便于断言).
///
/// 会话交易日是本会话全部业务数据的时间锚 (GLOSSARY「会话交易日」): 订单/成交/持仓/资金
/// 的 date、ACCOUNT_STATUS 帧的 date、日切判定都取它。它缺失时若继续登录, 数据会盖上无效
/// 日期、日切判定误判 —— 与 md 的 LoginParseError 同口径: 判为登录失败, 不进 LoggedIn。
/// @param days_since_epoch OnRspTdUserLoginField::days_since_epoch (SPI 线程解析结果)
/// @return true = 有效交易日 (正数 DzDate); false = 解析失败或哨兵, 调用方应按登录失败处置。
///   注意排除的不只是 INT32_MIN 哨兵: 0 (DZ_DATE_NA, 与 "19700101" 的解析结果重合)
///   与全部负数同样判不可用, 因此实现是 `> 0` 而非仅比哨兵。
bool trading_day_usable(int32_t days_since_epoch) noexcept;

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
