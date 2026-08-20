#include "settings_controller.h"

#include <dztrader/core/json_section.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>

namespace dztrader::webui {

using Json = nlohmann::json;

// PUT /api/settings/event-shm-config
// 契约 shm: 直发 SET_EVENT_SHM_CONFIG 帧 (无 instance_id, RFC 7386 合并 patch 整体透传),
// HTTP 同步响应仅表示"已受理", 最终状态由 WS event_shm_config 推送决定
void SettingsCtrl::set_event_shm_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 整体透传, 字段校验由 master EventShmConfig 负责 (失败经日志/NOTIFY_UI 呈现)
    shm_writer_->write_set_event_shm_config(body);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// GET /api/settings/webui
// 注: 不返回 log_level——dzweb 日志级别运行期可经「日志」页修改, 此处展示会失真且重复, 归属日志页管理
void SettingsCtrl::get_webui(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    callback(json_response(drogon::k200OK, {
        {"server_listen", webui_cfg_->server_listen},
        {"server_port", webui_cfg_->server_port},
        {"token_ttl_sec", webui_cfg_->token_ttl_sec},
        {"jwt_secret_set", !webui_cfg_->jwt_secret.empty()},
        {"notify_cache_size", webui_cfg_->notify_cache_size},
    }));
}

namespace {

// webui.json load-modify-save: 读全量 -> 应用受限字段 -> tmp+rename 原子写 (保留其他 section)
void save_webui_json(const std::filesystem::path& path, const nlohmann::json& patch) {
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs) {
            try {
                ifs >> full;
            } catch (const std::exception& e) {
                // 旧文件 JSON 损坏: 不能静默清空其他 section, 否则单 section 写入会丢失全部配置。
                // 备份损坏文件后用空 object 起步, 让用户能从备份恢复 (对齐 json_section.h)。
                spdlog::warn("webui.json parse failed | error={}", e.what());
                full = nlohmann::json::object();
                auto backup = path;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now()));
                std::error_code ec;
                std::filesystem::rename(path, backup, ec);
                // 备份失败不阻断 save (可能 permission denied), 但至少尝试备份
                if (ec) spdlog::warn("backup corrupt webui.json failed | error={}", ec.message());
            }
        }
    }
    // auth/notify 段守卫: 旧文件存在但对应段缺失/非 object (如数组) 时, 先规整为 object 再写子字段
    if (!full.contains("auth") || !full["auth"].is_object()) full["auth"] = nlohmann::json::object();
    if (!full.contains("notify") || !full["notify"].is_object()) full["notify"] = nlohmann::json::object();
    if (patch.contains("token_ttl_sec")) full["auth"]["token_ttl_sec"] = patch["token_ttl_sec"];
    if (patch.contains("notify_cache_size")) full["notify"]["cache_size"] = patch["notify_cache_size"];

    auto tmp = path;
    tmp += ".tmp";
    try {
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("open failed | path=" + tmp.string());
            ofs << full.dump(2);
            ofs.flush();
            if (!ofs) throw std::runtime_error("write failed | path=" + tmp.string());
            // 显式 close 并检查: ofstream 析构调用的 close() 会丢弃错误,
            // 磁盘满/配额耗尽时可能写入不完整, rename 会把截断文件提升为正式配置。
            ofs.close();
            if (!ofs) throw std::runtime_error("close failed | path=" + tmp.string());
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) throw std::runtime_error("rename failed | from=" + tmp.string() + " to=" + path.string());
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

}  // namespace

// PUT /api/settings/webui
void SettingsCtrl::set_webui(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    const bool has_ttl = body.contains("token_ttl_sec");
    const bool has_cache = body.contains("notify_cache_size");
    if (!has_ttl && !has_cache) {
        callback(error_response(drogon::k400BadRequest, "no editable field"));
        return;
    }
    // 仅允许 token_ttl_sec / notify_cache_size, 其余字段(如 jwt_secret)忽略并拒绝
    uint64_t ttl_sec = 0;
    uint64_t cache_size = 0;
    for (const auto& [k, v] : body.items()) {
        if (k != "token_ttl_sec" && k != "notify_cache_size") {
            callback(error_response(drogon::k400BadRequest, "unsupported field: " + k));
            return;
        }
        if (!v.is_number_unsigned()) {
            callback(error_response(drogon::k400BadRequest, "invalid value: " + k));
            return;
        }
        // 先用 uint64 取值, 避免后续 get<uint32_t>/get<size_t> 截断导致大小判断不一致
        try {
            if (k == "token_ttl_sec") {
                ttl_sec = v.get<uint64_t>();
            } else {
                cache_size = v.get<uint64_t>();
            }
        } catch (...) {
            callback(error_response(drogon::k400BadRequest, "invalid value: " + k));
            return;
        }
    }
    // token_ttl_sec 下限 60 秒、业务上限 604800 (7 天): 防误配超大值导致 JWT 过期时间异常
    if (has_ttl && (ttl_sec < 60 || ttl_sec > 604800)) {
        callback(error_response(drogon::k400BadRequest, "token_ttl_sec must be in [60, 604800]"));
        return;
    }
    // notify_cache_size 业务上限 100 万条: 防止误配过大值撑爆 NotifyCache 内存 (7×24 运行稳定)
    if (has_cache && cache_size > 1000000) {
        callback(error_response(drogon::k400BadRequest, "notify_cache_size must be <= 1000000"));
        return;
    }
    // 持久化 + 热生效 (token_ttl_sec 运行期直接改共享持有者; notify_cache_size 重启生效)
    try {
        save_webui_json(webui_config_path_, body);
    } catch (const std::exception& e) {
        callback(error_response(drogon::k500InternalServerError, e.what()));
        return;
    }
    if (has_ttl) webui_cfg_->token_ttl_sec = body["token_ttl_sec"].get<uint32_t>();
    if (has_cache) webui_cfg_->notify_cache_size = body["notify_cache_size"].get<size_t>();
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

// get_master 为 Task 4 期的 stub（保证本 Task 独立可链接、可测），
// Task 4 实现 get_master 时整体替换。
void SettingsCtrl::get_master(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    callback(json_response(drogon::k200OK, Json::object()));
}

}  // namespace dztrader::webui
