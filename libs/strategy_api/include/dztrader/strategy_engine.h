/**
 * @file strategy_engine.h
 * @brief C++20 模板策略引擎 - 以回调约定接入任意策略类型
 *
 * 核心入口 dztrader::run_strategy(strategy): 封装 dz_init -> 主循环 -> dz_release
 * 的完整进程生命周期, 策略类型只须满足 dztrader::Strategy concept。
 *
 * 回调协议:
 *   - 必选: on_start(DzContext*) -- concept 硬约束, 拼写错误在接入处编译报错
 *   - 可选: on_tick(const DzTick&) 及其余回调, 见 strategy_base.h:
 *     on_stop(), on_trade_report, on_order_report, on_position_info,
 *     on_trading_account, on_account_status, on_schedule, on_ui_input,
 *     on_error(DzErrorCode, std::string_view) -- 编译期探测, 实现了才调用
 *   - 无 on_tick 的策略不接收行情 (引擎仍会排空行情通道以推进 reader 位置,
 *     帧被丢弃), 因此不要订阅不需要的行情合约
 *   - SDK 拦截的事件帧 (TD_RISK_REJECT 与 TD_TRANSFER / TD_PASSWORD_UPDATE 系列等)
 *     与平台内部帧不会到达引擎; 非 TICK 行情帧被静默忽略
 *
 * 异常语义:
 *   - on_start 抛异常 = 生命周期失败: 引擎报告错误、释放会话、返回
 *     DZ_EC_INTERNAL 退出 -- 半初始化策略不允许进入主循环响应行情
 *   - 其余回调抛异常: 引擎捕获后经 report_error 报告 (实现 on_error 则
 *     转发, 否则 stderr 诊断行), 进程继续运行; 消息带回调名
 *   - on_error 可能在 on_start 之前被调用 (dz_init 失败时, 用于报告初始化错误)
 *   - DZ_FRAME_SHUTDOWN 先置停止标志再调 on_stop, on_stop 为
 *     进程内最后一个用户回调; on_stop 抛异常只走 dz_diag (不触发 on_error),
 *     不影响退出
 *
 * 自主退出 (可选):
 *   - 实现 bool should_exit() 返回 true 即请求优雅退出; 引擎在 on_start 后与
 *     每批事件泵 (64/8 上限) 后探测, 命中后调 on_stop、释放会话、返回退出码;
 *     双通道持续饱和时也不会被饿死
 *   - 可选 int32_t exit_code() 指定进程退出码 (默认 0, 建议 0-127;
 *     只接受精确 int32_t 返回值, 不做隐式收窄); 退出时 stdout 输出一行
 *     strategy exit | reason=self code=N
 *   - 后台线程请求退出: 置用户侧 atomic 标志 + dz_notify_self 唤醒引擎等待
 *   - master 语义: 0=Stopped 不重启; 非 0=Crashed (策略类默认 restart 关闭)
 *
 * 线程模型 (api.h 句柄契约: SDK 非线程安全):
 *   - 全部回调运行于唯一事件循环线程; 除 dz_notify_self 外, 任何 dz_* C API
 *     只能在回调内 (即引擎线程) 调用, 用户自建线程须在进程退出前 join
 *   - 回调参数是 SHM/SDK 缓冲的引用, 仅在本次回调返回前有效, 需留存请拷贝
 *
 * 自检: 启动时向 stdout 输出一行回调探测清单 (stop/tick/trade/order/position/
 * account/account_status/schedule/input/error/self_exit/exit_code 的 yes/no),
 * 将"回调名拼错导致永不触发"的静默失败暴露在首次运行。
 * 注意: 继承 StrategyBase 的策略各项恒为 yes (基类提供全部空实现),
 * 拼写错误的 override 由编译器 override 检查兜底。
 *
 * 典型用法:
 * @code
 *   struct MyStrategy : dztrader::StrategyBase {   // 继承仅为省写空回调
 *       void on_start(DzContext* ctx) override { (void)ctx; }  // 订阅、下单
 *       void on_tick(const DzTick& tick) override { (void)tick; }  // 逻辑
 *   };
 *   int main() {
 *       MyStrategy s;
 *       return dztrader::run_strategy(s);
 *   }
 * @endcode
 * 也可不继承 StrategyBase, 任意类型只要提供所需回调即可。
 */
