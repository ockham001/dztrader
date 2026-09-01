#include "process_supervisor.h"

#include <dztrader/core/encoding.h>
#include <dztrader/platform/frame_codec.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <thread>

namespace dztrader::master {

namespace {

/// 构造订阅者名。命名规则（无 pid 后缀，进程名即身份，重启复用同名）：
/// - 系统内部进程（GatewayMd/GatewayTd/WebUI）: <entry.name>
///   子进程通过 dztrader::this_process::exe_stem() 构造自己的 NamedSemaphore 名，
///   master 必须用相同名字注册才能 notify 到对应信号量。
///   任务 B 后 entry.name 已与 exe stem 一致 (dzweb/dzmd_ctp/dztd_ctp),
///   故直接用 entry.name 构造, 无需再走 exe.stem()。
/// - 策略（Strategy）: stg.<name>
/// 唯一性由 master 生命周期监督保证（退出即注销、启动即重置），无需 pid 区分实例。
std::string make_subscriber_name(const ProcessEntry& entry) {
    if (entry.category == Category::Strategy) {
        return std::format("stg.{}", entry.name);
    }
    return entry.name;
}

}  // namespace

std::vector<std::vector<std::string>> build_shutdown_batches(
    const std::vector<std::pair<std::string, Category>>& running) {
    // 逆序 = 启动顺序严格互逆: md 最先启动 → 最后停止; dzweb 最后启动 → 最后关闭
    // (UI 最后关闭可展示进度)。类别遍历顺序即批次顺序。
    std::vector<std::vector<std::string>> batches;
    for (Category cat : {Category::Strategy, Category::GatewayTd, Category::GatewayMd,
                         Category::WebUI}) {
        std::vector<std::string> names;
        for (const auto& [name, category] : running) {
            if (category == cat) {
                names.push_back(name);
            }
        }
        if (!names.empty()) {
            batches.push_back(std::move(names));
        }
    }
    return batches;
}

ProcessSupervisor::ProcessSupervisor(boost::asio::io_context& ioc,
                                     ProcessRegistry& registry,
                                     ShmManager& shm_mgr,
                                     OrphanGuard& orphan_guard,
                                     int single_stop_timeout_sec,
                                     int md_ready_timeout_sec)
    : ioc_(ioc), registry_(registry), shm_mgr_(shm_mgr),
      notify_ui_(shm_mgr.name(), shm_mgr.event_writer()),
      orphan_guard_(orphan_guard),
      single_stop_timeout_sec_(single_stop_timeout_sec),
      md_ready_timeout_sec_(md_ready_timeout_sec)
{}

void ProcessSupervisor::start_all() {
    const auto& entries = registry_.entries();
    SPDLOG_INFO("starting configured processes | total={}", entries.size());

    auto start_one = [this](const ProcessEntry& entry) {
        try {
            // launch_child 返回值在此忽略: 启动失败时 launch_child 内部已处理失败路径反馈
            // (失败路径 B 推 crashed + notify_ui, 失败路径 D 推 crashed + notify_ui),
            // start_all 不需要额外处理 (与原行为一致)
            (void)launch_child(entry);   // 内部按需扫描填充 exe; GatewayMd 成功后
                                         // 内部挂 ready watchdog (契约 4.5, 每次 spawn 均挂)

            // 行情进程启动后通过事件通道发 UPDATE_SHM_MD_SUBSCRIBER instance_id=name 帧,
            // 通知行情进程刷新 spi_.writer_ 缓存的行情通道订阅者列表
            // （子进程 reader 创建后会读到该帧,即使启动时序未对齐也无害,
            //   因为 SPI writer 在 create 时已 sync_from_channel_meta 获取初始列表）
            if (entry.category == Category::GatewayMd) {
                shm_mgr_.notify_md_channel_subscriber_update(entry.name);
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("start process failed, skipping | name={} error=\"{}\"", entry.name,
                         e.what());
        }
    };

    // 三趟启动编排 (契约 4.4):
    //   第一趟: 全部行情网关 (md 必须先行: 策略 SDK 需已存在的 md 通道 + 预注册 reader)
    //   第二趟: td / webui 立即启动, 不等待 md ready
    //   第三趟: 同步有界等待被策略引用的 md ready, 逐源启动绑定策略
    //           超时后未 ready 源的策略进 pending
    for (const auto& entry : entries) {
        if (entry.category == Category::GatewayMd) {
            start_one(entry);
        }
    }
    for (const auto& entry : entries) {
        if (entry.category != Category::GatewayMd && entry.category != Category::Strategy) {
            start_one(entry);
        }
    }
    wait_for_md_ready_and_start_strategies();
}

void ProcessSupervisor::arm_md_ready_watchdog(const std::string& source) {
    // 每次 spawn md 进程后启动 ready 定时器; 收到对应 NOTIFY_MD_STARTED 时由
    // on_md_channel_ready 取消。超时按"是否存在运行中绑定策略"区分行为。
    auto timer = std::make_unique<boost::asio::steady_timer>(
        ioc_, std::chrono::seconds(md_ready_timeout_sec_));
    auto* raw = timer.get();
    raw->async_wait([this, source](const boost::system::error_code& ec) {
        on_md_ready_timeout(source, ec);
    });
    // 防御性: 同名旧 timer 先取消再覆盖 (不应发生, 同源连续 spawn 时保护)
    if (auto it = md_ready_timers_.find(source); it != md_ready_timers_.end()) {
        it->second->cancel();
        md_ready_timers_.erase(it);
    }
    md_ready_timers_[source] = std::move(timer);
}

void ProcessSupervisor::on_md_ready_timeout(const std::string& source,
                                            const boost::system::error_code& ec) {
    if (ec) {
        // 被取消 (收到 STARTED, on_md_channel_ready 已 erase)
        return;
    }
    md_ready_timers_.erase(source);
    // 是否仍有运行中绑定策略 (md 运行期重启场景: 策略不退出, reader 保持打开)
    bool has_running_bound = false;
    for (const auto& name : strategies_bound_to(source)) {
        auto child = find_child(name);
        if (child && child->state() != ChildState::Stopped) {
            has_running_bound = true;
            break;
        }
    }
    if (has_running_bound) {
        // 存在运行中绑定策略: 判定该次 md 启动失败, terminate + 走 restart policy
        SPDLOG_ERROR("md ready timeout, terminating unready md with running strategy | source={}",
                     source);
        auto child = find_child(source);
        if (child) {
            notify_ui_.error(std::string("market source failed to become ready, restarting | source=") +
                             source);
            force_killed_names_.insert(source);  // 不按 crash 弹窗
            child->terminate();
        }
        return;
    }
    // 无运行中绑定策略 (启动编排超时或策略已退): 不杀 md, 策略保持 pending,
    // 记录 ERROR, 等待晚到 STARTED (on_md_channel_ready 会补启动)
    SPDLOG_ERROR("md ready timeout, strategies stay pending | source={}", source);
}

std::vector<std::string> ProcessSupervisor::strategies_bound_to(const std::string& source) const {
    std::vector<std::string> names;
    for (const auto& e : registry_.entries()) {
        if (e.category == Category::Strategy && e.md_source == source) {
            names.push_back(e.name);
        }
    }
    return names;
}

void ProcessSupervisor::pre_register_strategy_reader(const std::string& strategy_name,
                                                     const std::string& md_source) {
    // 契约 4.3: 策略 spawn 前 add_reader(stg.<name>) 预注册 md 读者。
    // 策略 SDK 的 dz_init 只 open_only, 不自行注册 (1013-1016 不再用于普通策略);
    // master 统一维护 readers 表。随后通知 md 进程刷新 writer 缓存。
    const auto* state = shm_mgr_.md_channel_state(md_source);
    if (!state || !state->meta || state->status != MdChannelStatus::Running ||
        !state->ready) {
        // 源未 ready: 不应走到此处 (调用方保证 ready), 防御性记录
        SPDLOG_WARN("pre-register reader skipped, source not ready | strategy={} source={}",
                    strategy_name, md_source);
        return;
    }
    const auto sub_name = std::format("stg.{}", strategy_name);
    (void)state->meta->add_reader(sub_name, /*pid=*/0);
    SPDLOG_INFO("strategy md reader pre-registered | strategy={} source={} reader={}",
                strategy_name, md_source, sub_name);
    shm_mgr_.notify_md_channel_subscriber_update(md_source);
}

void ProcessSupervisor::wait_for_md_ready_and_start_strategies() {
    // 契约 4.4 第三趟: 同步有界等待被至少一个策略引用的 md ready。
    // start_all 在 ioc.run() 之前执行, 不能依赖 asio 异步, 只能同步 drain
    // event channel 处理 NOTIFY_MD_STARTED。
    // 收集被引用的源 (去重)
    std::vector<std::string> referenced_sources;
    {
        std::unordered_set<std::string> seen;
        for (const auto& e : registry_.entries()) {
            if (e.category == Category::Strategy && !e.md_source.empty() &&
                seen.insert(e.md_source).second) {
                referenced_sources.push_back(e.md_source);
            }
        }
    }
    if (referenced_sources.empty()) {
        // 无绑定 md 的策略 (空 md_source, 测试/手工构造): 直接启动
        for (const auto& e : registry_.entries()) {
            if (e.category == Category::Strategy && e.md_source.empty() && !find_child(e.name)) {
                (void)launch_child(e);
            }
        }
        return;  // 无策略, 无需等待
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(md_ready_timeout_sec_);
    std::unordered_set<std::string> ready_sources;
    // 单个源的就绪处理: 首次变为 ready 时立即启动其绑定策略
    // (契约 4.4: 已 ready 的源不因其他源未 ready 而等待)。幂等: 已在
    // ready_sources 中的源跳过, start_strategies_for_source 内部再按 find_child 去重。
    auto promote_ready_source = [&](const std::string& source) {
        if (ready_sources.count(source)) {
            return;
        }
        const auto* state = shm_mgr_.md_channel_state(source);
        if (state && state->status == MdChannelStatus::Running && state->ready) {
            ready_sources.insert(source);
            start_strategies_for_source(source);
        }
    };
    while (std::chrono::steady_clock::now() < deadline) {
        // 同步 drain: 处理已到达的 NOTIFY_MD_STARTED (mark_md_channel_ready 内部
        // 回调 on_md_channel_ready 启动 pending 策略)
        shm_mgr_.drain_event_channel();
        for (const auto& source : referenced_sources) {
            promote_ready_source(source);
        }
        if (ready_sources.size() == referenced_sources.size()) {
            break;  // 全部 ready
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    shm_mgr_.drain_event_channel();  // 最后一轮排空 (其处理的 STARTED 由下方补检)
    for (const auto& source : referenced_sources) {
        promote_ready_source(source);
    }

    // 无绑定 md 的策略 (空 md_source, 测试/手工构造): 直接启动 (与空源分支同语义)
    for (const auto& e : registry_.entries()) {
        if (e.category == Category::Strategy && e.md_source.empty() && !find_child(e.name)) {
            (void)launch_child(e);
        }
    }

    // 超时后仍未 ready 的源: 绑定策略进 pending (不 spawn)。
    // 已 ready 源的策略在循环内 spawn (on_md_channel_ready 处理的晚到 STARTED
    // 场景走 pending, 不在此列)。
    for (const auto& source : referenced_sources) {
        if (ready_sources.count(source)) {
            continue;
        }
        for (const auto& name : strategies_bound_to(source)) {
            if (find_child(name)) {
                continue;  // 已在运行 (晚到 STARTED 已启动)
            }
            auto& list = pending_strategies_[source];
            if (std::find(list.begin(), list.end(), name) == list.end()) {
                list.push_back(name);
                send_process_status(name, ChildState::Starting, 0,
                                    "waiting for md source " + source);
                SPDLOG_INFO("strategy pending (md not ready) | strategy={} source={}", name,
                            source);
            }
        }
    }
}

void ProcessSupervisor::start_strategies_for_source(const std::string& source) {
    // 启动绑定 source 且未在运行的策略 (pre-register reader + spawn)
    for (const auto& name : strategies_bound_to(source)) {
        if (find_child(name)) {
            continue;  // 已在运行
        }
        const auto* entry = registry_.find(name);
        if (!entry) {
            continue;
        }
        try {
            pre_register_strategy_reader(name, source);
            (void)launch_child(*entry);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to start strategy | strategy={} error=\"{}\"", name, e.what());
        }
    }
}

void ProcessSupervisor::on_md_channel_ready(const std::string& source) {
    // 取消该源 ready watchdog (收到对应 NOTIFY_MD_STARTED)
    if (auto it = md_ready_timers_.find(source); it != md_ready_timers_.end()) {
        it->second->cancel();
        md_ready_timers_.erase(it);
    }
    // 启动该源全部 pending 策略并清空 pending
    auto it = pending_strategies_.find(source);
    if (it == pending_strategies_.end()) {
        return;
    }
    const auto pending_names = it->second;  // 拷贝: launch_child 回调可能改 pending
    pending_strategies_.erase(it);
    for (const auto& name : pending_names) {
        const auto* entry = registry_.find(name);
        if (!entry) {
            continue;
        }
        try {
            pre_register_strategy_reader(name, source);
            (void)launch_child(*entry);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to start pending strategy | strategy={} error=\"{}\"", name,
                         e.what());
        }
    }
}

bool ProcessSupervisor::is_pending_strategy(const std::string& name, std::string& message) const {
    for (const auto& [source, names] : pending_strategies_) {
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            message = "waiting for md source " + source;
            return true;
        }
    }
    return false;
}

void ProcessSupervisor::shutdown() {
    if (shutting_down_) { return; }  // 幂等
    shutting_down_ = true;
    SPDLOG_INFO("shutdown started");

    // 取消所有单进程停止定时器与待处理的重启定时器 (整体关闭期间抑制自动重启)
    for (auto& [n, timer] : single_stop_timers_) {
        if (timer) timer->cancel();
    }
    single_stop_timers_.clear();
    for (auto& [name, timer] : restart_timers_) {
        timer->cancel();
    }
    restart_timers_.clear();
    restart_counts_.clear();
    // 取消 md ready watchdog + 清空 pending (整体关闭后不再补启动, master 重启后重新编排)
    for (auto& [source, timer] : md_ready_timers_) {
        if (timer) timer->cancel();
    }
    md_ready_timers_.clear();
    pending_strategies_.clear();

    // 冻结关闭批次 (逆序: 策略 → 交易 → 行情 → dzweb)
    std::vector<std::pair<std::string, Category>> running;
    for (auto& child : children_) {
        if (child && child->state() != ChildState::Stopped) {
            running.emplace_back(child->name(), child->entry().category);
        }
    }
    shutdown_batches_ = build_shutdown_batches(running);
    shutdown_batch_index_ = 0;

    if (shutdown_batches_.empty()) {
        // 无子进程或全部已停止，立即完成
        check_shutdown_complete();
        return;
    }
    send_current_shutdown_batch();
}

void ProcessSupervisor::send_current_shutdown_batch() {
    // 逐批推进: 批内有成员运行则定向 SHUTDOWN + 排批超时;
    // 批已全停止 (期间自然退出) 则直接看下一批, 最终 check_shutdown_complete
    while (shutdown_batch_index_ < shutdown_batches_.size()) {
        const auto& batch = shutdown_batches_[shutdown_batch_index_];
        bool any_running = false;
        for (const auto& name : batch) {
            auto child = find_child(name);
            if (child && child->state() != ChildState::Stopped) {
                any_running = true;
                try {
                    shm_mgr_.send_shutdown(name);
                } catch (const std::exception& e) {
                    SPDLOG_WARN(
                        "send_shutdown failed, will force terminate on timeout | name={} error=\"{}\"",
                        name, e.what());
                }
                send_process_status(name, ChildState::Stopping, static_cast<int>(child->pid()));
            }
        }
        if (any_running) {
            SPDLOG_INFO("shutdown batch {}/{} stopping | members={}",
                        shutdown_batch_index_ + 1, shutdown_batches_.size(), batch.size());
            // 批次逐批串行, 最坏 N 批 × single_stop_timeout_sec_ (默认 3s, 四类批次最坏 12s)。
            // main.cpp 信号处理器中的 10s 硬超时仍是最终兜底: 硬超时回调先
            // force_terminate_all 强杀剩余子进程 (消除逐批超时后的尾批孤儿),
            // 再 release_all + ioc.stop。属既有安全网, 仅补强杀不改变语义。
            // 批次超时定时器: 超时强制终止仍在运行的批次成员;
            // 取消由 advance_shutdown_batch 在批次完成时触发 (事件驱动, 无轮询)
            // 捕获排定下标: 防"旧 timer 已过期入队、批次已推进"时误杀新批次 (自检 P1)
            auto armed_index = shutdown_batch_index_;
            shutdown_timer_ = std::make_unique<boost::asio::steady_timer>(
                ioc_, std::chrono::seconds(single_stop_timeout_sec_));
            shutdown_timer_->async_wait([this, armed_index](const boost::system::error_code& ec) {
                if (ec) return;  // 批次及时完成, 定时器被取消
                force_terminate_batch(armed_index);
            });
            return;
        }
        ++shutdown_batch_index_;
    }
    check_shutdown_complete();
}

void ProcessSupervisor::force_terminate_batch(size_t armed_index) {
    // 校验排定下标: 过期 timer 在批次已推进后触发时, 不得误杀新批次 (自检 P1)
    if (armed_index != shutdown_batch_index_ || armed_index >= shutdown_batches_.size()) {
        return;
    }
    for (const auto& name : shutdown_batches_[armed_index]) {
        auto child = find_child(name);
        if (child && child->state() != ChildState::Stopped) {
            SPDLOG_WARN("shutdown batch timeout, force terminating | name={}", name);
            // 通过 notify_ui 反馈用户 (popup=false: 警告级别, 不弹窗只 toast)
            // 与单进程停止路径 on_single_stop_timeout 的强杀反馈对称 (自检 A-偏离1)
            notify_ui_.warn(std::string("shutdown batch timeout, force killing | name=") + name);
            force_killed_names_.insert(name);  // 跳过退出时的 crash 弹窗
            try {
                child->terminate();
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("force terminate failed | name={} error=\"{}\"", name, e.what());
            }
        }
    }
}

void ProcessSupervisor::advance_shutdown_batch() {
    // 仅整体关闭期间有意义: 当前批次全部退出后推进下一批
    if (!shutting_down_ || shutdown_batch_index_ >= shutdown_batches_.size()) {
        return;
    }
    for (const auto& name : shutdown_batches_[shutdown_batch_index_]) {
        auto child = find_child(name);
        if (child && child->state() != ChildState::Stopped) {
            return;  // 批次仍有成员未退出
        }
    }
    if (shutdown_timer_) {
        shutdown_timer_->cancel();
        shutdown_timer_.reset();
    }
    ++shutdown_batch_index_;
    send_current_shutdown_batch();
}

void ProcessSupervisor::force_terminate_all() {
    // 二次 Ctrl+C / 硬超时路径: 强制终止所有仍在运行的子进程, 避免孤儿
    // 与 shutdown() 的超时强制终止逻辑一致, 但不发送 SHUTDOWN, 不启超时定时器
    // 信号处理上下文 / 硬超时回调调用, 每个子进程独立 try-catch 避免一个失败影响其他
    for (auto& child : children_) {
        if (child && child->state() != ChildState::Stopped) {
            try {
                SPDLOG_WARN("force terminating | name={}", child->name());
                child->terminate();
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("force terminate failed | name={} error=\"{}\"",
                                child->name(), e.what());
            }
        }
    }
}

void ProcessSupervisor::set_shutdown_callback(ShutdownCallback cb) {
    shutdown_callback_ = std::move(cb);
}

bool ProcessSupervisor::is_shutting_down() const {
    return shutting_down_;
}

bool ProcessSupervisor::start_process(std::string_view name) {
    // 整体关闭期间拒绝启动: 新进程不在冻结批次内, 关闭会悬停至硬超时并留孤儿
    // (与 stop_process 的 shutting_down_ 守卫对称, 自检 B-P3 / C-问题2)
    if (shutting_down_) {
        SPDLOG_WARN("start_process ignored, full shutdown in progress | name={}", name);
        return false;
    }

    const auto* entry = registry_.find(name);
    if (entry) {
        // json 声明的进程: launch_child 内部按需扫描填充 exe
        return launch_registered_process(*entry);
    }
    // find 未命中: 进程未在 json 声明, 仍尝试实时扫描启动
    // (PROCESS_CONTROL start 添加新行情源场景: dzweb 点击添加 -> master 启动 -> 写 json)
    const auto* scanned = registry_.find_exe_by_stem(name);
    if (!scanned) {
        // 失败路径 B: 扫描未找到 exe
        // 与 launch_child 失败路径 B 对齐: 推 crashed 状态清除前端 startPending,
        // erase restart_counts_ 避免状态泄漏 (与失败路径 D 同理, exe 不存在属于已知不可启动)
        std::string err = std::format("process exe not found | name={}", name);
        SPDLOG_ERROR("{}", err);
        send_process_status(std::string(name), ChildState::Crashed, 0, err);
        notify_ui_.error(err);
        restart_counts_.erase(std::string(name));
        return false;
    }
    return launch_registered_process(*scanned);
}

bool ProcessSupervisor::launch_registered_process(const ProcessEntry& entry) {
    // 策略: 绑定源未 ready 则进 pending (契约 4.6 运行期策略动态启动)。
    // 空 md_source 属测试/手工构造的免 md 策略 (生产配置解析保证非空), 直接启动。
    if (entry.category == Category::Strategy && !entry.md_source.empty()) {
        const auto& source = entry.md_source;
        const auto* state = shm_mgr_.md_channel_state(source);
        const bool source_ready = state && state->status == MdChannelStatus::Running &&
                                  state->ready && state->meta;
        if (!source_ready) {
            // 绑定源不存在或未 ready: 加入 pending, 反馈"等待行情源"
            auto& list = pending_strategies_[source];
            if (std::find(list.begin(), list.end(), entry.name) == list.end()) {
                list.push_back(entry.name);
            }
            const std::string msg = "waiting for md source " + source;
            send_process_status(entry.name, ChildState::Starting, 0, msg);
            SPDLOG_INFO("strategy pending (md not ready) | strategy={} source={}", entry.name,
                        source);
            return false;
        }
        // 源 ready: pre-register reader + spawn
        try {
            pre_register_strategy_reader(entry.name, source);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("pre-register strategy reader failed | strategy={} error=\"{}\"",
                         entry.name, e.what());
        }
    }
    return launch_child(entry);
}

void ProcessSupervisor::stop_process(std::string_view name) {
    if (shutting_down_) {
        SPDLOG_WARN("stop_process ignored, full shutdown in progress | name={}", name);
        return;
    }

    auto child = find_child(name);
    if (!child) {
        SPDLOG_WARN("child not found | name={}", name);
        return;
    }
    if (child->state() == ChildState::Stopped) {
        SPDLOG_INFO("child already stopped | name={}", name);
        return;
    }

    stop_single_child(std::move(child));
}

void ProcessSupervisor::cancel_pending_restart(const std::string& name) {
    // 移除/显式停止时取消挂起的退避重启定时器 (进程不运行路径):
    // 否则定时器到期会复活已移除的源 (重建通道+spawn), 违反"remove 不重启"契约
    if (auto it = restart_timers_.find(name); it != restart_timers_.end()) {
        it->second->cancel();
        restart_timers_.erase(it);
        SPDLOG_INFO("pending restart cancelled | name={}", name);
    }
    restart_counts_.erase(name);
}

void ProcessSupervisor::stop_single_child(std::shared_ptr<ChildProcess> child) {
    const auto& name = child->name();

    // 1. 定向发送 SHUTDOWN
    try {
        shm_mgr_.send_shutdown(name);
        SPDLOG_INFO("shutdown sent to single child | name={}", name);
    } catch (const std::exception& e) {
        SPDLOG_WARN("send_shutdown failed, will force terminate on timeout | name={} error=\"{}\"",
                    name, e.what());
    }

    // 推送 PROCESS_STATUS: stopping
    // 在 single_stop_timers_ 写入之前推送, 避免状态推送异常影响单进程停止流程
    // (send_process_status 内部 catch 异常, 不会抛, 此处只是防御性顺序)
    send_process_status(name, ChildState::Stopping, static_cast<int>(child->pid()));

    // 2. 启动超时定时器 (按 name 索引, 支持多并发 stop)
    // 旧实现: single_stop_timer_ 是单值, 第二次 stop 会销毁第一次的 timer,
    //         第一个进程若卡死则永远无法 force-kill (变僵尸)。
    // 现实现: 每个 name 独立持有 timer, 互不干扰。
    // 若同名 stop 重复调用 (理论上不应发生), 旧 timer 被 unique_ptr 析构 cancel,
    // 其回调以 ec=aborted 触发后走 on_single_stop_timeout 的 ec 分支 (无副作用)。
    auto timer = std::make_unique<boost::asio::steady_timer>(
        ioc_, std::chrono::seconds(single_stop_timeout_sec_));
    auto* raw_timer = timer.get();
    // 注意: 不能用引用捕获 name, 这里 name 是 const ref 到 child->name();
    // timer 回调可能在 child 析构后触发, 故拷贝 name 到回调
    std::string name_copy = name;
    raw_timer->async_wait(
        [this, child_ptr = child, name_copy](const boost::system::error_code& ec) {
            on_single_stop_timeout(std::move(child_ptr), ec, name_copy);
        });
    single_stop_timers_[name] = std::move(timer);
}

void ProcessSupervisor::on_single_stop_timeout(
        std::shared_ptr<ChildProcess> child,
        const boost::system::error_code& ec,
        const std::string& name) {
    if (ec) {
        SPDLOG_INFO("single stop timer cancelled, child exited gracefully | name={}", name);
        // 旧实现: single_stop_target_.clear() — 单值, 会清掉所有 stop 跟踪
        // 现实现: 仅 erase 自己的 entry (其他并发 stop 的 timer 不受影响)
        single_stop_timers_.erase(name);
        return;
    }

    // 失败路径 C: 子进程停止超时, 强制 kill
    // 流程契约: 主进程打 WARN 日志记录强制终止, 继续走步骤 6-9 完成删除
    // (on_child_exit 会在 async_wait 回调中触发, 推送 stopped/crashed 让前端移除卡片)
    if (child->state() != ChildState::Stopped) {
        SPDLOG_WARN("single stop timeout, force terminating | name={}", child->name());
        // 通过 notify_ui 反馈用户 (popup=false: 警告级别, 不弹窗只 toast)
        notify_ui_.warn(std::string("process stop timeout, force killing | name=") + child->name());
        // 标记 force_killed, 让随后触发的 on_child_exit 跳过 crash 分支的 notify_ui
        // (避免双弹窗: "force killed" + "crashed" — 第二个文案与实际场景不符)
        // 注意: 不跳过 send_process_status / unregister_subscriber / remove_gateway_section
        force_killed_names_.insert(child->name());
        child->terminate();
    }
    // 超时分支同样需要 erase 自己的 entry (无论是否 force kill, 此 timer 已结束)
    single_stop_timers_.erase(name);
}

std::shared_ptr<ChildProcess> ProcessSupervisor::find_child(std::string_view name) {
    for (auto& child : children_) {
        if (child && child->name() == name) { return child; }
    }
    return nullptr;
}

const std::vector<ProcessEntry>& ProcessSupervisor::registry_entries() const {
    return registry_.entries();
}

const ProcessEntry* ProcessSupervisor::find_registry_entry(std::string_view name) const {
    return registry_.find(name);
}

const ProcessEntry* ProcessSupervisor::find_exe_by_stem(std::string_view name) const {
    return registry_.find_exe_by_stem(name);
}

void ProcessSupervisor::register_gateway(ProcessEntry entry) {
    registry_.register_gateway(std::move(entry));
}

void ProcessSupervisor::update_display_name(std::string_view name,
                                             const std::string& display_name) {
    if (registry_.update_display_name(name, display_name)) {
        SPDLOG_INFO("registry display_name updated | name={} display_name={}",
                    name, display_name);
    }
}

void ProcessSupervisor::update_entry_config(const std::string& name,
                                            const std::vector<std::string>& args,
                                            const std::unordered_map<std::string, std::string>& env,
                                            const platform::RestartPolicy& restart,
                                            const std::string& display_name) {
    // 类型统一: ProcessEntry.restart 与 platform::RestartPolicy 同类型, 直接透传
    registry_.update_entry(name, args, env, restart, display_name);
    SPDLOG_INFO("registry entry config updated | name={}", name);
}

void ProcessSupervisor::unregister_entry(const std::string& name) {
    registry_.unregister(name);
    SPDLOG_INFO("registry entry unregistered | name={}", name);
}

void ProcessSupervisor::send_process_status(const std::string& name,
                                             ChildState state,
                                             int pid,
                                             const std::string& message,
                                             std::optional<platform::ProcessEvent> event) {
    platform::ProcessStatus status;
    status.name = name;
    // ChildState 已为 platform:: 类型 (child_process.h alias), 直接赋值
    status.state = state;
    status.pid = pid;
    status.message = message;
    // display_name 从 store 读取 (契约 process, store 为配置真相源)
    // 这样所有 send_process_status 调用 (launch_child / on_child_exit / stop_single_child)
    // 都能自动携带 display_name, 无需在每个调用点显式传参
    status.display_name = shm_mgr_.display_name_of(name);
    status.event = event;
    shm_mgr_.write_process_status(status);
}

void ProcessSupervisor::mark_remove_pending(const std::string& name) {
    remove_pending_.insert(name);
}

bool ProcessSupervisor::consume_remove_pending(const std::string& name) {
    auto it = remove_pending_.find(name);
    if (it == remove_pending_.end()) return false;
    remove_pending_.erase(it);
    return true;
}

void ProcessSupervisor::notify_removed_for_inactive(const std::string& name, Category category) {
    // 兜底路径: 子进程不在运行时 (on_child_exit 不会被触发),
    // 由 supervisor 完成 remove 流程剩余步骤 (删配置 + 清订阅者 + 推 stopped + consume_remove_pending)
    // 行为与原 shm_manager.cpp 中内联兜底逻辑近似等价: 层次划分调整 (shm_manager 调用 supervisor,
    // supervisor 调用 shm_mgr_ 完成具体操作), 并新增失败路径 D 的 notify_ui 反馈
    SPDLOG_INFO("remove: child not running, finalize immediately | name={}", name);
    // 取消挂起的退避重启定时器: 否则崩溃退避窗口内的 Remove 会被定时器"复活"
    // (launch_child 重建通道 + spawn 已删源), 违反"remove 不重启"契约
    cancel_pending_restart(name);
    try {
        if (category == Category::Strategy) {
            remove_strategy_section(shm_mgr_.config_path(), name);
        } else {
            remove_gateway_section(shm_mgr_.config_path(), category, name);
        }
        SPDLOG_INFO("process config section removed | name={} path={}",
                    name, shm_mgr_.config_path().string());
    } catch (const std::exception& e) {
        // 失败路径 D: 配置段删除失败 -> notify_ui 反馈
        // 契约要求: 继续完成订阅者清理, 不重启子进程 (删除是用户意图)
        // 契约 process: json 操作失败必须打 ERROR 日志并通知 UI
        SPDLOG_ERROR("failed to remove gateway section | name={} error=\"{}\"", name, e.what());
        notify_ui_.error(std::string("failed to remove gateway config | name=") + name
                        + " error=" + e.what());
    }
    // Remove 流程: 对 md 源标记 tombstone (契约 4.7: 不删文件、不清空 readers 表,
    // 保留 meta 供 PageCleaner 清理与同源重加复用)。不再 destroy_md_channel。
    if (category == Category::GatewayMd) {
        try {
            shm_mgr_.tombstone_md_channel(name);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to tombstone md channel | name={} error=\"{}\"", name, e.what());
        }
    }
    if (category == Category::GatewayTd) {
        // 契约 account-status: remove 三路径统一走 notify_td_stopped —— 推 Offline
        // (gateway_name=真实网关名) 后清镜像, 与 on_child_exit 退出路径语义一致
        shm_mgr_.notify_td_stopped(name);
    }
    // 清理订阅者注册 (子进程可能曾注册过, 退出后残留)
    // remove_reader 是幂等的, 不存在也无害
    shm_mgr_.remove_reader(name);
    // 推送 Stopped 状态让前端移除卡片 (契约 process: 未运行 pid=0; event 缺失=自发)
    platform::ProcessStatus st{};
    st.name = name;
    st.state = ChildState::Stopped;
    st.pid = 0;
    st.message = "removed (process not running)";
    shm_mgr_.write_process_status(st);
    // 消费 remove_pending (避免集合累积)
    consume_remove_pending(name);
}

const std::vector<std::shared_ptr<ChildProcess>>& ProcessSupervisor::children() const {
    return children_;
}

bool ProcessSupervisor::launch_child(const ProcessEntry& entry) {
    // 防止同名进程重复启动
    auto existing = find_child(entry.name);
    if (existing && existing->state() != ChildState::Stopped) {
        // 失败路径 B: 进程已在运行, 推送 PROCESS_STATUS running + notify_ui
        // - send_process_status: state=running 触发前端 tryPromotePending 晋升卡片 (C-1 修复)
        //   并清 startPending; 语义与实际进程状态一致, 避免误导用户为"已崩溃"
        // - notify_ui: 弹 toast 反馈"进程已在运行", 让用户知道本次启动未真正发生
        SPDLOG_ERROR("process already running, skipping | name={}", entry.name);
        const std::string err_msg = "process already running | name=" + entry.name;
        send_process_status(entry.name, ChildState::Running, static_cast<int>(existing->pid()), err_msg);
        notify_ui_.error(err_msg, false);
        return false;
    }

    // exe 为空时实时扫描填充 (json 不写 exe 字段, 由 find_exe_by_stem 提供)
    // 扫描结果通过 entry_to_launch 拷贝传递, 不回写 registry (避免缓存过期)
    ProcessEntry entry_to_launch = entry;
    if (entry_to_launch.exe.empty()) {
        const auto* scanned = registry_.find_exe_by_stem(entry.name);
        if (!scanned) {
            // 失败路径 B: 扫描未找到 exe
            std::string err_msg = std::format("process exe not found | name={}", entry.name);
            SPDLOG_ERROR("{}", err_msg);
            send_process_status(entry.name, ChildState::Crashed, 0, err_msg);
            notify_ui_.error(err_msg);
            restart_counts_.erase(entry.name);
            return false;
        }
        entry_to_launch.exe = scanned->exe;
        entry_to_launch.start_dir = scanned->start_dir;
        // args/restart/display_name 仍以 json 配置为准 (不覆盖)
    }

    // 启动 md 进程前先创建/重建 md 通道 (含冷启动 clear_readers / page_size 分派;
    // 关闭期间人工修改的 page_size 经 open_or_create 自动重置生效)。
    // 可能抛异常, 由调用方 catch (start_one / handle_process_start / schedule_restart 均有 try)。
    if (entry_to_launch.category == Category::GatewayMd) {
        shm_mgr_.create_md_channel(entry_to_launch.name);
    }

    // 策略进程注入 DZTRADER_MD_SOURCE (契约 4.2): 显式覆盖, 避免继承 master 环境污染。
    // 由 child_process.cpp 的环境合并逻辑合并进子进程环境。
    // 空 md_source 属测试/手工构造的免 md 策略 (生产配置解析保证非空): 跳过注入。
    if (entry_to_launch.category == Category::Strategy) {
        if (entry_to_launch.md_source.empty()) {
            SPDLOG_WARN("strategy launched without md_source (manual/test entry) | name={}",
                        entry_to_launch.name);
        } else {
            entry_to_launch.env["DZTRADER_MD_SOURCE"] = entry_to_launch.md_source;
            SPDLOG_INFO("strategy env injected | name={} DZTRADER_MD_SOURCE={}",
                        entry_to_launch.name, entry_to_launch.md_source);
        }
    }

    auto child = ChildProcess::create(ioc_, entry_to_launch);

    boost::system::error_code ec;
    if (!child->start(ec)) {
        // 失败路径 D: spawn 失败
        // 通过 PROCESS_STATUS state=crashed 反馈给前端 (setProcessStatus 会清 startPending)
        // 通过 notify_ui 给出具体错误信息 (setNotifyUi 兜底清 startPending + 弹 toast)
        // 内部 SPDLOG 与对外 notify_ui 共用同一 err_msg, 便于日志检索与关联
        std::string err_msg = std::format("spawn child failed | name={} error=\"{}\"",
                                          entry.name, dztrader::to_utf8_from_system(ec.message()));
        SPDLOG_ERROR("{}", err_msg);
        send_process_status(entry.name, ChildState::Crashed, 0, err_msg);
        notify_ui_.error(err_msg);
        // I1 修复: spawn 失败时不安排 restart 定时器
        // (避免持续重试已知无法启动的进程, 浪费资源并产生噪声日志;
        //  仅 on_child_exit 路径才走 restart 策略, 因此时子进程已成功 spawn 过)
        restart_counts_.erase(entry.name);  // 清理状态泄漏, 防止下次启动时残留计数
        return false;
    }

    // 启动成功 — 取消待处理的重启定时器 (用户手动启动场景) + 重置重启计数
    // 场景: 子进程崩溃 → schedule_restart 设置 restart_timers_[name] → 用户手动 start_process
    //       → launch_child 成功 → 此处取消定时器, 避免定时器回调误判 "already running"
    if (auto it = restart_timers_.find(entry.name); it != restart_timers_.end()) {
        it->second->cancel();
        restart_timers_.erase(it);
        SPDLOG_INFO("cancelled pending restart timer for {} (manual start)", entry.name);
    }
    restart_counts_.erase(entry.name);

    // 注册到 OrphanGuard (用 entry_to_launch.exe, 含扫描后的路径)
    orphan_guard_.register_child(child->pid(), entry.name, entry_to_launch.exe);

    // 注册子进程为事件通道订阅者 + 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧
    // 子进程启动后会通过 Reader::create → sync_from_channel_meta 获取最新订阅者列表，
    // 并在 poll event channel 时收到 UPDATE_SHM_EVENT_SUBSCRIBER 帧调 refresh_subscribers 刷新本地缓存
    auto sub_name = make_subscriber_name(entry_to_launch);
    shm_mgr_.register_subscriber(sub_name, static_cast<uint64_t>(child->pid()));

    // 设置退出回调 — 捕获 shared_ptr 保持子进程存活
    child->async_wait(
        [this, child](boost::system::error_code exit_ec, int exit_code) {
            on_child_exit(child, exit_ec, exit_code);
        });

    // 推送 PROCESS_STATUS: running
    // 必须在 std::move(child) 之前捕获 name 和 pid, move 后 child 为空
    auto launched_name = child->name();
    auto launched_pid = static_cast<int>(child->pid());
    send_process_status(launched_name, ChildState::Running, launched_pid);

    // 每次 spawn md 进程后挂 ready watchdog (契约 4.5): 覆盖启动编排 / 手动 start /
    // restart policy 全部 spawn 路径。收到对应 NOTIFY_MD_STARTED 时由
    // on_md_channel_ready 取消; 超时按"是否存在运行中绑定策略"分流处理。
    if (entry_to_launch.category == Category::GatewayMd) {
        arm_md_ready_watchdog(launched_name);
    }

    children_.push_back(std::move(child));
    return true;
}

void ProcessSupervisor::on_child_exit(std::shared_ptr<ChildProcess> child,
                                       [[maybe_unused]] boost::system::error_code ec,
                                       int exit_code) {
    if (!child) { return; }

    auto name = child->name();
    auto pid = child->pid();

    // 流程契约 (02-remove-market-source.md) 步骤 5-8:
    //   5. 主进程等待子进程退出回调 (async_wait) — 此处即回调入口
    //   6. 删除 dztraderd.json [md.<name>]/[td.<name>] 配置段 (若 remove_pending)
    //   7. 清理订阅者注册
    //   8. 推送状态增量 (Stopped/Crashed) 让前端移除卡片
    // 顺序: 先删配置 (步骤 6), 再清理订阅者 (步骤 7), 最后推状态 (步骤 8) - 严格遵守契约
    // 每个步骤独立 try-catch: 单步骤失败不阻塞其他步骤 (失败路径 D)
    ChildState exit_state = (exit_code == 0) ? ChildState::Stopped : ChildState::Crashed;
    std::string exit_msg = (exit_code != 0)
        ? std::format("exit_code={}", exit_code) : "";

    try {
        // 从 OrphanGuard 注销
        orphan_guard_.unregister_child(pid);

        // 从 children_ 中移除已停止的子进程，防止累积
        remove_child(child);

        // 取消该 name 对应的单进程停止定时器 (若存在)
        // 子进程正常/异常退出后, 超时定时器不再需要 (避免误触 force kill)
        // 旧实现: 比较 single_stop_target_ == name (单值), 无法跟踪并发 stop
        // 现实现: contains 检查 map, 命中则 cancel + erase
        if (auto it = single_stop_timers_.find(name); it != single_stop_timers_.end()) {
            it->second->cancel();
            single_stop_timers_.erase(it);
        }

        // 步骤 6: 若属于 remove 流程, 删除 dztraderd.json [md.<name>]/[td.<name>] 段
        // (流程契约要求: 删除配置在 on_child_exit 中执行, 不能同步在 stop_process 之后)
        bool is_remove_flow = consume_remove_pending(name);
        // 移除窗口内同名源被重新添加(registry 条目重现): 用户意图已翻转为保留,
        // 跳过移除收尾的配置段删除与通道销毁 —— 反删刚写回的配置会造成
        // json/registry/store 三态不一致, master 重启后该源永久丢失
        const bool remove_flow_re_added =
            is_remove_flow && registry_.find(name) != nullptr;
        if (is_remove_flow && !remove_flow_re_added) {
            const auto& cfg_path = shm_mgr_.config_path();
            // category 取 child->entry(): registry 条目已被 store remove 的 apply
            // 回调删除, find 恒 miss, 兜底值会对 td 目标查错段 (BUG-3.1)
            const Category category = child->entry().category;
            try {
                // Strategy 条目持久化在 strategy 数组 section, 不能走 gateway 段删除
                // (否则会误写 td.<name> 段)
                if (category == Category::Strategy) {
                    remove_strategy_section(cfg_path, name);
                } else {
                    remove_gateway_section(cfg_path, category, name);
                }
                SPDLOG_INFO("process config section removed (on remove flow) | name={} path={}",
                            name, cfg_path.string());
            } catch (const std::exception& e) {
                // 失败路径 D: 配置段删除失败 -> notify_ui 反馈前端
                // 契约要求: 继续完成订阅者清理 (步骤 7) 与推状态 (步骤 8), 不重启子进程 (删除是用户意图)
                // 契约 process: json 操作失败必须打 ERROR 日志并通知 UI
                SPDLOG_ERROR("failed to remove gateway section | name={} error=\"{}\"",
                            name, e.what());
                notify_ui_.error(std::string("failed to remove gateway config | name=") + name
                                + " error=" + e.what());
            }
            if (category == Category::GatewayTd) {
                // 契约 account-status: remove 流程统一走 notify_td_stopped ——
                // 推 Offline (remove 也必须让策略感知) + 清镜像。
                // 后续 on_child_exit 尾部 GatewayTd 分支再调 notify_td_stopped 时
                // 镜像已清, 幂等 no-op (契约 Offline 恒推, 重复无害)
                shm_mgr_.notify_td_stopped(name);
            }
        }

        // 步骤 7: 注销事件通道订阅者 + 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧
        // 无论退出原因（正常退出/崩溃/启动失败/主动终止），都统一注销
        // 独立 try-catch: 步骤 6 失败不应阻塞订阅者清理 (失败路径 D)
        const auto sub_name = make_subscriber_name(child->entry());
        try {
            shm_mgr_.unregister_subscriber(sub_name);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to unregister subscriber | name={} pid={} error=\"{}\"",
                        name, pid, e.what());
        }

        // 步骤 7b: 注销该进程在所有 md 通道的读者条目 (策略经帧 1013 自注册) +
        // 通知各 md 进程刷新订阅者缓存 (防止继续向死信号量 notify)。
        // remove_reader 对缺失 key 幂等; 网关类进程从未注册, 循环为 no-op。
        try {
            shm_mgr_.remove_reader_from_all_md_channels(sub_name);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to remove md reader | name={} error=\"{}\"", name, e.what());
        }

