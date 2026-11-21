#ifndef DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
#define DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_

#include <chrono>
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

/// 未知枚举「通知」的限频判定 (issue #367 上报口径).
/// 未知枚举可能每条回报都带: 逐条通知会刷爆前端通知环.
/// @param now_s 当前单调时钟秒
/// @param last_report_s 上次上报时刻 (<=0 = 尚未上报过)
/// @param interval_s 最小间隔秒
/// @return true = 本次应当通知
bool unknown_enum_report_due(int64_t now_s, int64_t last_report_s, int64_t interval_s) noexcept;

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

/// 登录失败通知正文 (纯构造便于断言, #407).
///
/// 为什么需要场景说明而不是直接转发状态机的通用文案: 登录失败后状态回滚到
/// Authenticated/Connected —— 两者都是**正常中间态**, 前端无法与「正在登录」区分,
/// 而本进程不会自动重发登录 (#406)。所以通知必须自己说清「发生了什么」, 只说
/// 「交易登录失败」等于让用户拿不到任何可用信息。
///
/// 正文**不承诺**「请登出后重新登录」: WebUI 目前没有 td 的连接/断开入口
/// (见 #409), 写了也是让用户去做界面上做不到的事。恢复路径由运维侧处置
/// (等自动调度窗口 / 重启网关), 登记在 docs/components/dztd_ctp.md。
/// @param reason 失败场景的中文说明 (如「交易日无效（原始值 "00000000"）」)
/// @return 形如「交易登录失败（交易日无效（原始值 "00000000"）），本进程不会自动重试该账户」
std::string login_failed_message(const std::string& reason);

// ============================================================================
// 基准持仓查询的有限重试策略 (ADR 0049 决策 3)
// ============================================================================
// 持仓基准是登录成功的前置 (没有基准就没有账): 单次查询失败先按本策略重试,
// 预算用尽即判本次登录失败 —— 停在当前进度、不转 Ready, 由用户处理.
// 只在登录 / 重连链上生效, 与已删除的周期查询/补查链无关.

/// 基准持仓查询最多重试次数 (首次尝试之外; 总尝试次数 = 1 + 本值).
inline constexpr int kPositionQueryMaxRetries = 3;

/// 基准持仓查询重试间隔 (ADR 0049 决策 3: 3 秒; 沿用旧版 DzTrader3 的节奏, 见 #413).
inline constexpr std::chrono::milliseconds kPositionQueryRetryInterval{3000};

/// 基准持仓查询本次失败后是否还应再重试一次.
/// @param retries_done 已重试次数 (0 = 首次尝试刚失败, 尚未重试过)
/// @return true = 还可用剩余重试预算; false = 预算已用尽, 调用方应判登录失败
bool position_query_retry_allowed(int retries_done) noexcept;

/// 基准持仓查询重试耗尽的通知正文 (纯构造便于断言).
///
/// 只陈述事实 (「查询失败 + 账户未就绪 + 已重试几次」), **不承诺** UI 上不存在的操作:
/// WebUI 目前没有 td 的连接/断开入口 (见 #409), 写「请登出后重新登录」等于让用户
/// 去做一件界面上做不到的事 (与 login_failed_message 同一口径).
/// @return 固定文案「持仓查询失败，账户未就绪（已重试 3 次）」(N = kPositionQueryMaxRetries)
std::string position_query_failed_message();

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_ACCOUNT_SESSION_PURE_H_
