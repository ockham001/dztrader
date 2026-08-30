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
    std::string order_ref;  ///< CTP OrderRef (12 位补零格式)
    int32_t front_id = 0;   ///< CTP FrontID (OnRtnOrder 回报填充)
    int32_t session_id = 0; ///< CTP SessionID (OnRtnOrder 回报填充)
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

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
