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

// 复用平台库的前置类型 (契约 md-config), 避免与 md 侧重复定义。
using BrokerFrontend = dztrader::platform::CtpBrokerFrontend;

/// 配置操作类型 (op-based 下发, 替代全量替换)。
/// 状态保护: RemoveAccount/UpdateAccount 在非 Idle 时拒绝 (编排层校验)。
enum class TdConfigOp {
    // 进程级 op
    AddAccount,     // 添加账户 ID + 创建账户配置
    RemoveAccount,  // 移除账户 (仅校验 state==Idle, 不校验持仓)
    // 注: AddSchedule/RemoveSchedule/SetAutoLogin 已随契约 auto-login 迁移移除
    // (排程单一真相源为 SET/RTN_AUTO_LOGIN 帧, 见 td_api_scheduled.cpp)
    SetExt,           // 进程级柜台特有配置的字段级补丁 (契约 td-config §2.1)
    SetQryIntervals,  // qry_account_interval_s / qry_position_interval_s / qry_flush_interval_ms

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
        case TdConfigOp::SetExt:
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

/// CTP 账户级 `ext`（契约 td-config §2.3）：该接口类型的连接参数。
///
/// **为什么不直接复用平台 `CtpBrokerEntry`**（同文件顶部声明「复用平台库避免重复定义」的
/// 口径，此处是有意的例外，两条理由都必须成立才分道）：
/// 1. `CtpBrokerEntry` 自带 `name`（md 侧的经纪商名，md 可配多个经纪商并切换，是它的业务
///    主键）。td 侧没有这个概念——一个 td 账户就是一个账户，账户身份是 `account_id`。强行
///    复用会把 `name` 泄进 td 配置与 `RTN_CONFIG` payload，dzweb 与前端就得忽略一个恒空的
///    字段，将来有人会以为它可以填。
/// 2. **分层是契约要求**：契约 td-config §1 规定「通用层 + 柜台 `ext`」是跨接口类型约定，
///    `ext` 的 schema 由各接口类型自己定义（§2.3）。`ext` 的键集合由契约固定
///    （`broker_id`/`user_id`/`password`/`product_info`/`frontends`/`auth_code`/`app_id`/
///    `flow_dir`），不完全等于 `CtpBrokerEntry` 的字段集合——复用平台类型会让「契约里的
///    ext schema」与「平台 md 类型」两处各自演进却看起来同源。
///
/// 代价（已知且接受）：平台侧给 `CtpBrokerEntry` 加连接字段时本结构不会自动跟随。这与
/// 「md 与 td 是不同接口域、字段集本就不同」一致；`BrokerFrontend` 仍然复用（见顶部
/// `using`），因为前置地址的语义两侧完全相同。
struct CtpAccountExt {
    std::string broker_id;                  // 经纪公司代码
    std::string user_id;                    // 用户代码
    std::string password;                   // 登录密码 (回报中脱敏为 "****")
    std::string product_info;               // 产品信息, 可空
    std::vector<BrokerFrontend> frontends;  // 前置地址列表, 默认空
    std::string auth_code;                  // CTP 认证码, 可空
    std::string app_id;                     // CTP AppID, 可空
    std::string flow_dir;  // 默认空 = 由网关构造 paths::home()/"flow"/<进程名>/<account_id>/

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpAccountExt,
                                                broker_id,
                                                user_id,
                                                password,
                                                product_info,
                                                frontends,
                                                auth_code,
                                                app_id,
                                                flow_dir)
};

/// 账户配置：通用层（契约 td-config §2.2）+ 柜台特有 `ext`（§2.3）。
/// 分层是跨接口类型约定：新增接口类型只换 `ext`，通用层不变（§1 双侧约定）。
struct AccountConfig {
    std::string account_id;  // 不可变 key；全局唯一由平台入口校验（§5），本进程只查自身重复
    bool enabled = true;
    bool risk_control_enabled = false;  // 账户级风控开关 (单层)
    std::string currency_id = "CNY";
    CtpAccountExt ext;  // 该接口类型的连接参数 (interface_type = ctp)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        AccountConfig, account_id, enabled, risk_control_enabled, currency_id, ext)
};

/// CTP 进程级 `ext`（契约 td-config §2.3）：`SetExt` 的补丁目标。
struct CtpProcessExt {
    bool enable_lock_mode = true;  // 开平仓转换 LOCK 模式

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpProcessExt, enable_lock_mode)
};

/// TD 网关配置 (td section)。
/// 排程/自动登录已迁移契约 auto-login（SET/RTN_AUTO_LOGIN 帧，持久化到 auto_login section，
/// 由 AutoLoginConfig 管理）——本结构不再含 schedules/enable_auto_login_logout 字段。
/// **文件形态与帧 payload 同形**（契约 td-config §2.4）：通用层字段 + `ext` 分层，不做双序列化
/// ——同一事实两处定义必然漂移。代价：旧扁平格式（连接参数直接在账户对象上）不再兼容，
/// 开发期无迁移义务（契约 §6：内容非法即致命退出）。
struct TdConfig {
    int qry_account_interval_s = 5;    // 资金查询间隔 (秒)
    int qry_position_interval_s = 60;  // 持仓查询间隔 (秒)
    int qry_flush_interval_ms = 1500;  // 流控队列间隔 (毫秒)
    CtpProcessExt ext;                 // 进程级柜台特有配置 (interface_type = ctp)
    std::vector<AccountConfig> accounts;

    /// 从 JSON 文件的 "td" section 加载 (与 mdctp CtpMdConfig::load 模式一致)。
    /// 契约 td-config §6 两级语义：文件/section 缺失 -> 默认值 + 自愈写回；内容非法 -> 抛异常。
    /// **旧扁平格式不在此抛**：未知字段按总则 §8 忽略、解码成功，由 {@link validate} 拒绝
    /// （调用方对「解码失败」与「校验失败」的处理相同——记 CRITICAL 后致命退出）。
    /// @throws std::runtime_error JSON 解析失败或 section 类型不符
    static TdConfig load(const std::filesystem::path& path, const std::string& section = "td");

    /// 原子写入 JSON 文件的指定 section (load-modify-save, tmp + rename)。
    void save(const std::filesystem::path& path, const std::string& section = "td") const;

    /// 自愈写回默认配置（契约 td-config §6「文件/section 缺失」路径）。
    /// 写失败只记 ERROR、不阻断启动——内存里已有可用默认值。
    static void repair_file(const TdConfig& defaults,
                            const std::filesystem::path& path,
                            const std::string& section = "td");

    /// 序列化为脱敏 JSON (账户 ext.password -> "****", 用于上报到 dzweb/前端)。
    [[nodiscard]] nlohmann::json to_safe_json() const;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdConfig,
                                                qry_account_interval_s,
                                                qry_position_interval_s,
                                                qry_flush_interval_ms,
                                                ext,
                                                accounts)
};

/// 集中校验 (与 mdctp validate 模式一致): 返回错误消息, 校验通过返回 std::nullopt。
std::optional<std::string> validate(const TdConfig& cfg);
}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_CONFIG_H_
