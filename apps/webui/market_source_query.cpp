#include "market_source_controller.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <unordered_set>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/process/exe_scanner.h>
#include <dztrader/platform/process.h>

namespace dztrader::webui {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// 镜像 helpers (跨文件共享给 controller.cpp / ops.cpp)
// ---------------------------------------------------------------------------
std::optional<Json> MarketSourceCtrl::get_config_from_mirror(int64_t source_id) const {
    auto source = repo_->get_market_source(source_id);
    if (!source.has_value()) {
        return std::nullopt;
    }
    const std::string process_name = process_name_for_source_type(source->source_type);
    auto config = process_mirror_->get_config(process_name);
    if (!config.has_value() || !config->is_object()) {
        return std::nullopt;
    }
    return *config;
}

// 检查指定 source 对应进程在镜像中是否为 running (SHM 下发前确认 dzmd_ctp 在线)
// B-C3: 读 statuses_ (PROCESS_STATUS 先到, 避免误判为离线); best-effort 检查
bool MarketSourceCtrl::is_process_running_in_mirror(int64_t source_id) const {
    auto source = repo_->get_market_source(source_id);
    if (!source.has_value()) {
        return false;
    }
    const std::string process_name = process_name_for_source_type(source->source_type);
    auto status = process_mirror_->get_status(process_name);
    if (!status.has_value()) {
        return false;
    }
    return status->state == platform::ChildState::Running;
}

// ---------------------------------------------------------------------------
// GET /api/market-sources/available
// 扫描 app_root() 下所有 dzmd_* 行情源 exe (契约 process: App Root + 去重), 返回真实可用进程
// ---------------------------------------------------------------------------
void MarketSourceCtrl::list_available(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;
    namespace fs = std::filesystem;
    // 契约 process: dzweb 扫描 dzmd/dztd 必须用 App Root, 不得用 dzweb 自身 exe 目录或 cwd
    const fs::path scan_dir = dztrader::this_process::app_root();

    auto existing = repo_->list_market_sources();
    // added 语义（契约 rest §2.3 修订）: DB 存在且 is_added=1（生命周期标记），
    // 不再依赖镜像运行状态
    std::unordered_map<std::string, bool> added_by_name;
    for (const auto& s : existing) {
        added_by_name[s.source_name] = s.is_added;
    }

    // 委托给 libs/process 库 (消除与 master find_exe_by_stem 的扫描规则重复)
    auto scanned = dztrader::process::scan_all_exes(scan_dir);

    Json arr = Json::array();
    for (const auto& info : scanned) {
        // 仅返回 dzmd_* 行情源 (dztd_/dzweb 不在 available 列表中)
        if (info.kind != dztrader::process::ProcessKind::GatewayMd) {
            continue;
        }
        const auto it_added = added_by_name.find(info.name);
        const bool in_db = it_added != added_by_name.end();
        const bool added = in_db && it_added->second;
        arr.push_back({
            {"name", info.name},
            {"display_name", info.name},
            {"ui_card", extract_ui_card(info.name)},
            {"added", added},
            {"in_db", in_db},
        });
    }

    SPDLOG_INFO("available market sources scanned | count={} dir={}",
                arr.size(), scan_dir.string());
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// GET /api/market-sources
// 契约 rest §3: 行情源列表以 DB 为真相源--返回 is_added=1 的 dzmd_* 行，
// 不依赖进程镜像运行状态（运行状态由 WS process_status 表达，前端按卡片渲染）
// ---------------------------------------------------------------------------
void MarketSourceCtrl::list(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;
    auto sources = repo_->list_market_sources();
    Json arr = Json::array();
    for (const auto& s : sources) {
        if (!s.source_name.starts_with("dzmd_")) {
            continue;  // 防御: 仅行情源
        }
        if (!s.is_added) {
            continue;  // remove 已下发的行不再列出（主表记录保留）
        }
        Json src_json = market_source_to_json(s);
        // auto_login 从镜像读 (dzmd_ctp 上报的 RTN_AUTO_LOGIN, 契约 auto-login)
        auto auto_login_opt = process_mirror_->get_auto_login(s.source_name);
        if (auto_login_opt.has_value()) {
            src_json["auto_login"] = auto_login_opt->value("enabled", false);
        }
        arr.push_back(std::move(src_json));
    }
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// GET /api/market-sources/{id} - detail (auto_login 从镜像读, 契约 auto-login)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::get(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
    (void)req;
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    callback(json_response(drogon::k200OK, market_source_detail(*source)));
}

// ---------------------------------------------------------------------------
// GET /api/market-sources/{id}/config - 读取配置 (从镜像), password 脱敏为 "****"
// ---------------------------------------------------------------------------
void MarketSourceCtrl::get_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    auto config_opt = get_config_from_mirror(id);
    if (!config_opt.has_value()) {
        callback(error_response(drogon::k503ServiceUnavailable,
                                 "process not running or mirror not ready"));
        return;
    }
    // 脱敏: password 字段返回掩码; Wave 5A: 同时脱敏 brokers[].password
    nlohmann::json safe_config = *config_opt;
    if (safe_config.contains("password") && !safe_config["password"].is_null()) {
        safe_config["password"] = "****";
    }
    if (safe_config.contains("brokers") && safe_config["brokers"].is_array()) {
        for (auto& broker : safe_config["brokers"]) {
            if (broker.contains("password") && !broker["password"].is_null()) {
                broker["password"] = "****";
            }
        }
    }
    callback(json_response(drogon::k200OK,
                            {{"source", source->source_name}, {"config", safe_config}}));
}

// ---------------------------------------------------------------------------
// POST /api/market-sources/refresh - 触发 SHM 全量查询 (WS 断连恢复后补拉镜像)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::refresh(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    SPDLOG_INFO("refresh market sources requested | user={}",
                req->getOptionalParameter<std::string>("user_id").value_or("unknown"));
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not ready"));
        return;
    }
    shm_writer_->write_query_all();
    SPDLOG_INFO("refresh dispatched | query_all=true");
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

}  // namespace dztrader::webui