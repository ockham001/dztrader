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
    LoginFailed,  ///< 登录被拒/超时/解析失败, 前置仍连接, 可直接重试登录
    Disconnected  ///< 网络断开, CTP 自动重连中 (API 实例仍存活)
};

/// 基于 magic_enum 自动派生 MdState 的 JSON 序列化, 字符串与日志一致。
DZ_JSON_ENUM(MdState)

/// 登录/连接失败原因码（契约 md-status 失败语义 `last_error.code`）。
enum class MdFailureCode {
    ConnectTimeout,   ///< connect_timeout: 30 秒内未连上前置
    LoginRejected,    ///< login_rejected: CTP 拒绝登录 (ErrorID != 0)
    LoginTimeout,     ///< login_timeout: 10 秒内未收到登录响应
    LoginParseError,  ///< login_parse_error: 登录响应缺失/解析失败
};

/// 失败原因码的线上字符串（契约 md-status 失败语义）。
const char* md_failure_code_str(MdFailureCode code);

/// 最近一次登录/连接失败（契约 md-status `last_error`，全量随 RTN_MD_STATUS 推送）。
struct MdLastError {
    std::string code;       ///< 机器码 (md_failure_code_str)
    std::string message;    ///< 用户可读原因 (中文); 空串 = 无详情
    int error_id = 0;       ///< CTP ErrorID (仅 login_rejected)
    int64_t timestamp = 0;  ///< Unix 秒级时间戳
};

/// 网关状态, 对 UI 和共享内存可见。
struct MdStatus {
    std::string api_version;
    std::string sys_version;
    std::string trading_day;
    std::string login_time;
    MdState state = MdState::Idle;

    // 最近一次登录/连接失败（契约 md-status 失败语义）；登录成功/新尝试开始时清空
    std::optional<MdLastError> last_error;

    // 步骤进度 (UI 可渲染为进度条)
    std::string progress_desc;
    int progress_min = 0;
    int progress_max = 0;
    int progress_current = 0;

    // 订阅统计
    size_t expected_subscribe_count = 0;
    size_t subscribed_count = 0;

    // NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 已删除:
    // RTN_MD_STATUS payload 由 build_md_status_payload() 构造（仅契约 md-status 字段）
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

    /// connect() 请求时调用。仅在 Idle 状态有效；清空 last_error（新尝试开始，契约 md-status）。
    std::optional<nlohmann::json> on_connect();

    /// 连接前置超时时调用（30s 未收到 OnFrontConnected）。仅 Connecting 状态有效：
    /// 记录 last_error 并回 Idle（契约 md-status 失败语义）。
    std::optional<nlohmann::json> on_connect_timeout();

    /// disconnect() 请求时调用。任意状态均有效。
    std::optional<nlohmann::json> on_disconnect();

    /// CTP OnFrontConnected 触发时调用。
    std::optional<nlohmann::json> on_front_connected();

    /// CTP OnFrontDisconnected 触发时调用。
    std::optional<nlohmann::json> on_front_disconnected(int reason);

    /// ReqUserLogin 发送时调用。Connected/LoginFailed -> LoggingIn，清空 last_error。
    std::optional<nlohmann::json> on_req_login();

    /// 登录成功时调用。清空 last_error（契约 md-status 失败语义）。
    std::optional<nlohmann::json> on_login_success(const std::string& sys_version,
                                                   const std::string& trading_day,
                                                   const std::string& login_time);

    /// 登录失败时调用 (LoggingIn 状态)。记录 last_error 并转 LoginFailed（契约 md-status）。
    /// @param code 失败原因码
    /// @param message 用户可读原因（CTP ErrorMsg 原文或固定文案；空串时用错误码兜底）
    /// @param error_id CTP ErrorID（仅 LoginRejected 有效，其余传 0）
    std::optional<nlohmann::json> on_login_failed(MdFailureCode code,
                                                  const std::string& message,
                                                  int error_id = 0);

    /// 登录响应中交易日解析错误时调用。
    std::optional<nlohmann::json> on_login_parse_error();

    /// 服务器主动登出时调用。
    std::optional<nlohmann::json> on_server_logout();

private:
    void set_state(MdState new_state);
    /// 记录最近一次失败（契约 md-status last_error，时间戳取当前 Unix 秒）。
    void set_last_error(MdFailureCode code, const std::string& message, int error_id);

    MdStatus status_;
};

/// 构造 RTN_MD_STATUS payload JSON（契约 md-status 全量字段，含 last_error）。
/// 纯函数，不依赖 CTP SDK / SHM，可单元测试。
nlohmann::json build_md_status_payload(const MdStatus& s);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_MD_STATE_H_
