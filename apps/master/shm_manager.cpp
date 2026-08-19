#include "shm_manager.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/encoding.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/frame_view.h>

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include "process_supervisor.h"

namespace dztrader::master {

using dztrader::platform::MIN_PAGE_SIZE_BYTES;
using dztrader::platform::mb_to_bytes;

ShmManager::ShmManager(const ShmGlobalConfig& shm_global,
                       const std::filesystem::path& cfg_path,
                       shm::CleanupPolicy cleanup_policy,
                       std::string name)
    : meta_file_size_(shm_global.meta_file_size),
      cleanup_policy_(cleanup_policy),
      name_(name),
      config_path_(cfg_path),
      event_meta_([this, &shm_global]() {
          // event 通道 page size (字节),最小 1MB
          // 此处提前从文件读取 page_size_mb, 因 event_meta_ 创建依赖 page size,
          // 而 event_shm_config_.load() 在构造体内才调用 (依赖 event_writer_)
          uint64_t page_size_mb = 64;  // EventShmConfig 默认 page_size_mb=64
          try {
              if (std::filesystem::exists(config_path_)) {
                  std::ifstream ifs(config_path_);
                  if (ifs) {
                      nlohmann::json full;
                      ifs >> full;
                      if (full.contains("shm") && full["shm"].contains("event") &&
                          full["shm"]["event"].contains("page_size_mb")) {
                          const auto& v = full["shm"]["event"]["page_size_mb"];
                          if (v.is_number_integer() && v.get<int64_t>() >= 1) {
                              page_size_mb = v.get<uint64_t>();
                          }
                      }
                  }
              }
          } catch (const std::exception& e) {
              SPDLOG_WARN("failed to read event page_size_mb, using default | error=\"{}\"",
                          e.what());
          }
          event_page_size_ = mb_to_bytes(page_size_mb);
          if (event_page_size_ < MIN_PAGE_SIZE_BYTES) {
              SPDLOG_WARN("event page_size_mb too small, clamped to 1MB | config={} actual={}",
                          page_size_mb, 1);
              event_page_size_ = MIN_PAGE_SIZE_BYTES;
          }
          const auto& shm_dir = dztrader::paths::shm();
          shm::ChannelConfig event_cfg{
              .channel_name = shm::channel_name("dzevent"),
              .shm_dir = shm_dir,
              .meta_file_size = shm_global.meta_file_size,
              .page_size = event_page_size_,
              .lock_memory = false,
              .prefetch_memory = false,
          };
          auto meta = shm::ChannelMeta::open_or_create(event_cfg);
          SPDLOG_INFO("event channel created | name={} meta_size={} page_size={}",
                      event_cfg.channel_name, shm_global.meta_file_size, event_page_size_);
          return std::make_shared<shm::ChannelMeta>(std::move(meta));
      }()),
      event_writer_(shm::MultiWriter::create(event_meta_, name)),
      event_reader_(shm::Reader::create(event_meta_, name)),
      cleaner_(std::make_unique<shm::PageCleaner>(event_meta_, cleanup_policy_)),
      log_config_(name_, config_path_),
      notify_ui_(name_, event_writer_),
      event_shm_config_(config_path_, event_writer_,
                        nlohmann::json::json_pointer("/shm/event")),
      event_sem_(name) {
    SPDLOG_INFO("event channel writer/reader/cleaner ready");

    // 重置订阅者列表: 清空上次运行遗留的僵尸订阅者,注册 master 自己
    reset_subscribers();

    // 加载日志配置 (从 dztraderd.json "log" section, 失败用默认值自愈, 与 spdlog 同步)
    log_config_.load();

    // 加载 event 通道 SHM 配置 (从 dztraderd.json "shm"."event" 子段, 失败用默认值自愈)
    event_shm_config_.load();

    // 进程配置存储: 注入 persist/apply 回调。初始镜像 load 不在此执行——
    // build_initial_config_map/persist/apply 均经 supervisor_ 访问 registry,
    // supervisor 由 set_supervisor 在构造后注入, 故 load 移到 set_supervisor 中。
    process_config_store_.emplace(
        event_writer_,
        [this](const std::string& name, const nlohmann::json& full) {
            persist_process_config(name, full);
        },
        [this](const std::string& name, const nlohmann::json& full) {
            apply_process_config(name, full);
        });

    // event_page_size_ 已在 event_meta_ 创建 lambda 中从文件读取并赋值,
    // 此处从 event_shm_config_ 再赋一次保证一致性 (同源数据, 值相同)
    event_page_size_ = std::max<uint64_t>(
        event_shm_config_.page_size_mb() * 1024 * 1024,
        dztrader::platform::MIN_PAGE_SIZE_BYTES);
}

void ShmManager::create_md_channel(std::string_view source_name) {
    const auto& shm_dir = dztrader::paths::shm();
    const auto channel_name = shm::channel_name(source_name);

    // 通道存活 (元数据句柄在) 时幂等返回: 运行中的通道不重建
    // (page_size 运行期间不可改, 无需重读配置)
    if (auto it = md_channels_.find(channel_name);
        it != md_channels_.end() && it->second.meta) {
        SPDLOG_INFO("md channel already exists | name={}", channel_name);
        return;
    }

    // page_size 优先级 (新协议无 UI override):
    //   1. read_md_page_size(source_name) (从子进程配置文件读取)
    //   2. kDefaultMdPageSize (1024MB, 与 MdShmConfig 默认值一致)
    // 关闭期间人工修改 page_size 在此生效: open_or_create 发现配置值与现存
    // 不一致时自动重置 (清空页文件 + 写位置归零 + 按新值重建, 通道内建机制)
    uint64_t page_size = kDefaultMdPageSize;
    if (auto ps = read_md_page_size(std::string(source_name))) {
        page_size = *ps;
    } else {
        SPDLOG_ERROR("failed to read md page size, using default | source={}", source_name);
    }

    if (page_size < MIN_PAGE_SIZE_BYTES) {
        SPDLOG_WARN("md page_size too small, clamped to 1MB | source={} actual={}", source_name,
                    MIN_PAGE_SIZE_BYTES);
        page_size = MIN_PAGE_SIZE_BYTES;
    }

    shm::ChannelConfig md_cfg{
        .channel_name = channel_name,
        .shm_dir = shm_dir,
        .meta_file_size = meta_file_size_,
        .page_size = page_size,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    auto md_meta = shm::ChannelMeta::open_or_create(md_cfg);
    auto md_meta_ptr = std::make_shared<shm::ChannelMeta>(std::move(md_meta));

    // 清空订阅者列表: master 不订阅行情通道 (master 不读 tick),
    // 行情通道订阅者由策略/数据进程注册, clear 后为初始空状态
    // (重启重建时兜底清理上一运行残留)
    md_meta_ptr->clear_readers();

    md_channels_[channel_name] = MdChannelState{md_meta_ptr, /*ready=*/false};
    SPDLOG_INFO("md channel created | name={} page_size={} meta_size={}", channel_name, page_size,
                meta_file_size_);
}

void ShmManager::close_md_channel(std::string_view source_name) {
    // 停止后果 (dztraderd 架构「行情进程生命周期」): 清空读者列表 + 释放句柄,
    // 不触碰数据文件/读取位置/page_size (保留待重启复用); 条目保留表示已配置
    // key 与 create_md_channel 保持一致 (shm::channel_name, 当前为恒等变换)
    auto it = md_channels_.find(shm::channel_name(source_name));
    if (it == md_channels_.end()) {
        return;
    }
    if (it->second.meta) {
        it->second.meta->clear_readers();
        it->second.meta.reset();
    }
    it->second.ready = false;
    SPDLOG_INFO("md channel closed | source={}", source_name);
}

void ShmManager::mark_md_channel_ready(std::string_view source_name) {
    auto it = md_channels_.find(shm::channel_name(source_name));
    if (it == md_channels_.end() || !it->second.meta) {
        // 未知行情进程或通道已关闭 (不在 master 编排内), 忽略
        SPDLOG_DEBUG("md started ignored for unknown/closed channel | source={}", source_name);
        return;
    }
    it->second.ready = true;
    SPDLOG_INFO("md channel ready | source={}", source_name);
}

void ShmManager::start_periodic_tasks(boost::asio::io_context& ioc) {
    ioc_ = &ioc;

    // 60s PageCleaner 清理（周期性维护任务，非事件监听轮询）
    cleanup_timer_ = std::make_unique<boost::asio::steady_timer>(ioc, std::chrono::seconds(60));
    auto recycle = std::make_shared<std::function<void(const boost::system::error_code&)>>();
    *recycle = [this, recycle](const boost::system::error_code& ec) {
        if (ec) return;  // 定时器被取消
        try {
            cleanup_old_pages();
        } catch (const std::exception& e) {
            SPDLOG_WARN("cleanup timer callback failed | error=\"{}\"", e.what());
        }
        cleanup_timer_->expires_after(std::chrono::seconds(60));
        cleanup_timer_->async_wait(*recycle);
    };
    cleanup_timer_->expires_after(std::chrono::seconds(60));
    cleanup_timer_->async_wait(*recycle);

    // event channel 信号量监听线程（事件驱动，非轮询）
    // 阻塞在 NamedSemaphore::wait，被 Writer::notify_subscribers 唤醒后
    // post 到 io_context 执行 drain_event_channel，实现零延迟事件驱动
    // 信号量本身有计数特性：notify 在 wait 之前也不丢，无需超时兜底
    // stop_event_thread 通过 notify 唤醒线程检查 stop_flag 并退出
    event_thread_ = std::thread([this]() {
        SPDLOG_INFO("event channel monitor thread started | mode=semaphore-driven");
        while (!stop_flag_.load(std::memory_order_acquire)) {
            event_sem_.wait();
            if (stop_flag_.load(std::memory_order_acquire)) {
                break;
            }
            // 投递回 io_context 线程执行，避免在监听线程中直接访问 ShmManager 状态
            boost::asio::post(*ioc_, [this]() {
                try {
                    drain_event_channel();
                } catch (const std::exception& e) {
                    SPDLOG_WARN("event thread drain failed | error=\"{}\"", e.what());
                }
            });
        }
        SPDLOG_INFO("event channel monitor thread exited");
    });

    SPDLOG_INFO("shm periodic tasks started | cleanup=60s event_monitor=semaphore");
}

void ShmManager::stop_event_thread() {
    stop_flag_.store(true, std::memory_order_release);
    event_sem_.notify();  // 唤醒线程使其检查 stop_flag_ 并退出
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
}

void ShmManager::set_supervisor(ProcessSupervisor* supervisor) {
    supervisor_ = supervisor;
    // 初始镜像 load: store 的 persist/apply 回调与 build_initial_config_map 均依赖
    // supervisor_ (ShmManager 不持有 registry, 一律经 supervisor_ 访问),
    // supervisor 在构造后注入, 故 load 在此执行 (注入 nullptr 时跳过)
    if (supervisor_ && process_config_store_) {
        process_config_store_->load(build_initial_config_map());
    }
}

std::optional<uint64_t> ShmManager::read_md_page_size(const std::string& source_name) {
    namespace fs = std::filesystem;
    auto json_path = dztrader::paths::configs() / (source_name + ".json");

    if (!fs::exists(json_path)) {
        // 文件不存在: 不是失败 (新行情源首次启动), 使用前缀感知默认
        SPDLOG_INFO("config not found, using default md_page_size | source={} path={}", source_name,
                    json_path.string());
        return kDefaultMdPageSize;
    }

    try {
        // 直接用 nlohmann::json 读子进程配置文件的 shm.page_size_mb
        nlohmann::json cfg;
        std::ifstream ifs(json_path);
        if (ifs) {
            ifs >> cfg;
        }
        if (!cfg.contains("shm") || !cfg["shm"].contains("page_size_mb")) {
            // 字段缺失: 使用前缀感知默认 (不是失败)
            return kDefaultMdPageSize;
        }
        const auto& ps = cfg["shm"]["page_size_mb"];
        if (!ps.is_number_integer() && !ps.is_number_float()) {
            // 类型非法: 使用前缀感知默认 (不是失败)
            return kDefaultMdPageSize;
        }
        uint64_t page_size_mb = ps.get<uint64_t>();
        if (page_size_mb == 0) {
            // 字段为 0: 使用前缀感知默认 (不是失败)
            return kDefaultMdPageSize;
        }
        uint64_t page_size = mb_to_bytes(page_size_mb);
        if (page_size < MIN_PAGE_SIZE_BYTES) {
            SPDLOG_WARN("md_page_size too small, clamped to 1MB | source={} size_mb={}",
                        source_name, page_size_mb);
            page_size = MIN_PAGE_SIZE_BYTES;
        }
        return page_size;
    } catch (const std::exception& e) {
        // 失败路径 C: 文件存在但解析失败 (malformed json)
        SPDLOG_ERROR("failed to read md_page_size | source={} error=\"{}\"", source_name, e.what());
        return std::nullopt;
    }
}

void ShmManager::release_all() {
    // 先停止 event_thread_，避免线程在释放 reader/writer 后还访问它们
    stop_event_thread();

    // 取消 event 通道维护定时器 (recycle lambda 经 `if (ec) return` 安全退出)
    // 必须在 cleaner_/event_writer_ reset 之前取消, 否则 pending 回调可能在
    // writer/cleaner 已销毁后触发, 引发 use-after-free
    if (event_maintenance_timer_) {
        event_maintenance_timer_->cancel();
        event_maintenance_timer_.reset();
    }
    if (preload_points_timer_) {
        preload_points_timer_->cancel();
        preload_points_timer_.reset();
    }
    // 取消 60s 周期清理定时器 (recycle lambda 同样经 `if (ec) return` 安全退出)
    // 旧实现遗漏未 cancel: ioc 在 release_all 后若继续运行, recycle 会持续 60s 触发空操作直到 ioc
    // 销毁
    if (cleanup_timer_) {
        cleanup_timer_->cancel();
        cleanup_timer_.reset();
    }

    // 按依赖逆序释放：cleaner -> md_channels -> event_meta。
    // writer/reader/sem 是直接成员, 析构时自动释放 (声明顺序保证先于 event_meta_ 析构)。
    // cleaner_ 是 unique_ptr, 需显式 reset 以在 event_meta_ 之前释放。
    cleaner_.reset();
    md_channels_.clear();
    event_meta_.reset();
    SPDLOG_INFO("all shm channels released");
}

void ShmManager::persist_process_config(const std::string& name, const nlohmann::json& full) {
    if (full.is_null()) {
        // 删除条目：按 category 删段（registry 条目在 apply 前仍存在）
        const auto* entry = supervisor_ ? supervisor_->find_registry_entry(name) : nullptr;
        const Category category = entry ? entry->category : Category::GatewayMd;
        if (category == Category::WebUI) {
            remove_webui_section(config_path_);
        } else {
            remove_gateway_section(config_path_, category, name);
        }
        SPDLOG_INFO("process config section removed | name={} path={}",
                    name, config_path_.string());
        return;
    }
    // 写段：category 决定 write_gateway_section / write_webui_section
    const auto* entry = supervisor_ ? supervisor_->find_registry_entry(name) : nullptr;
    const Category category = entry ? entry->category : Category::GatewayMd;
    std::vector<std::string> args = full["args"].get<std::vector<std::string>>();
    // write_*_section 接收 platform::RestartPolicy (ProcessEntry.restart 同类型)
    platform::RestartPolicy restart;
    const auto& rj = full["restart"];
    restart.enabled = rj["enabled"].get<bool>();
    restart.max_attempts = rj["max_attempts"].get<int>();
    restart.backoff_sec = rj["backoff_sec"].get<int>();
    const std::string display_name =
        full.contains("display_name") ? full["display_name"].get<std::string>() : "";
    if (category == Category::WebUI) {
        write_webui_section(config_path_, args, restart, display_name);
    } else {
        write_gateway_section(config_path_, category, name, args, restart, display_name);
    }
    SPDLOG_INFO("process config section persisted | name={} path={}", name, config_path_.string());
}

void ShmManager::register_dynamic_gateway(const ProcessEntry& scanned) {
    // 1. registry 注册（persist_process_config 依赖 find_registry_entry 取 category,
    //    否则 dztd_* 会误写 md 段）
    supervisor_->register_gateway(scanned);
    // 2. store 注册: persist（写 dztraderd.json 对应段）+ apply（更新 registry 条目, 幂等）+ 镜像
    nlohmann::json full = {
        {"args", scanned.args},
        {"env", scanned.env},
        {"restart", {{"enabled", scanned.restart.enabled},
                     {"max_attempts", scanned.restart.max_attempts},
                     {"backoff_sec", scanned.restart.backoff_sec}}},
    };
    if (!scanned.display_name.empty()) {
        full["display_name"] = scanned.display_name;
    }
    try {
        process_config_store_->register_process(scanned.name, full);
    } catch (...) {
        // 失败回滚 registry, 避免 ghost 条目（registry 有而 store/文件无）
        // 导致后续 Start 走"已注册"路径却抛 target not registered
        supervisor_->unregister_entry(scanned.name);
        throw;
    }
    SPDLOG_INFO("gateway dynamically registered | name={} category={} exe={}",
                scanned.name, static_cast<int>(scanned.category), scanned.exe.string());
}

void ShmManager::apply_process_config(const std::string& name, const nlohmann::json& full) {
    if (!supervisor_) {
        return;
    }
    if (full.is_null()) {
        supervisor_->unregister_entry(name);
        SPDLOG_INFO("registry entry removed | name={}", name);
        return;
    }
    std::vector<std::string> args = full["args"].get<std::vector<std::string>>();
    std::unordered_map<std::string, std::string> env;
    for (auto it = full["env"].begin(); it != full["env"].end(); ++it) {
        env[it.key()] = it.value().get<std::string>();
    }
    platform::RestartPolicy restart;
    const auto& rj = full["restart"];
    restart.enabled = rj["enabled"].get<bool>();
    restart.max_attempts = rj["max_attempts"].get<int>();
    restart.backoff_sec = rj["backoff_sec"].get<int>();
    const std::string display_name =
        full.contains("display_name") ? full["display_name"].get<std::string>() : "";
    supervisor_->update_entry_config(name, args, env, restart, display_name);
}

nlohmann::json ShmManager::build_initial_config_map() const {
    nlohmann::json map = nlohmann::json::object();
    if (!supervisor_) {
        return map;
    }
    for (const auto& e : supervisor_->registry_entries()) {
        nlohmann::json cfg = {
            {"args", e.args},
            {"env", e.env},
            {"restart", {{"enabled", e.restart.enabled},
                         {"max_attempts", e.restart.max_attempts},
                         {"backoff_sec", e.restart.backoff_sec}}},
        };
        if (!e.display_name.empty()) {
            cfg["display_name"] = e.display_name;
        }
        map[e.name] = std::move(cfg);
    }
    return map;
}

std::string ShmManager::display_name_of(const std::string& name) const {
    if (!process_config_store_) {
        return "";
    }
    const nlohmann::json* cfg = process_config_store_->find(name);
    if (!cfg || !cfg->contains("display_name")) {
        return "";
    }
    return (*cfg)["display_name"].get<std::string>();
}

}  // namespace dztrader::master
