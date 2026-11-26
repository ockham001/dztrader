#ifndef DZTRADER_CTP_TD_API_PURE_H_
#define DZTRADER_CTP_TD_API_PURE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/data_type.h>  // DzAccountState

#include "td/td_config.h"
#include "td/td_state.h"  // TdState (会话拆除判据)

namespace dztrader::ctp {

// ============================================================================
// 纯函数声明 (实现在 td_api_pure.cpp, 供单元测试直接编译, 不依赖 TdApi 类和 CTP 头)
// 与 md_api.h 声明 apply_config_op / find_current_broker_in 的模式一致。
//
// 声明单源：本头是这些纯函数的**唯一声明处**——`td_api.h`（CTP 协调层）include 本头，
// `td_api_pure.cpp` 与各 `td_*_test.cpp` 直接 include 本头，不再各自本地重声明
// （本地重声明曾导致多处手工同步，漏改即编译失败，见 #465）。
// ============================================================================

/// 纯粹的 op 应用: switch(req.op) 修改 cfg, 无副作用无 I/O。
/// @throws std::invalid_argument 若 op 非法或参数越界 (如重复 AddAccount / UpdateAccount 不存在)
void apply_config_op(TdConfig& cfg, const TdConfigOpReq& req);

/// 在 accounts 中查找 account_id 对应的账户 (纯函数)。
/// account_id 为空或未找到时返回 nullptr。
const AccountConfig* find_current_account_in(const std::vector<AccountConfig>& accounts,
                                             const std::string& account_id);

/// 在 cfg.accounts 中查找 account_id 对应的账户 (纯函数, 便捷重载)。
/// account_id 为空或未找到时返回 nullptr。委托给 find_current_account_in(cfg.accounts, ...)。
const AccountConfig* find_account_in(const TdConfig& cfg, const std::string& account_id);

/// 会话是否已终止、应被主循环拆除 (#495 / ADR 0059 决策 7)。
///
/// 连接超时是唯一「会话对象还在、状态机已回 `Idle`」的组合：不拆掉它，下一次 `TD_CONNECT`
/// 会被 `connect_account_by_id` 的幂等守卫（`find_session != nullptr` → skip）静默跳过，
/// 而 `Idle` 下前端又禁用登出 —— 账户既登录不了也登不出。判据抽在本层是为了让「不做就死锁」
/// 这条不可逆决策有可失败的单元测试（`td_api.cpp` 不参与任何测试目标）。
///
/// @param state 会话状态机的当前态
/// @return true = 已终止（`Idle`），主循环 sweep 应推终态并 erase
bool session_should_be_reaped(TdState state) noexcept;

/// 2115 查询路由: queried 为空串 = 查全部 (true); 否则命中配置才应答 (不在配置不回, master 兜底)
bool account_query_matches(const std::vector<std::string>& configured, const std::string& queried);

/// 2018 三态去重判定 (契约 account-status 场景 2/3): 非强制时, 同态同交易日且非 Offline 不推;
/// 同态但 `date` 变化 (日切) 必推; Offline 恒推 (删账户/崩溃兜底语义)。
/// `last_date`/`next_date` 是帧内交易日 (已由 account_status_date 归一化: 非 Ready 恒 0)。
/// force (快照/查询应答) 绕过去重。
bool should_push_account_status(
    DzAccountState last, int32_t last_date, DzAccountState next, int32_t next_date, bool force);

/// "YYYYMMDD" -> 距纪元天数; 空串/非法回落 0 (契约: 非 Ready 或缺失交易日 = 0)。
/// Date::from_string 抛异常在此消化。
int32_t epoch_day_from_yyyymmdd(const std::string& yyyymmdd);

/// 契约 account-status: 帧内 `date` 仅 Ready 时为登录返回交易日, 其余状态恒 0。
/// 非法/缺失交易日回落 0。
int32_t account_status_date(DzAccountState state, const std::string& date);

/// DZ_FRAME_TD_REFRESH_REQ payload（契约 td-refresh）：调用方追踪字段 request_id 原样记录。
struct TdRefreshReq {
    std::string account_id;  ///< 目标账户（非空）
    int64_t request_id = 0;  ///< 调用方追踪 ID（可选，缺失 = 0；非整数视为非法帧）
};

/// 解析并校验 TD_REFRESH_REQ JSON payload（契约 td-refresh §校验）。
/// 成功返回 true；失败返回 false 并写 `error` 原因（invalid_json / payload_not_object /
/// invalid_account_id / invalid_request_id），调用方整帧忽略并记 WARN。
bool parse_td_refresh_req(std::string_view payload, TdRefreshReq* out, std::string* error);

/// 账户上次推送的状态缓存 (三态去重口径: 状态 + 帧内交易日)。
/// 同态但交易日变化 (日切) 必须重推 (契约场景 3), 故缓存不能只存状态。
struct AccountStatusCache {
    DzAccountState state = DZ_ACCOUNT_OFFLINE;  ///< 上次推送的三态
    int32_t date = 0;                           ///< 上次推送的帧内交易日 (account_status_date)
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_API_PURE_H_
