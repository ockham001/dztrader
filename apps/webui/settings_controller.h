#ifndef DZTRADER_WEBUI_SETTINGS_CONTROLLER_H_
#define DZTRADER_WEBUI_SETTINGS_CONTROLLER_H_

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>

#include "config.h"
#include "controller_base.h"
#include "repository.h"
#include "shm_writer.h"

namespace dztrader::webui {

/// 「系统设置」页后端：主进程(dztraderd.json)只读展示 + 事件通道(SET_EVENT_SHM_CONFIG)
/// + webui.json(共享 WebuiConfig 持有者热生效)。全部 admin-only。
class SettingsCtrl : public drogon::HttpController<SettingsCtrl, false>,
                     public ControllerBase {
public:
    SettingsCtrl(std::shared_ptr<Repository> repo,
                 std::shared_ptr<ShmWriter> shm_writer,
                 std::filesystem::path master_config_path,
                 std::filesystem::path webui_config_path,
                 std::shared_ptr<WebuiConfig> webui_cfg)
        : ControllerBase(std::move(repo)),
          shm_writer_(std::move(shm_writer)),
          master_config_path_(std::move(master_config_path)),
          webui_config_path_(std::move(webui_config_path)),
          webui_cfg_(std::move(webui_cfg)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SettingsCtrl::set_event_shm_config,
                  "/api/settings/event-shm-config", drogon::Put);
    ADD_METHOD_TO(SettingsCtrl::get_master, "/api/settings/master", drogon::Get);
    ADD_METHOD_TO(SettingsCtrl::get_webui, "/api/settings/webui", drogon::Get);
    ADD_METHOD_TO(SettingsCtrl::set_webui, "/api/settings/webui", drogon::Put);
    METHOD_LIST_END

    /// PUT /api/settings/event-shm-config: 事件通道配置 merge patch, 直发 SET_EVENT_SHM_CONFIG
    void set_event_shm_config(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET /api/settings/master: 只读展示 dztraderd.json [master]/[shm] 段
    void get_master(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// GET /api/settings/webui: 展示 webui.json 配置 (jwt_secret 仅回传是否存在)
    void get_webui(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// PUT /api/settings/webui: 修改 token_ttl_sec / notify_cache_size, 持久化 + 热生效
    void set_webui(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<ShmWriter> shm_writer_;
    std::filesystem::path master_config_path_;
    std::filesystem::path webui_config_path_;
    std::shared_ptr<WebuiConfig> webui_cfg_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SETTINGS_CONTROLLER_H_
