/**
 * @file strategy_engine.h
 * @brief C++20 模板策略引擎 - 以回调约定接入任意策略类型
 *
 * 核心入口 dztrader::run_strategy(strategy): 封装 dz_init -> 主循环 -> dz_release
 * 的完整进程生命周期, 策略类型只须满足 dztrader::Strategy concept。
 *
 * 回调协议:
 *   - 必选: on_start(DzContext*), on_tick(const DzTick&) -- concept 硬约束,
 *     拼写错误在接入处编译报错
 *   - 可选: on_stop(), on_trade_report(const DzTradeReport&),
 *     on_order_report(const DzOrderReport&), on_schedule(const DzScheduleEvent&),
 *     on_user_input(const DzUserInput&),
 *     on_account_status(const DzAccountStatus&),
 *     on_error(DzErrorCode, std::string_view) -- 编译期探测, 实现了才调用
 *   - 引擎不分发的事件帧 (TD 查询回报 2002-2017 等其余帧)
 *     与非 tick 行情帧被静默忽略, 实现对应回调不会被调用
 *
 * 异常语义:
 *   - on_start 抛异常 = 生命周期失败: 引擎报告错误、释放会话、返回
 *     DZ_EC_INTERNAL 退出 -- 半初始化策略不允许进入主循环响应行情
 *   - 其余回调抛异常: 引擎捕获后经 report_error 报告 (实现 on_error 则
 *     转发, 否则 stderr 诊断行), 进程继续运行
 *   - DZ_FRAME_REQUEST_SHUTDOWN 先置停止标志再调 on_stop, on_stop 为
 *     进程内最后一个用户回调; on_stop 抛异常不影响退出
 *
 * 线程模型 (api.h 句柄契约: SDK 非线程安全):
 *   - 全部回调运行于唯一事件循环线程; 除 dz_notify_self 外, 任何 dz_* C API
 *     只能在回调内 (即引擎线程) 调用, 用户自建线程须在进程退出前 join
 *   - 回调参数是 SHM/SDK 缓冲的引用, 仅在本次回调返回前有效, 需留存请拷贝
 *
 * 自检: 启动时向 stdout 输出一行回调探测清单 (stop/trade/order/schedule/input/error
 * 的 yes/no), 将"回调名拼错导致永不触发"的静默失败暴露在首次运行。
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
namespace dztrader::strategy_engine_internal {

/* ── 引擎内部: 模板实现细节, 策略用户勿直接引用 ──
   前置于公开入口: run_strategy 内的限定名在模板定义点须可见。 */

/* ── 可选回调探测 concept ── */

template <typename T>
concept HasOnStop = requires(T t) { t.on_stop(); };

template <typename T>
concept HasOnTradeReport = requires(T t, const DzTradeReport& rpt) { t.on_trade_report(rpt); };

template <typename T>
concept HasOnOrderReport = requires(T t, const DzOrderReport& rpt) { t.on_order_report(rpt); };

template <typename T>
concept HasOnSchedule = requires(T t, const DzScheduleEvent& ev) { t.on_schedule(ev); };

template <typename T>
concept HasOnUserInput = requires(T t, const DzUserInput& input) { t.on_user_input(input); };

template <typename T>
concept HasOnError =
    requires(T t, DzErrorCode code, std::string_view msg) { t.on_error(code, msg); };

template <typename T>
concept HasOnAccountStatus = requires(T t, const DzAccountStatus& status) {
    t.on_account_status(status);
};

/* ── 异常报告: 分发层捕获 → report_error 转发/兜底 → dz_diag (导出诊断输出) 终端。
      report_error noexcept: 用户 on_error 再抛异常也在此消化,
      异常不逃逸出引擎。 ── */

