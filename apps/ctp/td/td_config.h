#ifndef DZTRADER_CTP_TD_CONFIG_H_
#define DZTRADER_CTP_TD_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/sched_common.h"

#include <dztrader/core/json_enum.h>
#include <dztrader/platform/ctp_md_config.h>

namespace dztrader::ctp {

// 复用平台库的经纪商/前置类型 (契约 08), 避免与 md 侧重复定义。
// 字段与原 BrokerEntry/BrokerFrontend 完全一致, td 侧代码无需改动。
using BrokerEntry = dztrader::platform::CtpBrokerEntry;
using BrokerFrontend = dztrader::platform::CtpBrokerFrontend;

/// 配置操作类型 (op-based 下发, 替代全量替换)。
/// 状态保护: RemoveAccount/UpdateAccount 在非 Idle 时拒绝 (编排层校验)。
enum class TdConfigOp {
    // 进程级 op
    AddAccount,             // 添加账户 ID + 创建账户配置
    RemoveAccount,          // 移除账户 (仅校验 state==Idle, 不校验持仓)
    // 注: AddSchedule/RemoveSchedule/SetAutoLogin 已随契约 04 迁移移除
    // (排程单一真相源为 SET/RTN_AUTO_LOGIN 帧, 见 td_api_scheduled.cpp)
    SetLockMode,            // OffsetConverter LOCK 模式开关
    SetQryIntervals,        // qry_account_interval_s / qry_position_interval_s / qry_flush_interval_ms

    // 账户级 op (仅影响该账户)
    UpdateAccount,          // 修改连接参数 (需 state==Idle)
    SetAccountEnabled,      // 启用/禁用
    SetAccountRiskControl,  // 账户级风控开关
    SetAccountCurrency,
};

/// 基于 magic_enum 自动派生 TdConfigOp 的 JSON 序列化。
DZ_JSON_ENUM(TdConfigOp)

/// 判断 op 是否涉及连接参数变更 (非 Idle 时拒绝)。
/// switch 覆盖所有枚举值, 新增枚举时编译器 (-Wswitch) 会警告。
inline bool is_connection_op(TdConfigOp op) {
    switch (op) {
        case TdConfigOp::RemoveAccount:
        case TdConfigOp::UpdateAccount:
            return true;
        case TdConfigOp::AddAccount:
        case TdConfigOp::SetLockMode:
        case TdConfigOp::SetQryIntervals:
        case TdConfigOp::SetAccountEnabled:
        case TdConfigOp::SetAccountRiskControl:
        case TdConfigOp::SetAccountCurrency:
            return false;
    }
    return false;  // 兜底
}

/// op-based 配置请求 (DZ_FRAME_TD_REQ_MODIFY_CONFIG 帧 payload)。
struct TdConfigOpReq {
    TdConfigOp op;
    nlohmann::json params;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdConfigOpReq, op, params)
};

/// 账户配置 (td section 的 accounts 数组元素)。
/// 复用平台库的 CtpBrokerEntry (broker_id/user_id/password/product_info/frontends),
/// 新增 td 专用字段 (auth_code/app_id/flow_dir/risk_control_enabled 等)。
struct AccountConfig {
    std::string account_id;          // 不可变 key (与 BrokerEntry.name 区分: account_id 是平台内部标识)
    BrokerEntry broker;              // 复用 mdctp: broker_id/user_id/password/product_info/frontends
    std::string auth_code;           // 可空 (CTP 认证码)
    std::string app_id;              // 可空 (CTP AppID)
    std::string flow_dir;            // 默认空, 由网关自动构造 paths::home()/"flow"/<进程名>/<account_id>/
    bool enabled = true;
    bool risk_control_enabled = false;  // 账户级风控开关 (单层)
    std::string currency_id = "CNY";

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AccountConfig, account_id, broker,
                                                auth_code, app_id, flow_dir, enabled,
                                                risk_control_enabled, currency_id)
};

/// TD 网关配置 (td section)。
/// 排程/自动登录已迁移契约 04（SET/RTN_AUTO_LOGIN 帧，持久化到 auto_login section，
/// 由 AutoLoginConfig 管理）——本结构不再含 schedules/enable_auto_login_logout 字段。
struct TdConfig {
    int qry_account_interval_s = 5;          // 资金查询间隔 (秒)
    int qry_position_interval_s = 5;         // 持仓查询间隔 (秒)
    int qry_flush_interval_ms = 1500;        // 流控队列间隔 (毫秒)
    bool enable_lock_mode = true;            // OffsetConverter LOCK 模式
    std::vector<AccountConfig> accounts;     // 账户数组

    /// 从 JSON 文件的 "td" section 加载 (与 mdctp CtpMdConfig::load 模式一致)。
    static TdConfig load(const std::filesystem::path& path, const std::string& section = "td");

    /// 原子写入 JSON 文件的指定 section (load-modify-save, tmp + rename)。
    void save(const std::filesystem::path& path, const std::string& section = "td") const;

    /// 序列化为脱敏 JSON (password -> "****", 用于上报到 dzweb/前端)。
    [[nodiscard]] nlohmann::json to_safe_json() const;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdConfig,
                                                qry_account_interval_s, qry_position_interval_s,
                                                qry_flush_interval_ms, enable_lock_mode, accounts)
};

/// 集中校验 (与 mdctp validate 模式一致): 返回错误消息, 校验通过返回 std::nullopt。
std::optional<std::string> validate(const TdConfig& cfg);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_CONFIG_H_