#ifndef DZTRADER_STRATEGY_ENGINE_H_
#define DZTRADER_STRATEGY_ENGINE_H_

#include <dztrader/api.h>
#include <dztrader/data_type.h>
#include <dztrader/error.h>
#include <dztrader/strategy_base.h>
#include <dztrader/struct.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
namespace dztrader::strategy_engine_internal {

/* ── 引擎内部: 模板实现细节, 策略用户勿直接引用 ──
   前置于公开入口: run_strategy 内的限定名在模板定义点须可见。 */

/* ── 可选回调探测 concept ── */

template <typename T>
concept HasOnStop = requires(T t) { t.on_stop(); };

template <typename T>
concept HasOnTick = requires(T t, const DzTick& tick) { t.on_tick(tick); };

template <typename T>
concept HasShouldExit = requires(T t) {
    { t.should_exit() } -> std::convertible_to<bool>;
};

/// 退出码必须精确是 int32_t: 拒绝 bool/double/uint64 等可转换类型,
/// 避免静默收窄与 stdout/进程退出码不一致。
template <typename T>
concept HasExitCode = requires(T t) {
    t.exit_code();
} && std::same_as<std::remove_cvref_t<decltype(std::declval<T&>().exit_code())>, int32_t>;

template <typename T>
concept HasOnTradeReport = requires(T t, const DzTradeReport& rpt) { t.on_trade_report(rpt); };

template <typename T>
concept HasOnOrderReport = requires(T t, const DzOrderReport& rpt) { t.on_order_report(rpt); };

template <typename T>
concept HasOnPositionInfo = requires(T t, const DzPositionInfo& info) { t.on_position_info(info); };

template <typename T>
concept HasOnTradingAccount =
    requires(T t, const DzTradingAccount& account) { t.on_trading_account(account); };

template <typename T>
concept HasOnSchedule = requires(T t, const DzScheduleEvent& ev) { t.on_schedule(ev); };

template <typename T>
concept HasOnUiInput = requires(T t, std::string_view data) { t.on_ui_input(data); };

template <typename T>
concept HasOnError =
    requires(T t, DzErrorCode code, std::string_view msg) { t.on_error(code, msg); };

template <typename T>
concept HasOnAccountStatus =
    requires(T t, const DzAccountStatus& status) { t.on_account_status(status); };

/* ── 异常报告: 分发层捕获 → report_error 转发/兜底 → dz_diag (导出诊断输出) 终端。
      report_error noexcept: 用户 on_error 再抛异常也在此消化,
      异常不逃逸出引擎。 ── */

template <typename StrategyT>
void report_error(StrategyT& strategy, DzErrorCode errcode, std::string_view message) noexcept {
    if constexpr (HasOnError<StrategyT>) {
        try {
            strategy.on_error(errcode, message);
        } catch (const std::exception& e) {
            dz_diag(std::format("on_error threw | reason=\"{}\" original=\"{}\"", e.what(), message)
                        .c_str());
        } catch (...) {
            dz_diag(
                std::format("on_error threw unknown exception | original=\"{}\"", message).c_str());
        }
    } else {
        dz_diag(
            std::format("callback exception | code={} message=\"{}\"", errcode, message).c_str());
    }
}

template <typename StrategyT>
void report_error(StrategyT& strategy, DzErrorCode errcode, const char* message) noexcept {
    if (message == nullptr) [[unlikely]] {
        report_error(strategy, errcode, std::string_view{""});
        return;
    }
    report_error(strategy, errcode, std::string_view{message});
}

/* ── 帧解析: 帧指针 → 头 / payload 引用 (SHM 帧布局: DzFrameHeader + payload) ── */

inline const DzFrameHeader& frame_header(const std::byte* frame) noexcept {
    return *std::launder(reinterpret_cast<const DzFrameHeader*>(frame));
}

template <typename PayloadT>
const PayloadT& frame_payload(const std::byte* frame) noexcept {
    return *std::launder(reinterpret_cast<const PayloadT*>(frame + sizeof(DzFrameHeader)));
}

/// payload 尺寸防御: frame_type 与 payload 类型的绑定纯靠写读两端约定
/// (编译期与写端均无强制点), 此处一次 load+cmp 防止写端布局偏离时
/// 读出帧尾之外 (帧尾恰在页尾时越页即 SIGSEGV, 否则静默错数据更难排查)。
template <typename PayloadT>
bool payload_size_matches(const std::byte* frame) noexcept {
    return frame_header(frame).frame_size == sizeof(DzFrameHeader) + sizeof(PayloadT);
}

/* ── 防御与诊断辅助 ── */

/// 有界 C 字符串长度: 最多扫 max 字节, 未找到 NUL 返回 max (调用方按"无终止符"处理)
inline size_t bounded_strlen(const char* s, size_t max) noexcept {
    size_t len = 0;
    while (len < max && s[len] != '\0') {
        ++len;
    }
    return len;
}

/// 已知帧 payload 尺寸不符诊断: 写端布局偏离 = 版本/布局错配, 必须可观测。
/// 进程内只报一次: tick 频率下逐帧诊断会刷屏并拖慢事件循环 (布局错配也不会中途自愈)。
inline void diag_payload_size_mismatch(const std::byte* frame,
                                       size_t expected_payload_size) noexcept {
    static bool reported = false;
    if (reported) {
        return;
    }
    reported = true;
    const DzFrameHeader& hdr = frame_header(frame);
    dz_diag(std::format("frame dropped | reason=payload_size_mismatch frame_type={} frame_size={} "
                        "expected_payload_size={}",
                        hdr.frame_type, hdr.frame_size, expected_payload_size)
                .c_str());
}

/// 回调异常上报: 消息带回调名; 实现 on_error 则转发, 否则 stderr 诊断行
template <typename StrategyT>
void report_callback_error(StrategyT& strategy,
                           const char* callback,
                           const std::exception& e) noexcept {
    report_error(
        strategy, DZ_EC_INTERNAL,
        std::format("callback exception | callback={} error=\"{}\"", callback, e.what()).c_str());
}

template <typename StrategyT>
void report_callback_unknown(StrategyT& strategy, const char* callback) noexcept {
    report_error(
        strategy, DZ_EC_INTERNAL,
        std::format("callback exception | callback={} error=\"unknown exception\"", callback)
            .c_str());
}

/// on_stop 派发: 异常只走 dz_diag, 不触发 on_error
/// (保证 on_stop 是进程内最后一个用户回调)
template <typename StrategyT>
void dispatch_on_stop(StrategyT& strategy) noexcept {
    if constexpr (HasOnStop<StrategyT>) {
        try {
            strategy.on_stop();
        } catch (const std::exception& e) {
            dz_diag(std::format("on_stop threw | error=\"{}\"", e.what()).c_str());
        } catch (...) {
            dz_diag("on_stop threw | error=\"unknown exception\"");
        }
    }
}

/// 策略自主退出探测 (should_exit 抛异常按回调异常上报, 同一进程只报一次, 视为未请求)
template <typename StrategyT>
bool exit_requested(StrategyT& strategy, bool& probe_error_reported) noexcept {
    if constexpr (!HasShouldExit<StrategyT>) {
        return false;
    } else {
        try {
            return strategy.should_exit();
        } catch (const std::exception& e) {
            if (!probe_error_reported) [[unlikely]] {
                probe_error_reported = true;
                report_callback_error(strategy, "should_exit", e);
            }
        } catch (...) {
            if (!probe_error_reported) [[unlikely]] {
                probe_error_reported = true;
                report_callback_unknown(strategy, "should_exit");
            }
        }
        return false;
    }
}

/// 自主退出码探测 (默认 0; exit_code 抛异常上报后按 0 处理)
template <typename StrategyT>
int32_t requested_exit_code(StrategyT& strategy) noexcept {
    if constexpr (!HasExitCode<StrategyT>) {
        return 0;
    } else {
        try {
            return static_cast<int32_t>(strategy.exit_code());
        } catch (const std::exception& e) {
            report_callback_error(strategy, "exit_code", e);
        } catch (...) {
            report_callback_unknown(strategy, "exit_code");
        }
        return 0;
    }
}

/// 定长 payload 事件分发: 尺寸校验 + 独立异常边界 (消息带回调名)
template <typename PayloadT, typename StrategyT, typename CallbackT>
void dispatch_struct_event(StrategyT& strategy,
                           const std::byte* frame,
                           const char* callback,
                           CallbackT&& callback_fn) noexcept {
    if (!payload_size_matches<PayloadT>(frame)) [[unlikely]] {
        diag_payload_size_mismatch(frame, sizeof(PayloadT));
        return;
    }
    try {
        callback_fn(frame_payload<PayloadT>(frame));
    } catch (const std::exception& e) {
        report_callback_error(strategy, callback, e);
    } catch (...) {
        report_callback_unknown(strategy, callback);
    }
}

/// 变长 UI_INPUT 帧分发: 边界校验 (帧长/data_size/instance_id 终止符) + 异常边界。
/// 注意: SDK 分发层 (api.cpp valid_ext_inst_frame) 已先做同样校验并丢弃坏帧,
/// 这里是引擎侧的 defense-in-depth (两层独立实现, 用户已确认该设计);
/// 因此公开路径下本函数的诊断分支不可达, 但直接调用 handle_event 时仍需自保。
template <typename StrategyT>
void dispatch_ui_input(StrategyT& strategy, const std::byte* frame) noexcept {
    constexpr size_t kMinSize = sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    const DzFrameHeader& hdr = frame_header(frame);
    if (hdr.frame_size < kMinSize) [[unlikely]] {
        dz_diag(std::format("ui input frame dropped | reason=frame_too_short frame_size={}",
                            hdr.frame_size)
                    .c_str());
        return;
    }
    const auto& ext =
        *std::launder(reinterpret_cast<const DzExtInstFrameHeader*>(frame + sizeof(DzFrameHeader)));
    const size_t max_data_size = static_cast<size_t>(hdr.frame_size) - kMinSize;
    if (ext.data_size > max_data_size) [[unlikely]] {
        dz_diag(std::format("ui input frame dropped | reason=payload_out_of_frame frame_size={} "
                            "data_size={}",
                            hdr.frame_size, ext.data_size)
                    .c_str());
        return;
    }
    const size_t instance_id_len = bounded_strlen(ext.instance_id, sizeof(DzInstanceId));
    if (instance_id_len == sizeof(DzInstanceId)) [[unlikely]] {
        dz_diag(std::format("ui input frame dropped | reason=instance_id_not_terminated "
                            "frame_size={}",
                            hdr.frame_size)
                    .c_str());
        return;
    }
    const auto* data = reinterpret_cast<const char*>(frame + kMinSize);
    try {
        strategy.on_ui_input(std::string_view(data, ext.data_size));
    } catch (const std::exception& e) {
        report_callback_error(strategy, "on_ui_input", e);
    } catch (...) {
        report_callback_unknown(strategy, "on_ui_input");
    }
}

/* ── 帧分发: 每帧一个异常边界, 回调异常经 report_error 报告后继续 ── */

template <typename StrategyT>
void handle_md(StrategyT& strategy, const std::byte* frame) noexcept {
    if constexpr (!HasOnTick<StrategyT>) {
        return;  // 无 on_tick: 帧已由 reader 消费, 直接丢弃
    } else {
        if (frame_header(frame).frame_type != DZ_FRAME_TICK) [[likely]] {
            return;
        }
        if (!payload_size_matches<DzTick>(frame)) [[unlikely]] {
            diag_payload_size_mismatch(frame, sizeof(DzTick));
            return;
        }
        try {
            strategy.on_tick(frame_payload<DzTick>(frame));
        } catch (const std::exception& e) {
            report_callback_error(strategy, "on_tick", e);
        } catch (...) {
            report_callback_unknown(strategy, "on_tick");
        }
    }
}

template <typename StrategyT>
void handle_event(StrategyT& strategy, const std::byte* frame, bool& running) noexcept {
    const DzFrameType type = frame_header(frame).frame_type;
    switch (type) {
        case DZ_FRAME_TRADE_REPORT:
            if constexpr (HasOnTradeReport<StrategyT>) {
                dispatch_struct_event<DzTradeReport>(
                    strategy, frame, "on_trade_report",
                    [&strategy](const DzTradeReport& rpt) { strategy.on_trade_report(rpt); });
            }
            break;
        case DZ_FRAME_ORDER_REPORT:
            if constexpr (HasOnOrderReport<StrategyT>) {
                dispatch_struct_event<DzOrderReport>(
                    strategy, frame, "on_order_report",
                    [&strategy](const DzOrderReport& rpt) { strategy.on_order_report(rpt); });
            }
            break;
        case DZ_FRAME_POSITION_INFO:
            if constexpr (HasOnPositionInfo<StrategyT>) {
                dispatch_struct_event<DzPositionInfo>(
                    strategy, frame, "on_position_info",
                    [&strategy](const DzPositionInfo& info) { strategy.on_position_info(info); });
            }
            break;
        case DZ_FRAME_TRADING_ACCOUNT:
            if constexpr (HasOnTradingAccount<StrategyT>) {
                dispatch_struct_event<DzTradingAccount>(
                    strategy, frame, "on_trading_account",
                    [&strategy](const DzTradingAccount& account) {
                        strategy.on_trading_account(account);
                    });
            }
            break;
        case DZ_FRAME_SCHEDULE:
            if constexpr (HasOnSchedule<StrategyT>) {
                dispatch_struct_event<DzScheduleEvent>(
                    strategy, frame, "on_schedule",
                    [&strategy](const DzScheduleEvent& ev) { strategy.on_schedule(ev); });
            }
            break;
        case DZ_FRAME_UI_INPUT:
            if constexpr (HasOnUiInput<StrategyT>) {
                dispatch_ui_input(strategy, frame);
            }
            break;
        case DZ_FRAME_SHUTDOWN:
            running = false;  // 先定格退出决策 (帧已被消费, 反序可能永久阻塞)
            if (frame_header(frame).frame_size <
                sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader)) [[unlikely]] {
                // 畸形 SHUTDOWN 仍然退出 (fail-safe): 通道已不可信, 停比继续安全。
                // 公开路径下 SDK 分发层已先丢弃畸形帧, 本分支仅防御直接调用 handle_event。
                dz_diag(
                    std::format("shutdown frame malformed | reason=frame_too_short frame_size={}",
                                frame_header(frame).frame_size)
                        .c_str());
            }
            dispatch_on_stop(strategy);
            break;
        case DZ_FRAME_ACCOUNT_STATUS:
            if constexpr (HasOnAccountStatus<StrategyT>) {
                dispatch_struct_event<DzAccountStatus>(strategy, frame, "on_account_status",
                                                       [&strategy](const DzAccountStatus& status) {
                                                           strategy.on_account_status(status);
                                                       });
            }
            break;
        default:
            break;
    }
}