        // 步骤 8: 推送 PROCESS_STATUS: stopped (exit_code==0) 或 crashed
        // 覆盖所有退出路径 (shutting_down/正常退出/重启禁用/达上限), 故统一在最后推
        // 独立 try-catch: 步骤 7 失败不应阻塞状态推送
        try {
            send_process_status(name, exit_state, static_cast<int>(pid), exit_msg);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("failed to send process status | name={} pid={} error=\"{}\"",
                        name, pid, e.what());
        }

        // md 进程退出时执行通道生命周期 (dztraderd 架构 + 设计 spec 移除清理):
        // - Remove 流程: 配置已删, 标记 tombstone (保留文件与 readers, PageCleaner
        //   继续覆盖; 不再 destroy_md_channel —— 物理清理仅留给未来主动维护入口)
        // - 停止/崩溃 (待重启): 标记 Stopped (保留 meta/readers, 文件待重启复用)
        // - 移除窗口内重加: 按正常停止处理 (close)
        // 两者统一由主进程执行; 随后代发 NOTIFY_MD_STOPPED
        // (崩溃时 dzmd_ctp 自己无法广播, 由 master 代发; 正常停止时也需通知)
        // 独立 try-catch: 不阻塞后续崩溃通知/重启逻辑
        if (child->entry().category == Category::GatewayMd) {
            try {
                if (is_remove_flow && !remove_flow_re_added) {
                    shm_mgr_.tombstone_md_channel(name);
                } else {
                    shm_mgr_.close_md_channel(name);
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("failed to close/tombstone md channel | name={} error=\"{}\"",
                             name, e.what());
            }
            try {
                platform::write_ext_inst_raw(shm_mgr_.event_writer(), DZ_FRAME_NOTIFY_MD_STOPPED, name);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("failed to broadcast md_service_stopped | name={} error=\"{}\"",
                             name, e.what());
            }
        }

        if (child->entry().category == Category::GatewayTd) {
            // 契约 account-status: td 退出 (崩溃/停止/remove) 统一推 Offline + 清镜像
            // (notify_td_stopped 内部先推后清; remove 分支上方已推过时镜像已清, 此处 no-op)。
            // 崩溃时 td 自身无法广播, master 代推 (与 GatewayMd 代发 NOTIFY_MD_STOPPED 对称)。
            // shutting_down 时全局退出, 免推 (策略进程同样在退)。
            try {
                if (!shutting_down_) {
                    shm_mgr_.notify_td_stopped(name);
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("failed to notify td stopped | name={} error=\"{}\"",
                             name, e.what());
            }
        }

        // 子进程崩溃 (exit_code != 0) 时, 前端 setProcessStatus 只清 startPending,
        // 不弹 toast。流程契约要求"所有失败路径必须 UI 弹窗", 故崩溃时追加 notify_ui。
        // (与失败路径 D 的 notify_ui 对称, D 在本函数步骤 6 失败时已发)
        // 跳过 shutting_down 场景: 整体关闭时弹窗无意义, 日志已记录
        // 跳过 force_killed 场景: on_single_stop_timeout 已发 "force killing" notify_ui,
        // 此处再发 "crashed" 会造成双弹窗且文案不符 (实为 master 强制 kill, 非崩溃)
        bool was_force_killed = force_killed_names_.count(name) > 0;
        if (exit_code != 0 && !shutting_down_ && !was_force_killed) {
            std::string crash_msg = std::format(
                "child crashed | name={} pid={} exit_code={}", name, pid, exit_code);
            notify_ui_.error(crash_msg);
        }
        // 清理 force_killed 标记 (此处之后该 name 不再需要 force_killed 状态)
        // erase 对不存在的 key 是 no-op, 故所有路径 (force_killed/crash/正常退出) 均安全
        force_killed_names_.erase(name);

        if (shutting_down_) {
            SPDLOG_INFO("child exited during shutdown | name={} exit_code={}", name, exit_code);
            // 批次完成则推进下一批; 推进链路 (advance -> send_current -> 末批完成)
            // 内部必达 check_shutdown_complete。此处不再显式调 check:
            // 当前批未完成时 check 必为 no-op (批成员仍在运行),
            // 完成时显式 check 会与链路内的 check 双触发关闭回调
            // (release_all/orphan_guard.cleanup 被执行两次)
            advance_shutdown_batch();
            return;
        }

        // remove 流程不重启 (即使 crashed), 删除是用户意图
        if (is_remove_flow) {
            SPDLOG_INFO("child exited on remove flow, not restarting | name={} exit_code={}",
                        name, exit_code);
            restart_counts_.erase(name);
            return;
        }

        // 正常退出（退出码 0）— 不重启，清除重启计数
        if (exit_code == 0) {
            SPDLOG_INFO("child exited normally, not restarting | name={}", name);
            restart_counts_.erase(name);
            return;
        }

        // 检查重启策略（从 registry 取最新配置：SET 变更对后续崩溃处理立即生效，契约 process）
        const auto* latest_entry = registry_.find(name);
        const ProcessEntry& effective = latest_entry ? *latest_entry : child->entry();
        const auto& policy = effective.restart;
        if (!policy.enabled) {
            SPDLOG_WARN("child crashed, restart disabled | name={} exit_code={}", name, exit_code);
            restart_counts_.erase(name);
            return;
        }

        int count = ++restart_counts_[name];
        if (count > policy.max_attempts) {
            SPDLOG_ERROR("max restart attempts reached | name={} exit_code={} count={}",
                         name, exit_code, count);
            restart_counts_.erase(name);
            return;
        }

        // 退避重启
        schedule_restart(name, effective, count);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("on_child_exit error | name={} pid={} error=\"{}\"", name, pid, e.what());
    }
}

void ProcessSupervisor::schedule_restart(const std::string& name,
                                          const ProcessEntry& entry,
                                          int restart_count) {
    auto backoff = entry.restart.backoff_sec * restart_count;
    // 钳制最小 1 秒, 防止 backoff_sec=0 导致快速重启循环 (毫秒级内连续 spawn 失败)
    if (backoff < 1) backoff = 1;

    SPDLOG_INFO("restart scheduled | name={} backoff_sec={} restart_count={}",
                name, backoff, restart_count);

    auto timer = std::make_unique<boost::asio::steady_timer>(
        ioc_, std::chrono::seconds(backoff));

    // 按值捕获 entry 用于重启
    auto entry_copy = entry;
    // 崩溃恢复: 传 --recover 让子进程启动时自动补登
    // 仅网关进程 (dzmd_*/dztd_*) 解析此参数, 其他进程忽略
    entry_copy.args.push_back("--recover");

    timer->async_wait(
        [this, name, entry_copy](const boost::system::error_code& ec) {
            if (ec) {
                // 被 cancel 时不 erase, 避免误删后续 timer:
                // 若 schedule_restart 同名再次调用, 旧 timer T1 被 cancel 后回调以 ec=aborted 触发,
                // 此时 map 中已存新 timer T2, 此处 erase(name) 会误删 T2, 导致 T2 失去跟踪。
                SPDLOG_DEBUG("restart timer cancelled | name={}", name);
                return;
            }
            // 正常触发时才 erase (此时 map 中存的就是本 timer)
            restart_timers_.erase(name);
            if (shutting_down_) {
                SPDLOG_INFO("not restarting during shutdown | name={}", name);
                return;
            }

            try {
                SPDLOG_INFO("restarting child | name={}", name);
                // 重启走 launch_child：成功推 Running；spawn 失败推 Crashed + 终止重启循环
                // （launch_child 失败路径不安排下一次重启，符合契约"重启 spawn 失败终止循环"）
                (void)launch_child(entry_copy);
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("restart failed | name={} error=\"{}\"", name, e.what());
            }
        });

    // 防御性: 若存在旧的重启定时器 (例如 on_child_exit 被重复触发), 先取消再覆盖
    if (auto it = restart_timers_.find(name); it != restart_timers_.end()) {
        it->second->cancel();
        restart_timers_.erase(it);
    }
    restart_timers_[name] = std::move(timer);
}

void ProcessSupervisor::remove_child(const std::shared_ptr<ChildProcess>& child) {
    auto it = std::remove_if(children_.begin(), children_.end(),
        [&child](const std::shared_ptr<ChildProcess>& c) {
            return c.get() == child.get();
        });
    children_.erase(it, children_.end());
}

bool ProcessSupervisor::all_children_stopped() const {
    return std::all_of(children_.begin(), children_.end(),
        [](const std::shared_ptr<ChildProcess>& c) {
            return !c || c->state() == ChildState::Stopped;
        });
}

void ProcessSupervisor::check_shutdown_complete() {
    if (!shutting_down_) { return; }
    if (!all_children_stopped()) { return; }

    // 取消强制终止定时器 — 所有子进程已退出
    if (shutdown_timer_) {
        shutdown_timer_->cancel();
        shutdown_timer_.reset();
    }

    SPDLOG_INFO("all children stopped, shutdown complete");

    if (shutdown_callback_) {
        try {
            shutdown_callback_();
        } catch (const std::exception& e) {
            SPDLOG_CRITICAL("shutdown callback error | error=\"{}\"", e.what());
        }
    }
}

}  // namespace dztrader::master
