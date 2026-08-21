#ifndef DZTRADER_WEBUI_CONTROL_GUARD_H_
#define DZTRADER_WEBUI_CONTROL_GUARD_H_

#include <optional>
#include <string>

#include <dztrader/platform/process.h>

namespace dztrader::webui {

/// 控制命令下发的统一守卫结果（REST/WS 双通道共用，契约对齐）：
/// 取代 guard_process_dispatch（REST）与 evaluate_md_connect_precheck（WS）两套平行实现。
/// 判定顺序固定：is_admin → source 有效 → 事件通道(writer)可用 → 目标进程镜像 Running。
enum class ControlGuard {
    Ok,                  ///< 全部通过，可下发
    NotAdmin,            ///< 当前连接/请求非 admin
    SourceInvalid,       ///< source 缺失/不存在
    ChannelUnavailable,  ///< 事件通道(writer)不可用
    ProcessNotRunning,   ///< 目标进程镜像非 Running（含镜像未就绪，保守拒绝）
};

/// 统一守卫纯函数：REST 与 WS 双通道都只调用这一处，保证新增控制命令只需写一处守卫。
/// @param is_admin        当前是否 admin（REST 已由发送方先 403；WS 由 Session.is_admin 提供）
/// @param source_valid    source 是否存在/非空（REST 由 DB 查得；WS 为 source 非空）
/// @param writer_ready    事件通道(writer)是否可用（REST 为 shm_writer_->is_ready；WS 为 event_writer 非空）
/// @param process_running 目标进程镜像是否 Running（REST 查 source_type→进程；WS 查 source）
inline ControlGuard evaluate_control_guard(bool is_admin,
                                           bool source_valid,
                                           bool writer_ready,
                                           bool process_running) {
    if (!is_admin) return ControlGuard::NotAdmin;
    if (!source_valid) return ControlGuard::SourceInvalid;
    if (!writer_ready) return ControlGuard::ChannelUnavailable;
    if (!process_running) return ControlGuard::ProcessNotRunning;
    return ControlGuard::Ok;
}

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_CONTROL_GUARD_H_