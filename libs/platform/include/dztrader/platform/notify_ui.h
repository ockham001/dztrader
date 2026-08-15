#ifndef DZTRADER_PLATFORM_NOTIFY_UI_H_
#define DZTRADER_PLATFORM_NOTIFY_UI_H_

#include <string>
#include <utility>

#include <dztrader/data_type.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

/// UI 通知发送器。绑定 source + writer，封装 DZ_FRAME_NOTIFY_UI 帧（无 instance_id，source 在 payload）。
///
/// 无状态（source/writer 除外），fire-and-forget：内部吞异常并 spdlog::error。
/// 便捷方法 error/warn/info 按级别预设 popup 默认值（ERROR 弹窗，WARN/INFO 不弹窗），
/// 调用方可显式传 popup 覆盖。
class NotifyUi {
public:
    /// @param source 通知来源（进程名/策略 ID），填入 payload 的 source 字段
    /// @param writer 共享内存写入器
    NotifyUi(std::string source, shm::MultiWriter& writer)
        : source_(std::move(source)),
          writer_(writer) {}

    // 禁拷贝/移动：持有引用成员, 绑定后终身不变, 不应放入容器或按值传递
    NotifyUi(const NotifyUi&) = delete;
    NotifyUi& operator=(const NotifyUi&) = delete;
    NotifyUi(NotifyUi&&) = delete;
    NotifyUi& operator=(NotifyUi&&) = delete;
    ~NotifyUi() = default;

    /// 通用发送。popup 默认 false
    void notify(DzNotifyLevel level, std::string msg, bool popup = false);

    /// 错误通知。popup 默认 true（打断用户）
    void error(std::string msg, bool popup = true) {
        notify(DZ_NOTIFY_ERROR, std::move(msg), popup);
    }
    /// 警告通知。popup 默认 false
    void warn(std::string msg, bool popup = false) {
        notify(DZ_NOTIFY_WARN, std::move(msg), popup);
    }
    /// 信息通知。popup 默认 false
    void info(std::string msg, bool popup = false) {
        notify(DZ_NOTIFY_INFO, std::move(msg), popup);
    }

private:
    std::string source_;
    shm::MultiWriter& writer_;
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_NOTIFY_UI_H_