template <typename StrategyT>
void report_error(StrategyT& strategy, DzErrorCode errcode, std::string_view message) noexcept {
    if constexpr (HasOnError<StrategyT>) {
        try {
            strategy.on_error(errcode, message);
        } catch (const std::exception& e) {
            dz_diag(std::format("on_error threw | reason=\"{}\" original=\"{}\"", e.what(),
                                message)
                        .c_str());
        } catch (...) {
            dz_diag(std::format("on_error threw unknown exception | original=\"{}\"", message)
                        .c_str());
        }
    } else {
        dz_diag(std::format("callback exception | code={} message=\"{}\"", errcode, message)
                    .c_str());
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

/* ── 帧分发: 每帧一个异常边界, 回调异常经 report_error 报告后继续 ── */

template <typename StrategyT>
void handle_md(StrategyT& strategy, const std::byte* frame) noexcept {
    if (frame_header(frame).frame_type != DZ_FRAME_RTN_MD_TICK) [[likely]] {
        return;
    }
    if (!payload_size_matches<DzTick>(frame)) [[unlikely]] {
        return;  // 写端布局偏离防御: 丢弃而非越界读
    }
    try {
        strategy.on_tick(frame_payload<DzTick>(frame));
    } catch (const std::exception& e) {
        report_error(strategy, DZ_EC_INTERNAL, e.what());
    } catch (...) {
        report_error(strategy, DZ_EC_INTERNAL, "unknown exception in on_tick");
    }
}

template <typename StrategyT>
void handle_event(StrategyT& strategy, const std::byte* frame, bool& running) noexcept {
    const DzFrameType type = frame_header(frame).frame_type;
    try {
        switch (type) {
            case DZ_FRAME_TD_TRADE_RPT:
                if constexpr (HasOnTradeReport<StrategyT>) {
                    if (payload_size_matches<DzTradeReport>(frame)) [[likely]] {
                        strategy.on_trade_report(frame_payload<DzTradeReport>(frame));
                    }
                }
                break;
            case DZ_FRAME_TD_ORDER_RPT:
                if constexpr (HasOnOrderReport<StrategyT>) {
                    if (payload_size_matches<DzOrderReport>(frame)) [[likely]] {
                        strategy.on_order_report(frame_payload<DzOrderReport>(frame));
                    }
                }
                break;
            case DZ_FRAME_STG_SCHEDULE:
                if constexpr (HasOnSchedule<StrategyT>) {
                    if (payload_size_matches<DzScheduleEvent>(frame)) [[likely]] {
                        strategy.on_schedule(frame_payload<DzScheduleEvent>(frame));
                    }
                }
                break;
            case DZ_FRAME_STG_USER_INPUT:
                if constexpr (HasOnUserInput<StrategyT>) {
                    // 变长 ext_inst 帧: DzFrameHeader + DzExtInstFrameHeader + 变长 payload,
                    // 无固定 payload_size 可校验, 按 ext 头 data_size 解析 (契约 strategy)
                    const auto& ext =
                        *std::launder(reinterpret_cast<const DzExtInstFrameHeader*>(
                            frame + sizeof(DzFrameHeader)));
                    const auto* payload = static_cast<const char*>(
                        static_cast<const void*>(frame + sizeof(DzFrameHeader) +
                                                 sizeof(DzExtInstFrameHeader)));
                    strategy.on_user_input(DzUserInput{
                        .instance_id = std::string_view(ext.instance_id),
                        .data = payload,
                        .data_size = ext.data_size,
                    });
                }
                break;
            case DZ_FRAME_REQUEST_SHUTDOWN:
                running = false;  // 先定格退出决策 (帧已被消费, 反序可能永久阻塞)
                if constexpr (HasOnStop<StrategyT>) {
                    strategy.on_stop();
                }
                break;
            case DZ_FRAME_ACCOUNT_STATUS:
                if constexpr (HasOnAccountStatus<StrategyT>) {
                    if (payload_size_matches<DzAccountStatus>(frame)) [[likely]] {
                        strategy.on_account_status(frame_payload<DzAccountStatus>(frame));
                    }
                }
                break;
            default:
                break;
        }
    } catch (const std::exception& e) {
        report_error(strategy, DZ_EC_INTERNAL, e.what());
    } catch (...) {
        report_error(strategy, DZ_EC_INTERNAL, "unknown exception in event callback");
    }
}

/// 批次泵: md 上限 64 / event 上限 8, 双通道均空且未收到退出请求时返回,
/// 由调用方 dz_wait 阻塞等待。SHUTDOWN 后立即停止本批剩余派发,
/// 保证 on_stop 是最后一个用户回调。
template <typename StrategyT>
void pump_once(StrategyT& strategy, DzContext* ctx, bool& running) {
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
        if (!running || (md_count < 64 && event_count < 8)) {
            break;
        }
    }
}

/* ── 启动自检与生命周期回调 ── */

/// 启动自检: 正常启动的一次性信息, 走 stdout (master 以 info 级转发),
/// 不带 [dzsdk] 诊断前缀 (该前缀专属 stderr 诊断 dz_diag)。
template <typename StrategyT>
void report_start() noexcept {
    std::cout << std::format("strategy callbacks | stop={} trade={} order={} schedule={} "
                             "input={} error={}\n",
                             HasOnStop<StrategyT> ? "yes" : "no",
                             HasOnTradeReport<StrategyT> ? "yes" : "no",
                             HasOnOrderReport<StrategyT> ? "yes" : "no",
                             HasOnSchedule<StrategyT> ? "yes" : "no",
                             HasOnUserInput<StrategyT> ? "yes" : "no",
                             HasOnError<StrategyT> ? "yes" : "no");
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
        report_error(strategy, DZ_EC_INTERNAL, e.what());
    } catch (...) {
        report_error(strategy, DZ_EC_INTERNAL, "unknown exception in on_start");
    }
    return false;
}

template <typename StrategyT>
int32_t handle_init_error(StrategyT& strategy) noexcept {
    const auto errcode = static_cast<DzErrorCode>(dz_errcode());
    report_error(strategy, errcode, dz_errmsg());
    return errcode;
}

}  // namespace dztrader::strategy_engine_internal

namespace dztrader {

/// 策略类型约束: 必选回调 on_start + on_tick。
/// 其余回调可选 (strategy_engine_internal 按需探测), 拼写错误无法通过本 concept。
template <typename T>
concept Strategy = requires(T t, DzContext* ctx, const DzTick& tick) {
    t.on_start(ctx);
    t.on_tick(tick);
};

/// 运行策略直至收到 DZ_FRAME_REQUEST_SHUTDOWN。
/// 返回 0 = 正常退出; dz_init 失败返回其错误码; on_start 抛异常返回
/// DZ_EC_INTERNAL (会话已释放, 带病初始化的策略不进入主循环)。
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
    while (running) {
        strategy_engine_internal::pump_once(strategy, ctx, running);
        if (!running) {
            break;
        }
        dz_wait(ctx);
    }
    dz_release(ctx);
    return 0;
}

}  // namespace dztrader

#endif /* DZTRADER_STRATEGY_ENGINE_H_ */