/// 批次泵: md 上限 64 / event 上限 8, 双通道均空且未收到退出请求时返回,
/// 由调用方 dz_wait 阻塞等待。每批后探测自主退出 (双通道持续饱和时不会被饿死);
/// SHUTDOWN 后立即停止本批剩余派发, 保证 on_stop 是最后一个用户回调。
template <typename StrategyT>
void pump_once(StrategyT& strategy,
               DzContext* ctx,
               bool& running,
               bool& self_exit,
               bool& exit_probe_error_reported) {
    for (;;) {
        int md_count = 0;
        int event_count = 0;
        for (; md_count < 64 && running; ++md_count) {
            const auto* frame = static_cast<const std::byte*>(dz_next_md(ctx));
            if (frame == nullptr) [[unlikely]] {
                break;
            }
            handle_md(strategy, frame);
        }
        for (; event_count < 8 && running; ++event_count) {
            const auto* frame = static_cast<const std::byte*>(dz_next_event(ctx));
            if (frame == nullptr) [[unlikely]] {
                break;
            }
            handle_event(strategy, frame, running);
        }
        if (!running) {
            return;  // SHUTDOWN: on_stop 已在分发层调用
        }
        // 自主退出探测点: 每批 64/8 之后 (md+event 双饱和时仍能及时退出)
        if (exit_requested(strategy, exit_probe_error_reported)) {
            self_exit = true;
            return;
        }
        if (md_count < 64 && event_count < 8) {
            return;  // 双通道已取空
        }
    }
}

