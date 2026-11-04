#ifndef DZTRADER_CTP_TD_NOTIFY_TYPES_H_
#define DZTRADER_CTP_TD_NOTIFY_TYPES_H_

#include <functional>
#include <string>

#include <dztrader/data_type.h>  // DzNotifyLevel

namespace dztrader::ctp {

/// td 进程内 UI 通知回调（与 platform::NotifyUi 的 error/warn/info 同参）。
/// 唯一类型真相源：持久化失败分级（PersistWriter）、登录 flush 屏障超时
/// （run_flush_barrier）、账户会话通知（AccountSession）都别名到本类型，禁止各自
/// 重复定义同签名的 std::function。
///
/// 线程约定由调用方声明：PersistWriter 可能从 Writer 线程或 enqueue 调用线程触发，
/// 实现方须线程安全；登录收尾/账户会话路径在 TdApi 主循环线程同步调用。
using TdNotifyFn = std::function<void(DzNotifyLevel level, const std::string& message, bool popup)>;

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_NOTIFY_TYPES_H_
