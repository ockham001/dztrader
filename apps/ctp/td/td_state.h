#ifndef DZTRADER_CTP_TD_STATE_H_
#define DZTRADER_CTP_TD_STATE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/core/json_enum.h>
#include <dztrader/data_type.h>

namespace dztrader::ctp {

/// 未知枚举累计计数 (按种类分开): 「我们没跟上柜台」与「柜台给了坏数据」处置不同,
/// 合并成一个总数会让运维无从判断该补映射还是该查数据源 (issue #367 用户故事 8).
/// 语义 = 本会话生命周期内累计条数, 日切/断线不清零 (计数器只增不减, 供持续观察).
/// 放在 td_state.h 而非 AccountSession 里: 状态帧 payload 的拼接口径要与它同处一文件,
/// 才能在不链接 CTP 的目标里单测 (td_api_pure_test)。
struct UnknownEnumCounts {
    uint64_t order_status = 0;     ///< 不认识的柜台委托状态
    uint64_t direction = 0;        ///< 不认识的持仓方向
    uint64_t price_type = 0;       ///< 不认识的委托价格类型
    uint64_t position_effect = 0;  ///< 不认识的开平标志

    [[nodiscard]] uint64_t total() const noexcept {
        return order_status + direction + price_type + position_effect;
    }
};

/// CTP 交易网关状态机状态 (15 状态: 连接/登录链 11 段 + 四个失败态)。
/// 参考 mdctp MdState, 扩展以覆盖认证/结算确认/合约加载。
///
/// 失败态 (ADR 0059 / 契约 td-status §2.2): 登录链上任一段失败后**停在它自己那一段**,
/// `progress.current` 取该段数值、不回退到上一段 —— 失败只回答「在哪一段」, 段内原因由
/// `last_error` 承载。四条可失败段各有一个失败态, 常量为 11 + 4 = 15; 链上再没有
/// 「失败就退回上一段」的路径 (连接超时是唯一例外, 它回 `Idle` 且无失败态)。
enum class TdState {
    Idle,            ///< 未启动, 初始状态
    Connecting,      ///< API 已 Init, 等待 OnFrontConnected
    Connected,       ///< 前置已连接, 准备认证
    Authenticating,  ///< ReqAuthenticate 已发, 等待 OnRspAuthenticate
    AuthFailed,      ///< 认证失败, 停在 3/10 (不再回退 Connected); 原因见 last_error
    Authenticated,   ///< 认证通过, 准备登录
    LoggingIn,       ///< ReqUserLogin 已发, 等待 OnRspUserLogin
    LoginFailed,     ///< 登录失败, 停在 5/10 (不再回退 Authenticated/Connected); 原因见 last_error
    LoggedIn,        ///< 登录成功, 准备确认结算单
    Confirming,      ///< ReqSettlementInfoConfirm 已发, 等待响应
    SettlementConfirmFailed,  ///< 结算单确认失败, 停在 7/10 (不再回退 LoggedIn); 原因见 last_error
    LoadingInstruments,       ///< ReqQryInstrument 进行中 (流控重试)
    InstrumentsLoadFailed,    ///< 合约加载失败, 停在 8/10 (不再回退 LoggedIn); 原因见 last_error
    Ready,                    ///< 合约加载完成, 可下单
    Disconnected              ///< 网络断开, CTP 自动重连中
};

/// 基于 magic_enum 自动派生 TdState 的 JSON 序列化, 字符串与日志一致。
DZ_JSON_ENUM(TdState)

/// 登录链失败原因码 (契约 td-status §2.5 `last_error.code`)。
/// 一个码 = 同一失败阶段里的一种原因; 超时与柜台拒绝同等对待, 靠本枚举区分 (ADR 0059 决策 5)。
/// 12 个码全部可达: 连接超时例外 1 + 认证段 2 + 登录段 5 + 结算确认段 2 + 合约加载段 2。
enum class TdFailureCode {
    ConnectTimeout,       ///< connect_timeout: 30 秒内未收到 OnFrontConnected (回 Idle, 不进失败态)
    AuthRejected,         ///< auth_rejected: OnRspAuthenticate 的 ErrorID != 0 (柜台拒绝认证)
    AuthTimeout,          ///< auth_timeout: 10 秒内未收到认证响应
    LoginRejected,        ///< login_rejected: OnRspUserLogin 的 ErrorID != 0 (柜台拒绝登录)
    LoginTimeout,         ///< login_timeout: 10 秒内未收到登录响应
    LoginParseError,      ///< login_parse_error: 登录响应缺失 / 交易日无效
    DayLoadFailed,        ///< day_load_failed: 当日订单/成交基准装载失败
    LoginSendFailed,      ///< login_send_failed: ReqUserLogin 返回非 0 (发送被拒, 非柜台裁决)
    SettlementRejected,   ///< settlement_rejected: 结算确认被柜台拒绝 / 发送被拒
    SettlementTimeout,    ///< settlement_timeout: 60 秒看门狗未收到结算确认响应
    InstrumentsRejected,  ///< instruments_rejected: 合约查询发送被拒 / OnRspError / is_last 带错误
    InstrumentsTimeout,   ///< instruments_timeout: 合约加载 5 分钟超时
};

/// 失败原因码的线上字符串 (契约 td-status §2.5 码表)。
const char* td_failure_code_str(TdFailureCode code);

/// 连接超时的用户可读原因 (契约 td-status §2.5 的 `connect_timeout` 文案)。
///
/// 单一来源: 状态机写 `last_error.message`、会话拼通知正文都用它——两处各写一份字面量
/// 时, 改口径必漏一处 (用户会看到状态帧与弹窗说两件不同的事)。
inline constexpr std::string_view kConnectTimeoutReason = "30 秒内未连接上前置";

/// 最近一次登录链失败 (契约 td-status §2.5, 全量随 RTN_TD_STATUS 推送)。
/// 与状态机状态分工: 失败态回答「在哪一段」, 本结构回答「这一段里为什么」。
struct TdLastError {
    std::string code;       ///< 机器码 (td_failure_code_str); 前端按它映射固定文案, 不直接展示
    std::string message;    ///< 用户可读原因 (中文); 空串 = 无详情, 由前端按 code 兜底
    int error_id = 0;       ///< CTP ErrorID (仅 *_rejected 有意义), 其余为 0
    int64_t timestamp = 0;  ///< 失败发生时间 (Unix 秒)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdLastError, code, message, error_id, timestamp)
};

/// 交易二元健康度 (给策略/风控进程决策用, 与 TdState 细粒度状态独立)。
/// - Up: 已就绪 (Ready 状态, 可下单)
/// - Down: 任何异常或未就绪 (不可下单)
enum class TdHealth {
    Down,  ///< 不可交易
    Up     ///< 可交易
};

/// CTP 柜台特有状态字段（契约 td-status §2.3）：与通用层分层，新增接口类型只换本结构。
struct CtpStatusExt {
    std::string api_version;  ///< 接口库版本字符串，构造时设置一次
    std::string sys_version;  ///< 登录响应中的系统版本；未登录/进入 Idle 时为空串

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpStatusExt, api_version, sys_version)
};

