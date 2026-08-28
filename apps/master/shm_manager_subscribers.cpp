#include "shm_manager.h"

#include "process_supervisor.h"

#include <dztrader/core/this_process.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/frame_codec.h>
#include <spdlog/spdlog.h>

#include <format>
#include <string>

namespace dztrader::master {

namespace {

/// 校验读者身份 = 进程 instance_id (契约 shm: 任意已注册进程, 不限策略):
/// - "stg.<name>": name 必须为已注册策略条目
/// - 裸进程名: 必须为已注册的非策略类别条目 (策略身份必须带 stg. 前缀)
/// 仅作健全性闸门 (同部署进程互信), 防陌生名字污染 readers map 或误删他人条目。
/// supervisor 未注入时拒绝 (fail-closed, 生产环境必注入)。
bool is_registered_process(const ProcessSupervisor* supervisor, std::string_view subscriber) {
    if (supervisor == nullptr) {
        return false;
    }
    constexpr std::string_view kPrefix = "stg.";
    if (subscriber.starts_with(kPrefix)) {
        const auto name = subscriber.substr(kPrefix.size());
        if (name.empty()) {
            return false;
        }
        const auto* entry = supervisor->find_registry_entry(name);
        return entry != nullptr && entry->category == Category::Strategy;
    }
    const auto* entry = supervisor->find_registry_entry(subscriber);
    return entry != nullptr && entry->category != Category::Strategy;
}

/// 写读者接入/断开 RTN 帧 (契约 shm: instance_id=请求进程名,
/// payload={channel, ok, message 失败必填})
void write_md_reader_rtn(shm::MultiWriter& w, DzFrameType type, std::string_view subscriber,
                         std::string_view channel, bool ok, std::string_view message = "") {
    nlohmann::json payload = {{"channel", channel}, {"ok", ok}};
    if (!ok) {
        payload["message"] = message;  // 失败时必填 (契约 shm)
    }
    platform::write_ext_inst_json_obj(w, type, subscriber, payload);
}

}  // namespace

void ShmManager::notify_md_channel_subscriber_update(std::string_view source_name) {
    // 通过事件通道发 UPDATE_SHM_MD_SUBSCRIBER 帧 instance_id=source_name
    // 行情进程（dzmd_ctp）的 reader 收到后判断 instance_id == name_,
    // 调用 spi_.refresh_subscribers() 刷新行情通道 writer 缓存的订阅者列表（内部加自旋锁）
    // dzweb/其他进程收到 instance_id != "*" 的帧时忽略（不订阅行情通道）
    if (platform::write_ext_inst_raw(event_writer_, DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER,
                                     source_name)) {
        SPDLOG_INFO("md channel subscriber update notified | source={}", source_name);
    }
    // 写失败由 frame_codec 内部记 ERROR, 此处不再打成功日志误导
}

void ShmManager::update_subscribers(std::string_view channel_name,
                                    const std::vector<std::string>& subscribers) {
    // 查找通道并更新订阅者列表
    if (channel_name == shm::channel_name("dzevent") && event_meta_) {
        event_meta_->clear_readers();
        for (const auto& sub : subscribers) {
            (void)event_meta_->add_reader(sub, /*pid=*/0);
        }
        return;
    }

    // 查表 key 与 create/close/mark/register 统一用 shm::channel_name (自检收尾,
    // 当前为恒等变换; 该函数无生产调用方, 属既有保留代码, 仅保持一致)
    auto it = md_channels_.find(shm::channel_name(channel_name));
    if (it != md_channels_.end() && it->second.meta) {
        it->second.meta->clear_readers();
        for (const auto& sub : subscribers) {
            (void)it->second.meta->add_reader(sub, /*pid=*/0);
        }
    }
}

void ShmManager::notify_subscriber_update() {
    // 1. 刷新 master 自己的 event_writer_ 内部缓存的订阅者列表
    //    master writer 在 asio 线程直接可用，无需走"写帧->信号量->post->drain->refresh"路径
    //    （该路径仅适用于子进程：子进程 writer 缓存需要 reader 收到 UPDATE_SHM_EVENT_SUBSCRIBER
    //    帧后才能 refresh） master 自己的 drain_event_channel 不处理 UPDATE_SHM_EVENT_SUBSCRIBER
    //    帧（走 default 忽略）
    (void)event_writer_.refresh_subscribers();
    // 2. 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧 (无 instance_id) 给子进程
    //    子进程 reader 收到后调用 refresh_subscribers() 刷新自己的 writer 缓存
    platform::write_ext_raw(event_writer_, DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER);
}

void ShmManager::register_subscriber(const std::string& name, uint64_t pid) {
    if (!event_meta_) {
        SPDLOG_WARN("register_subscriber: event_meta not ready | name={}", name);
        return;
    }
    bool ok = event_meta_->add_reader(name, pid);
    if (ok) {
        SPDLOG_INFO("subscriber registered | name={} pid={}", name, pid);
    } else {
        SPDLOG_WARN("subscriber already exists, skip | name={}", name);
    }
    notify_subscriber_update();
}

void ShmManager::unregister_subscriber(const std::string& name) {
    if (!event_meta_) {
        SPDLOG_WARN("unregister_subscriber: event_meta not ready | name={}", name);
        return;
    }
    event_meta_->remove_reader(name);
    SPDLOG_INFO("subscriber unregistered | name={}", name);
    notify_subscriber_update();
}

void ShmManager::reset_subscribers() {
    if (!event_meta_) {
        SPDLOG_WARN("reset_subscribers: event_meta not ready");
        return;
    }
    // 清空上次运行遗留的僵尸订阅者
    event_meta_->clear_readers();

    // 注册 master 自己：订阅者名 = name_ (无 pid 后缀, 与 event_reader_ 的 reader_name 一致,
    // 保证 release_old_pages 的 set_reader_page_index 按名找到条目)
    auto master_pid = static_cast<uint64_t>(dztrader::this_process::pid());
    std::string master_name = name_;
    (void)event_meta_->add_reader(master_name, master_pid);
    SPDLOG_INFO("subscribers reset | master={} pid={}", master_name, master_pid);

    // 通知子进程（此时子进程可能尚未启动，但写入帧是无害的，后续启动的子进程
    // 会在 Reader::create 时 sync_from_channel_meta 获取最新列表）
    notify_subscriber_update();
}

void ShmManager::handle_md_reader_register(const shm::FrameView& view) {
    // 帧头 instance_id = 目标行情通道名; 消费方是 master (不匹配 name_), 故在
    // handle_frame 第一层处理。契约 shm: 必回 RTN_MD_READER_REGISTER (1015),
    // 时序固定: 更新列表 -> 广播 UPDATE -> 回 RTN; 失败携带 message。
    const std::string channel_name(view.ext_inst_id());

    nlohmann::json payload;
    try {
        payload = shm::decode_ext_inst_json<nlohmann::json>(view);
    } catch (const std::exception& e) {
        // payload 损坏无法获知请求方身份, 无法回 RTN, 仅记日志
        SPDLOG_WARN("md reader register rejected | reason=bad_payload error=\"{}\"", e.what());
        return;
    }
    if (!payload.is_object() || !payload.contains("subscriber") ||
        !payload["subscriber"].is_string()) {
        SPDLOG_WARN("md reader register rejected | reason=missing_subscriber");
        return;
    }
    const std::string subscriber = payload["subscriber"].get<std::string>();

    if (!is_registered_process(supervisor_, subscriber)) {
        SPDLOG_WARN("md reader register rejected | reason=unknown_subscriber subscriber={}",
                    subscriber);
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_REGISTER, subscriber,
                            channel_name, false, "unknown subscriber");
        return;
    }