/* ── 启动自检与生命周期回调 ── */

/// 启动自检: 正常启动的一次性信息, 走 stdout (master 以 info 级转发),
/// 不带 [dzsdk] 诊断前缀 (该前缀专属 stderr 诊断 dz_diag)。
template <typename StrategyT>
void report_start() noexcept {
    std::cout << std::format(
        "strategy callbacks | stop={} tick={} trade={} order={} position={} "
        "account={} account_status={} schedule={} input={} error={} self_exit={} exit_code={}\n",
        HasOnStop<StrategyT> ? "yes" : "no", HasOnTick<StrategyT> ? "yes" : "no",
        HasOnTradeReport<StrategyT> ? "yes" : "no", HasOnOrderReport<StrategyT> ? "yes" : "no",
        HasOnPositionInfo<StrategyT> ? "yes" : "no", HasOnTradingAccount<StrategyT> ? "yes" : "no",
        HasOnAccountStatus<StrategyT> ? "yes" : "no", HasOnSchedule<StrategyT> ? "yes" : "no",
        HasOnUiInput<StrategyT> ? "yes" : "no", HasOnError<StrategyT> ? "yes" : "no",
        HasShouldExit<StrategyT> ? "yes" : "no", HasExitCode<StrategyT> ? "yes" : "no");
    std::cout.flush();  // master 管道块缓冲: 显式排空, 保证自检行不滞留
}