/// 网关状态, 对 UI 和共享内存可见（契约 td-status §2.1 TdStatus）。
/// 通用层（account_id/state/date/login_time/progress_*/last_error）+ 柜台特有 `ext` 分层：
/// 新增接口类型只换 `ext`，dzweb 与前端按 interface_type 选择 schema。
struct TdStatus {
    std::string account_id;         ///< 账户标识 (per-account 状态上报用)
    std::string date;               ///< 会话交易日 "YYYYMMDD"；未登录/进入 Idle 时为空串
    std::string login_time;         ///< 登录时间；未登录/进入 Idle 时为空串
    TdState state = TdState::Idle;  ///< 当前状态

    // 步骤进度 (UI 可渲染为进度条)
    std::string progress_desc;  ///< 进度描述 (中文)
    int progress_min = 0;       ///< 进度最小值
    int progress_max = 10;      ///< 进度最大值 (td: max=10, md 是 4)
    int progress_current = 0;   ///< 当前进度

    /// 最近一次登录链失败 (契约 td-status §2.5)；null = 无未清空的失败。
    /// 清空时机: 下一次登录尝试被受理 (on_connect) 与转 Ready; 进入 Idle (登出) **不清空**
    /// —— 保留最近一次失败供用户回看 (ADR 0059 决策 6)。
    std::optional<TdLastError> last_error;