    auto it = md_channels_.find(shm::channel_name(channel_name));
    if (it == md_channels_.end() || it->second.status == MdChannelStatus::Tombstone) {
        // 通道未配置或已 tombstone (Remove): 视为通道未配置 (契约 4.7:
        // tombstone 保留条目但不得放行读者接入; 主进程不会拉起对应行情进程)
        SPDLOG_WARN(
            "md reader register rejected | reason=channel_not_configured channel={} subscriber={}",
            channel_name, subscriber);
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_REGISTER, subscriber,
                            channel_name, false, "channel not configured");
        return;
    }
    if (it->second.status != MdChannelStatus::Running || !it->second.meta) {
        // 行情进程未运行 (Stopped 待重启): 元数据保留但进程未拉起
        SPDLOG_WARN(
            "md reader register rejected | reason=md_not_running channel={} subscriber={}",
            channel_name, subscriber);
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_REGISTER, subscriber,
                            channel_name, false, "market process not running");
        return;
    }
    if (!it->second.ready) {
        // 通道未就绪: 行情进程尚未发出 NOTIFY_MD_STARTED (含在途停止场景)
        SPDLOG_WARN("md reader register rejected | reason=channel_not_ready channel={} subscriber={}",
                    channel_name, subscriber);
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_REGISTER, subscriber,
                            channel_name, false, "channel not ready");
        return;
    }

    const bool added = it->second.meta->add_reader(subscriber, /*pid=*/0);
    SPDLOG_INFO("md reader registered | channel={} subscriber={} new={}", channel_name,
                subscriber, added);
    notify_md_channel_subscriber_update(channel_name);
    write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_REGISTER, subscriber,
                        channel_name, true);
}

