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

/// 校验行情读者身份: "stg.<name>" 且 name 为 registry 中已注册的策略条目。
/// 仅作健全性闸门 (同部署进程互信), 防陌生名字污染 readers map 或误删他人条目。
/// supervisor 未注入时拒绝 (fail-closed, 生产环境必注入)。
bool is_registered_strategy(const ProcessSupervisor* supervisor, std::string_view subscriber,
                            std::string& out_name) {
    constexpr std::string_view kPrefix = "stg.";
    if (!subscriber.starts_with(kPrefix)) {
        return false;
    }
    out_name.assign(subscriber.substr(kPrefix.size()));
    if (out_name.empty()) {
        return false;
    }
    if (supervisor == nullptr) {
        return false;
    }
    const auto* entry = supervisor->find_registry_entry(out_name);
    return entry != nullptr && entry->category == Category::Strategy;
}

}  // namespace

void ShmManager::notify_md_channel_subscriber_update(std::string_view source_name) {
    // 通过事件通道发 UPDATE_SHM_MD_SUBSCRIBER 帧 instance_id=source_name
    // 行情进程（dzmd_ctp）的 reader 收到后判断 instance_id == name_,
    // 调用 spi_.refresh_subscribers() 刷新行情通道 writer 缓存的订阅者列表（内部加自旋锁）
    // dzweb/其他进程收到 instance_id != "*" 的帧时忽略（不订阅行情通道）
    platform::write_ext_inst_raw(event_writer_, DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER, source_name);
    SPDLOG_INFO("md channel subscriber update notified | source={}", source_name);
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

    auto it = md_channels_.find(std::string(channel_name));
    if (it != md_channels_.end() && it->second) {
        it->second->clear_readers();
        for (const auto& sub : subscribers) {
            (void)it->second->add_reader(sub, /*pid=*/0);
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
    // handle_frame 第一层处理。无 RTN: 注册失败不阻断数据消费 (reader 游标独立于
    // 注册), 唤醒缺失由单信号量 + 任意事件帧唤醒后排空兜底 (契约 02-shm)。
    const std::string channel_name(view.ext_inst_id());

    nlohmann::json payload;
    try {
        payload = shm::decode_ext_inst_json<nlohmann::json>(view);
    } catch (const std::exception& e) {
        SPDLOG_WARN("md reader register rejected | reason=bad_payload error=\"{}\"", e.what());
        return;
    }
    if (!payload.is_object() || !payload.contains("subscriber") ||
        !payload["subscriber"].is_string()) {
        SPDLOG_WARN("md reader register rejected | reason=missing_subscriber");
        return;
    }
    const std::string subscriber = payload["subscriber"].get<std::string>();

    std::string strategy_name;
    if (!is_registered_strategy(supervisor_, subscriber, strategy_name)) {
        SPDLOG_WARN("md reader register rejected | reason=unknown_subscriber subscriber={}",
                    subscriber);
        return;
    }

    auto it = md_channels_.find(channel_name);
    if (it == md_channels_.end() || !it->second) {
        // 启动顺序保证 md 先于策略 (start_all 两趟); 此处缺失 = md 未启动/已移除
        SPDLOG_WARN(
            "md reader register rejected | reason=channel_not_found channel={} subscriber={}",
            channel_name, subscriber);
        return;
    }
    const bool added = it->second->add_reader(subscriber, /*pid=*/0);
    SPDLOG_INFO("md reader registered | channel={} subscriber={} new={}", channel_name,
                subscriber, added);
    notify_md_channel_subscriber_update(channel_name);
}

void ShmManager::handle_md_reader_unregister(const shm::FrameView& view) {
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

    std::string strategy_name;
    if (!is_registered_strategy(supervisor_, subscriber, strategy_name)) {
        SPDLOG_WARN("md reader unregister rejected | reason=unknown_subscriber subscriber={}",
                    subscriber);
        return;
    }

    auto it = md_channels_.find(channel_name);
    if (it == md_channels_.end() || !it->second) {
        return;  // 通道已不存在, 读者条目随之消失, 无需处理
    }
    it->second->remove_reader(subscriber);
    SPDLOG_INFO("md reader unregistered | channel={} subscriber={}", channel_name, subscriber);
    notify_md_channel_subscriber_update(channel_name);
}

void ShmManager::remove_reader_from_all_md_channels(const std::string& sub_name) {
    for (auto& [name, meta] : md_channels_) {
        if (!meta) {
            continue;
        }
        meta->remove_reader(sub_name);
        notify_md_channel_subscriber_update(name);
    }
}

}  // namespace dztrader::master