    CtpStatusExt ext;  ///< 柜台特有字段 (interface_type = ctp)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TdStatus,
                                                account_id,
                                                date,
                                                login_time,
                                                state,
                                                progress_desc,
                                                progress_min,
                                                progress_max,
                                                progress_current,
                                                last_error,
                                                ext)
};

/// RTN_TD_STATUS 的 payload: 状态机快照 + `ext` 里的未知枚举计数 (契约 td-status §2.3)。
/// 计数挂在 `ext` 下按种类分开 (issue #367 用户故事 8), 不覆盖状态机自身字段。
/// 实现在 td_state.cpp: 与 TdStatus 同处一个不依赖 CTP 头的目标, 便于单测。
nlohmann::json td_status_payload(const TdStatus& status, const UnknownEnumCounts& counts);

/// 状态转移返回的通知信息。
/// 与 mdctp MdNotification 模式一致, 调用方应通过 notify_ui 发送给前端。
struct TdNotification {
    DzNotifyLevel level = DZ_NOTIFY_INFO;  ///< INFO/WARN/ERROR
    bool popup = false;                    ///< 是否弹窗
    std::string message;                   ///< 中文消息
};

/// TdState → 策略可见三态聚合映射 (spec §2.1)。
/// Ready 与 TdHealth::Up 同一判定点; 内部状态演进不得破坏此映射的稳定性。
/// 失败态属「会话未就绪」: 与它停住的那一段同类, 归 LoggingIn (契约 account-status)。
DzAccountState account_state_of(TdState state) noexcept;

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
    void set_api_version(const std::string& version) { status_.ext.api_version = version; }

    // --- 状态转移 ---
    // 每个方法返回可选通知, 调用方应发送给 UI

    /// connect() 请求时调用。仅在 Idle 状态有效；同时清空 last_error
    /// (登录尝试被受理, 契约 td-status §2.5 清空时机)。
    std::optional<TdNotification> on_connect();

    /// disconnect() 请求时调用。任意状态均有效。
    std::optional<TdNotification> on_disconnect();

    /// CTP OnFrontConnected 触发时调用。
    std::optional<TdNotification> on_front_connected();

    /// CTP OnFrontDisconnected 触发时调用。
    std::optional<TdNotification> on_front_disconnected(int reason);

    /// 连接超时触发时调用 (定时器回调)。仅在 Connecting 状态有效, 回退到 Idle。
    /// 与 on_front_disconnected 语义不同: 超时意味着从未连上, 不是断线。
    ///
    /// **例外路径 (ADR 0059 决策 7)**: 连接超时发生在登录链第一段之前, 没有失败阶段可停,
    /// 因此**不进失败态**——仍回 `Idle`(= Offline 三态, 唯一允许直接发起登录的端点)。
    /// 失败原因照写 `last_error` (`code` = connect_timeout), 用户从那里读原因。
    /// 给它失败态会把「从未连上」报成「登录中」, 改变 `ACCOUNT_STATUS` 对策略侧的语义。
    std::optional<TdNotification> on_connect_timeout();

    /// ReqAuthenticate 发送时调用。Connected -> Authenticating。
    std::optional<TdNotification> on_req_authenticate();

    /// 认证成功时调用。
    std::optional<TdNotification> on_authenticate_success();

    /// 认证失败时调用 (柜台拒绝 ErrorID != 0 / 10 秒超时)。
    /// 停在 `AuthFailed` (3/10「认证失败」), **不再回退到 `Connected`** (ADR 0059 决策 1)。
    /// 段内原因写入 `last_error` (契约 td-status §2.5):
    /// @param code 失败原因码 (auth_rejected / auth_timeout)
    /// @param message 用户可读原因 (中文; CTP ErrorMsg 原文或空串 = 无详情)
    /// @param error_id CTP ErrorID (仅 auth_rejected 有意义, 超时传 0)
    std::optional<TdNotification> on_authenticate_failed(TdFailureCode code,
                                                         const std::string& message,
                                                         int error_id = 0);

    /// ReqUserLogin 发送时调用。Connected/Authenticated -> LoggingIn。
    std::optional<TdNotification> on_req_login();

    /// 登录成功时调用, 同步 order_ref_ (由 AccountSession 处理, 状态机只更新状态)。
    std::optional<TdNotification> on_login_success(const std::string& sys_version,
                                                   const std::string& date,
                                                   const std::string& login_time);

    /// 登录失败时调用 (柜台拒绝 ErrorID != 0 / 10 秒超时 / 响应解析失败 / 基准装载失败)。
    /// 停在 `LoginFailed` (5/10「登录失败」), **不再回退到 `Authenticated`(4) / `Connected`(2)**
    /// (ADR 0059 决策 1)。失败位置与「是否经过认证」无关——链走到哪一段就停在哪一段。
    /// 段内原因写入 `last_error` (契约 td-status §2.5):
    /// @param code 失败原因码 (login_rejected / login_timeout / login_parse_error /
    /// day_load_failed / login_send_failed)
    /// @param message 用户可读原因 (中文; CTP ErrorMsg 原文或空串 = 无详情)
    /// @param error_id CTP ErrorID (仅 login_rejected 有意义, 其余传 0)
    std::optional<TdNotification> on_login_failed(TdFailureCode code,
                                                  const std::string& message,
                                                  int error_id = 0);

    /// ReqSettlementInfoConfirm 发送时调用。LoggedIn -> Confirming。
    std::optional<TdNotification> on_req_settlement_confirm();

    /// 结算单确认成功时调用。
    std::optional<TdNotification> on_settlement_confirmed();

    /// 结算单确认失败时调用 (柜台拒绝 ErrorID != 0 / 发送被拒 / 60 秒看门狗超时)。
    /// 停在 `SettlementConfirmFailed` (7/10「结算单确认失败」), **不再回退到 `LoggedIn`(6)**
    /// (ADR 0059 决策 1)。回退后的 `LoggedIn` 是正常中间态, 界面看不出异常 —— 这正是本票
    /// 要消灭的「已登录却下不了单」形态。
    /// 段内原因写入 `last_error` (契约 td-status §2.5):
    /// @param code 失败原因码 (settlement_rejected / settlement_timeout)
    /// @param message 用户可读原因 (中文; CTP ErrorMsg 原文或空串 = 无详情)
    /// @param error_id CTP ErrorID (仅 settlement_rejected 有意义, 超时 / 发送被拒传 0)
    std::optional<TdNotification> on_settlement_confirm_failed(TdFailureCode code,
                                                               const std::string& message,
                                                               int error_id = 0);

    /// 合约加载完成时调用。
    std::optional<TdNotification> on_instruments_loaded();

    /// 合约加载失败时调用 (查询发送被拒 / `OnRspError` 错误响应 / `is_last` 带错误 / 5 分钟超时)。
    /// 停在 `InstrumentsLoadFailed` (8/10「合约加载失败」), **不再回退到 `LoggedIn`(6)**
    /// (ADR 0059 决策 1)。段内原因写入 `last_error` (契约 td-status §2.5):
    /// @param code 失败原因码 (instruments_rejected / instruments_timeout)
    /// @param message 用户可读原因 (中文; 柜台 ErrorMsg 原文或空串 = 无详情)
    /// @param error_id CTP ErrorID (仅 instruments_rejected 的柜台响应有意义, 其余传 0)
    std::optional<TdNotification> on_instruments_load_failed(TdFailureCode code,
                                                             const std::string& message,
                                                             int error_id = 0);

private:
    void set_state(TdState new_state);

    /// 记录最近一次失败 (契约 td-status §2.5, 时间戳取当前 Unix 秒)。
    void set_last_error(TdFailureCode code, const std::string& message, int error_id);

    TdStatus status_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_STATE_H_