/// on_start 派发 + 异常边界: 异常即生命周期失败, 返回 false
/// (调用方须 dz_release 会话后非零退出, 半初始化策略不得进入主循环)。
template <typename StrategyT>
bool dispatch_on_start(StrategyT& strategy, DzContext* ctx) noexcept {
    try {
        strategy.on_start(ctx);
        return true;
    } catch (const std::exception& e) {
        report_callback_error(strategy, "on_start", e);
    } catch (...) {
        report_callback_unknown(strategy, "on_start");
    }
    return false;
}

template <typename StrategyT>
int32_t handle_init_error(StrategyT& strategy) noexcept {
    auto errcode = static_cast<DzErrorCode>(dz_errcode());
    if (errcode == DZ_EC_OK) [[unlikely]] {
        errcode = DZ_EC_INTERNAL;  // 防御: 初始化失败但未置码时不得把失败当成功
    }
    report_error(strategy, errcode, dz_errmsg());
    return errcode;
}

}  // namespace dztrader::strategy_engine_internal

namespace dztrader {

/// 策略类型约束: 必选回调 on_start。
/// 其余回调可选 (on_tick/on_stop/... 由 strategy_engine_internal 编译期探测);
/// on_start 拼写错误无法通过本 concept, 其余可选回调拼错由启动自检行暴露。
template <typename T>
concept Strategy = requires(T t, DzContext* ctx) { t.on_start(ctx); };

/// 运行策略直至收到 DZ_FRAME_SHUTDOWN 或策略自主退出 (should_exit 探测)。
/// 返回值: SHUTDOWN 退出 0; 自主退出返回 exit_code() (默认 0);
/// dz_init 失败返回其错误码; on_start 抛异常返回 DZ_EC_INTERNAL
/// (会话已释放, 带病初始化的策略不进入主循环)。
/// 无 on_tick 的策略不接收行情 (见头注释), 其余可选回调按编译期探测调用。
template <Strategy StrategyT>
int32_t run_strategy(StrategyT& strategy) {
    DzContext* ctx = dz_init();
    if (ctx == nullptr) [[unlikely]] {
        return strategy_engine_internal::handle_init_error(strategy);
    }
    strategy_engine_internal::report_start<StrategyT>();
    if (!strategy_engine_internal::dispatch_on_start(strategy, ctx)) {
        dz_release(ctx);
        return DZ_EC_INTERNAL;
    }

    bool running = true;
    bool self_exit = false;
    bool exit_probe_error_reported = false;
    // 自主退出检查点: on_start 后 + 事件泵每批 64/8 后 (泵内回调可能置用户退出标志,
    // 双通道持续饱和时由 pump_once 内部探测, 不会饿死)
    for (;;) {
        if (strategy_engine_internal::exit_requested(strategy, exit_probe_error_reported)) {
            self_exit = true;
            break;
        }
        strategy_engine_internal::pump_once(strategy, ctx, running, self_exit,
                                            exit_probe_error_reported);
        if (!running || self_exit) {
            break;  // SHUTDOWN: on_stop 已在分发层调用 (进程内最后一个用户回调)
        }
        dz_wait(ctx);
    }

    int32_t exit_code = 0;
    if (self_exit) {
        exit_code = strategy_engine_internal::requested_exit_code(strategy);
        strategy_engine_internal::dispatch_on_stop(strategy);
        std::cout << std::format("strategy exit | reason=self code={}\n", exit_code);
        std::cout.flush();  // master 管道块缓冲: 显式排空, 保证退出行不滞留
    }
    dz_release(ctx);
    return exit_code;
}

}  // namespace dztrader

#endif /* DZTRADER_STRATEGY_ENGINE_H_ */