void ShmManager::handle_md_reader_unregister(const shm::FrameView& view) {
    // 契约 shm: 必回 RTN_MD_READER_UNREGISTER (1016)。对通道不存在/已关闭
    // 幂等成功 (读者条目已随停止后果清空, 多路径叠加无害)。
    const std::string channel_name(view.ext_inst_id());

    nlohmann::json payload;
    try {
        payload = shm::decode_ext_inst_json<nlohmann::json>(view);
    } catch (const std::exception& e) {
        SPDLOG_WARN("md reader unregister rejected | reason=bad_payload error=\"{}\"", e.what());
        return;
    }
    if (!payload.is_object() || !payload.contains("subscriber") ||
        !payload["subscriber"].is_string()) {
        SPDLOG_WARN("md reader unregister rejected | reason=missing_subscriber");
        return;
    }
    const std::string subscriber = payload["subscriber"].get<std::string>();

    if (!is_registered_process(supervisor_, subscriber)) {
        SPDLOG_WARN("md reader unregister rejected | reason=unknown_subscriber subscriber={}",
                    subscriber);
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_UNREGISTER, subscriber,
                            channel_name, false, "unknown subscriber");
        return;
    }

    auto it = md_channels_.find(shm::channel_name(channel_name));
    if (it == md_channels_.end() || it->second.status == MdChannelStatus::Tombstone ||
        !it->second.meta) {
        // 通道不存在 / 已 tombstone / 已物理销毁: 读者条目随之消失, 幂等成功
        write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_UNREGISTER, subscriber,
                            channel_name, true);
        return;
    }
    it->second.meta->remove_reader(subscriber);
    SPDLOG_INFO("md reader unregistered | channel={} subscriber={}", channel_name, subscriber);
    notify_md_channel_subscriber_update(channel_name);
    write_md_reader_rtn(event_writer_, DZ_FRAME_RTN_MD_READER_UNREGISTER, subscriber,
                        channel_name, true);
}

void ShmManager::remove_reader_from_all_md_channels(const std::string& sub_name) {
    for (auto& [name, state] : md_channels_) {
        if (!state.meta) {
            continue;  // 已关闭通道读者已随停止后果清空
        }
        state.meta->remove_reader(sub_name);
        notify_md_channel_subscriber_update(name);
    }
}

}  // namespace dztrader::master
