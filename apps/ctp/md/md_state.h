#ifndef DZTRADER_CTP_MD_STATE_H_
#define DZTRADER_CTP_MD_STATE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/core/json_enum.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/data_type.h>
#include <dztrader/platform/subscription_manager.h>  // SubState, SubscriptionDetail, etc.

namespace dztrader::ctp {
// 类型已迁移到 platform, 保留命名空间内可见性
using SubState = dztrader::platform::SubState;
using SubscriptionDetail = dztrader::platform::SubscriptionDetail;
using RtnMdSubscriptionsRsp = dztrader::platform::RtnMdSubscriptionsRsp;
}  // namespace dztrader::ctp

namespace dztrader::ctp {

/// CTP 行情网关状态机状态。
enum class MdState {
    Idle,         ///< 无 API 实例, 初始状态或手动断开后
    Connecting,   ///< API 已创建, Init() 已调用, 等待 OnFrontConnected
    Connected,    ///< 前置已连接, 尚未登录
    LoggingIn,    ///< ReqUserLogin 已发送, 等待响应
    LoggedIn,     ///< 登录成功, 合约已订阅/订阅中
    Disconnected  ///< 网络断开, CTP 自动重连中 (API 实例仍存活)
};

/// 基于 magic_enum 自动派生 MdState 的 JSON 序列化, 字符串与日志一致。
DZ_JSON_ENUM(MdState)

/// 行情二元健康度 (给策略/数据存储进程决策用, 与 MdState 细粒度状态独立)。
/// - Up: 已登录 (可交易)
/// - Down: 任何异常或未登录 (不可交易)
enum class MdHealth {
    Down,  ///< 不可交易
    Up     ///< 可交易
};

/// 网关状态, 对 UI 和共享内存可见。
struct MdStatus {
    std::string api_version;
    std::string sys_version;
    std::string trading_day;
    std::string login_time;
    MdState state = MdState::Idle;

    // 步骤进度 (UI 可渲染为进度条)
    std::string progress_desc;
    int progress_min = 0;
    int progress_max = 0;
    int progress_current = 0;

    // 订阅统计
    size_t expected_subscribe_count = 0;
    size_t subscribed_count = 0;

    // NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 已删除:
    // RTN_MD_STATUS payload 由 build_md_status_payload() 构造（仅契约 md-status 的 6 字段）
    // RTN_PROGRESS payload 由 report_progress() 从 progress_* 字段读取
};

/// 将 CTP 断线原因码转为可读字符串。
std::string disconnect_reason_str(int reason);

/// 解析交易日字符串 "YYYYMMDD" 为自 epoch 以来的天数。
/// @throws std::runtime_error 格式非法时抛出
int32_t trading_day_to_days(const char* s);

/// CTP 行情网关的状态机。
/// 不依赖 CTP SDK 或共享内存, 可完全独立测试。
class MdStateMachine {
public:
    MdStateMachine();

    // --- 状态查询 ---
    MdState state() const { return status_.state; }
    const MdStatus& status() const { return status_; }

    /// 同步订阅统计（由 MdApi::report_md_status 调用，数据源为 SubscriptionManager）。
    void set_subscription_stats(size_t expected, size_t subscribed);

    /// 设置 CTP API 版本字符串 (构造时调用一次)。
    void set_api_version(const std::string& version) { status_.api_version = version; }

    // --- 状态转移 ---
    // 每个方法返回可选通知, 调用方应发送给 UI

    /// connect() 请求时调用。仅在 Idle 状态有效。
    std::optional<nlohmann::json> on_connect();

    /// disconnect() 请求时调用。任意状态均有效。
    std::optional<nlohmann::json> on_disconnect();

    /// CTP OnFrontConnected 触发时调用。
    std::optional<nlohmann::json> on_front_connected();

    /// CTP OnFrontDisconnected 触发时调用。
    std::optional<nlohmann::json> on_front_disconnected(int reason);

    /// ReqUserLogin 发送时调用。Connected -> LoggingIn。
    std::optional<nlohmann::json> on_req_login();

    /// 登录成功时调用。
    std::optional<nlohmann::json> on_login_success(const std::string& sys_version,
                                                   const std::string& trading_day,
                                                   const std::string& login_time);

    /// 登录失败时调用 (ErrorID != 0)。
    std::optional<nlohmann::json> on_login_failed();

    /// 登录响应中交易日解析错误时调用。
    std::optional<nlohmann::json> on_login_parse_error();

    /// 服务器主动登出时调用。
    std::optional<nlohmann::json> on_server_logout();

private:
    void set_state(MdState new_state);

    MdStatus status_;
};

/// 构造 RTN_MD_STATUS payload JSON（契约 md-status 的 6 字段，始终全量）。
/// 纯函数，不依赖 CTP SDK / SHM，可单元测试。
nlohmann::json build_md_status_payload(const MdStatus& s);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_MD_STATE_H_
