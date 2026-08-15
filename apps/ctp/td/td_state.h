#ifndef DZTRADER_CTP_TD_STATE_H_
#define DZTRADER_CTP_TD_STATE_H_

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/core/json_enum.h>
#include <dztrader/data_type.h>

namespace dztrader::ctp {

/// CTP 交易网关状态机状态 (11 状态)。
/// 参考 mdctp MdState, 扩展为 11 状态以覆盖认证/结算确认/合约加载。
enum class TdState {
    Idle,                ///< 未启动, 初始状态
    Connecting,          ///< API 已 Init, 等待 OnFrontConnected
    Connected,           ///< 前置已连接, 准备认证
    Authenticating,      ///< ReqAuthenticate 已发, 等待 OnRspAuthenticate
    Authenticated,       ///< 认证通过, 准备登录
    LoggingIn,           ///< ReqUserLogin 已发, 等待 OnRspUserLogin
    LoggedIn,            ///< 登录成功, 准备确认结算单
    Confirming,          ///< ReqSettlementInfoConfirm 已发, 等待响应
    LoadingInstruments,  ///< ReqQryInstrument 进行中 (流控重试)
    Ready,               ///< 合约加载完成, 可下单
    Disconnected         ///< 网络断开, CTP 自动重连中
};

/// 基于 magic_enum 自动派生 TdState 的 JSON 序列化, 字符串与日志一致。
DZ_JSON_ENUM(TdState)

/// 交易二元健康度 (给策略/风控进程决策用, 与 TdState 细粒度状态独立)。
/// - Up: 已就绪 (Ready 状态, 可下单)
/// - Down: 任何异常或未就绪 (不可下单)
enum class TdHealth {
    Down,  ///< 不可交易
    Up     ///< 可交易
};

/// 网关状态, 对 UI 和共享内存可见。
/// 参考 mdctp MdStatus, 去掉订阅统计字段 (md 特有), 新增 account_id (per-account 标识)。
struct TdStatus {
    std::string account_id;          ///< 账户标识 (per-account 状态上报用)
    std::string api_version;         ///< CTP API 版本
    std::string sys_version;         ///< 系统版本 (OnRspUserLogin 返回)
    std::string trading_day;         ///< 交易日 "YYYYMMDD"
    std::string login_time;          ///< 登录时间
    TdState state = TdState::Idle;   ///< 当前状态

    // 步骤进度 (UI 可渲染为进度条)
    std::string progress_desc;       ///< 进度描述 (中文)
    int progress_min = 0;            ///< 进度最小值
    int progress_max = 10;           ///< 进度最大值 (td: max=10, md 是 4)
    int progress_current = 0;        ///< 当前进度

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdStatus,
                                                account_id,
                                                api_version,
                                                sys_version,
                                                trading_day,
                                                login_time,
                                                state,
                                                progress_desc,
                                                progress_min,
                                                progress_max,
                                                progress_current)
};

/// 状态转移返回的通知信息。
/// 与 mdctp MdNotification 模式一致, 调用方应通过 notify_ui 发送给前端。
struct TdNotification {
    DzNotifyLevel level = DZ_NOTIFY_INFO;  ///< INFO/WARN/ERROR
    bool popup = false;                     ///< 是否弹窗
    std::string message;                    ///< 中文消息
};

/// CTP 交易网关的状态机。
/// 不依赖 CTP SDK 或共享内存, 可完全独立测试。
/// per-account 实例 (AccountSession 持有), 不持有 account_id (由 AccountSession 管理)。
class TdStateMachine {
public:
    TdStateMachine();

    // --- 状态查询 ---
    TdState state() const { return status_.state; }
    const TdStatus& status() const { return status_; }

    /// 设置 CTP API 版本字符串 (构造时调用一次)。
    void set_api_version(const std::string& version) { status_.api_version = version; }

    // --- 状态转移 ---
    // 每个方法返回可选通知, 调用方应发送给 UI

    /// connect() 请求时调用。仅在 Idle 状态有效。
    std::optional<TdNotification> on_connect();

    /// disconnect() 请求时调用。任意状态均有效。
    std::optional<TdNotification> on_disconnect();

    /// CTP OnFrontConnected 触发时调用。
    std::optional<TdNotification> on_front_connected();

    /// CTP OnFrontDisconnected 触发时调用。
    std::optional<TdNotification> on_front_disconnected(int reason);

    /// 连接超时触发时调用 (定时器回调)。仅在 Connecting 状态有效, 回退到 Idle。
    /// 与 on_front_disconnected 语义不同: 超时意味着从未连上, 不是断线。
    std::optional<TdNotification> on_connect_timeout();

    /// ReqAuthenticate 发送时调用。Connected -> Authenticating。
    std::optional<TdNotification> on_req_authenticate();

    /// 认证成功时调用。
    std::optional<TdNotification> on_authenticate_success();

    /// 认证失败时调用 (ErrorID != 0)。
    std::optional<TdNotification> on_authenticate_failed();

    /// ReqUserLogin 发送时调用。Connected/Authenticated -> LoggingIn。
    std::optional<TdNotification> on_req_login();

    /// 登录成功时调用, 同步 order_ref_ (由 AccountSession 处理, 状态机只更新状态)。
    std::optional<TdNotification> on_login_success(const std::string& sys_version,
                                                    const std::string& trading_day,
                                                    const std::string& login_time);

    /// 登录失败时调用 (ErrorID != 0)。
    std::optional<TdNotification> on_login_failed();

    /// ReqSettlementInfoConfirm 发送时调用。LoggedIn -> Confirming。
    std::optional<TdNotification> on_req_settlement_confirm();

    /// 结算单确认成功时调用。
    std::optional<TdNotification> on_settlement_confirmed();

    /// 结算单确认失败时调用 (ErrorID != 0). 回退到 LoggedIn 等待重试.
    std::optional<TdNotification> on_settlement_confirm_failed(const std::string& err);

    /// 合约加载完成时调用。
    std::optional<TdNotification> on_instruments_loaded();

    /// 合约加载失败时调用 (ErrorID != 0 或流控超时)。
    std::optional<TdNotification> on_instruments_load_failed(const std::string& err);

private:
    void set_state(TdState new_state);

    TdStatus status_;

    /// 标记是否经过认证成功 (用于 on_login_failed 决定回退目标)。
    /// on_authenticate_success 时置 true; on_disconnect 时重置 false。
    bool was_authenticated_ = false;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_STATE_H_
