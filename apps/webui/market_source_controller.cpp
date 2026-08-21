#include "market_source_controller.h"
#include "ws_controller.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <cctype>
#include <dztrader/platform/process.h>

namespace dztrader::webui {

using Json = nlohmann::json;

// 单实例模式下,source_type → process.name 映射
// "CTP" → "dzmd_ctp", "XTP" → "dzmd_xtp" (future)
std::string process_name_for_source_type(const std::string& source_type) {
    std::string lower = source_type;
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return "dzmd_" + lower;
}

// 提取 ui_card 大类 (契约 md-config 接口类型识别): 去掉 dzmd_/dztd_ 前缀, 取第一个 _ 之前的部分
// dzmd_ctp -> ctp, dzmd_ctp_1234 -> ctp, 未知前缀/退化 -> ""
std::string extract_ui_card(const std::string& process_name) {
    constexpr size_t k_prefix_len = 5;  // "dzmd_" / "dztd_"
    if (process_name.size() <= k_prefix_len) {
        return "";
    }
    std::string tail;
    if (process_name.compare(0, k_prefix_len, "dzmd_") == 0 ||
        process_name.compare(0, k_prefix_len, "dztd_") == 0) {
        tail = process_name.substr(k_prefix_len);
    } else {
        return "";
    }
    if (tail.empty()) {
        return "";
    }
    auto pos = tail.find('_');
    return pos == std::string::npos ? tail : tail.substr(0, pos);
}

Json MarketSourceCtrl::market_source_to_json(const MarketSource& s) {
    return {
        {"id", s.id},
        {"source_type", s.source_type},
        {"source_name", s.source_name},
        {"display_name", s.display_name},
        {"ui_card", extract_ui_card(s.source_name)},
        {"is_added", s.is_added},
        {"auto_login", false},  // 占位: 实际值由镜像覆盖 (market_source_detail/list 中处理)
        {"created_at", s.created_at},
        {"updated_at", s.updated_at},
    };
}

Json MarketSourceCtrl::market_source_detail(const MarketSource& s) {
    Json detail = market_source_to_json(s);
    // 排程/自动登录已迁移到契约 auto-login (SET/RTN_AUTO_LOGIN 帧), 镜像驱动
    auto auto_login_opt = process_mirror_->get_auto_login(s.source_name);
    if (auto_login_opt.has_value()) {
        detail["auto_login"] = auto_login_opt->value("enabled", false);
    }
    return detail;
}

// ---------------------------------------------------------------------------
// POST /api/market-sources - create (admin)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::create(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
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
    const std::string source_type = body.value("source_type", "");
    const std::string source_name = body.value("source_name", "");
    const std::string display_name = body.value("display_name", "");
    if (source_type.empty() || source_name.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing required fields"));
        return;
    }
    int64_t const id = repo_->create_market_source(source_type, source_name, display_name);
    auto source = repo_->get_market_source(id);
    notifier_.broadcast_data_changed("market_sources");
    callback(json_response(drogon::k201Created, market_source_detail(*source)));
}

// ---------------------------------------------------------------------------
// PUT /api/market-sources/{id} - update (admin)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::update(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                              int64_t id) {
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
    const std::string display_name = body.value("display_name", "");
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    // display_name 走 DB; auto_login 由 PUT /auto-login 端点单独处理走 SHM
    if (!repo_->update_market_source(id, display_name)) {
        callback(error_response(drogon::k500InternalServerError, "failed to update market source"));
        return;
    }
    auto updated = repo_->get_market_source(id);
    notifier_.broadcast_data_changed("market_sources");  // 契约 rest §2.3: 多客户端列表同步
    callback(json_response(drogon::k200OK, market_source_detail(*updated)));
}

// ---------------------------------------------------------------------------
// DELETE /api/market-sources/{id} - "删除此行情源"按钮 (admin)
// 行为：发 PROCESS_CONTROL "remove" → master stop_process + remove_gateway_section
//       master 持久化移除 dztraderd.Json [gateways.<name>] 段, 下次启动不再拉起
// 保留 DB 主表记录 (market_sources 行); 用户再次"添加"时复用主表记录
// ---------------------------------------------------------------------------
void MarketSourceCtrl::remove(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                              int64_t id) {
    SPDLOG_INFO("remove market source requested | id={}", id);
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    std::string process_name = process_name_for_source_type(source->source_type);
    SPDLOG_INFO("remove market source | id={} source_name={} source_type={} process={}",
                id, source->source_name, source->source_type, process_name);
    // 失败路径 A: shm_writer 未就绪 -> 返回 503
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        SPDLOG_WARN("shm not available, cannot send remove | source_id={} process={}", id, process_name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    // action="remove" (区别于 "stop"): master 收到 remove 后 stop_process + on_child_exit 中
    // remove_gateway_section 持久化移除网关声明, 下次 master 启动不再自动拉起
    if (!shm_writer_->write_process_control(platform::ProcessAction::Remove, process_name)) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    // 契约 rest（修订）: 下发成功即标记生命周期（is_added=0）。
    // 边缘: master 侧 RemoveFailed 时该行已被隐藏--NOTIFY_UI 弹窗提示用户，
    // 可从 available 列表重新添加（create 复用行并复位 is_added=1）
    repo_->set_market_source_added(id, false);
    // 保留 DB 主表记录 (market_sources 行); 不调用 repo_->delete_market_source(id)
    notifier_.broadcast_data_changed("market_sources");  // 列表条目消失（is_added=0）
    SPDLOG_INFO("remove dispatched | source_id={} process={} (db kept)", id, process_name);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"id", id}}));
}

// ---------------------------------------------------------------------------
// POST /api/market-sources/{id}/login - SHM md_connect (admin)
// Wave 2C: 凭证已存于子进程配置文件; 本端点只发 md_connect 事件, 不持久化任何数据
// ---------------------------------------------------------------------------
void MarketSourceCtrl::login(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                             int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    const std::string source_name = source.has_value() ? source->source_name : std::string{};
    // 共享守卫（control_guard.h，与 WS md_connect 同一处决策）；source_valid 已由上方 404 兜底
    const bool writer_ready = shm_writer_ && shm_writer_->is_ready();
    const bool process_running = is_process_running_in_mirror(id);
    switch (evaluate_control_guard(true, source.has_value(), writer_ready, process_running)) {
        case ControlGuard::NotAdmin:  // 理论不可达：入口已拦截
            callback(error_response(drogon::k403Forbidden, "forbidden"));
            return;
        case ControlGuard::SourceInvalid:
            callback(error_response(drogon::k404NotFound, "market source not found"));
            return;
        case ControlGuard::ChannelUnavailable:
            SPDLOG_WARN("shm not available, cannot send md_connect | source={}", source_name);
            callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
            return;
        case ControlGuard::ProcessNotRunning:
            SPDLOG_WARN("login rejected: process not running | id={}", id);
            callback(error_response(drogon::k503ServiceUnavailable,
                                    "process not running, cannot send md_connect"));
            return;
        case ControlGuard::Ok:
            break;
    }
    // 契约 rest §1: 写入事件通道失败 -> 503（不再 200 {ok:false}，避免前端 pending 悬挂至超时）
    if (!shm_writer_->write_md_connect(source->source_name)) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

// ---------------------------------------------------------------------------
// POST /api/market-sources/{id}/logout - SHM md_disconnect (admin)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::logout(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                              int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    const std::string source_name = source.has_value() ? source->source_name : std::string{};
    // 共享守卫（control_guard.h，与 WS md_disconnect 同一处决策）
    const bool writer_ready = shm_writer_ && shm_writer_->is_ready();
    const bool process_running = is_process_running_in_mirror(id);
    switch (evaluate_control_guard(true, source.has_value(), writer_ready, process_running)) {
        case ControlGuard::NotAdmin:  // 理论不可达：入口已拦截
            callback(error_response(drogon::k403Forbidden, "forbidden"));
            return;
        case ControlGuard::SourceInvalid:
            callback(error_response(drogon::k404NotFound, "market source not found"));
            return;
        case ControlGuard::ChannelUnavailable:
            SPDLOG_WARN("shm not available, cannot send md_disconnect | source={}", source_name);
            callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
            return;
        case ControlGuard::ProcessNotRunning:
            SPDLOG_WARN("logout rejected: process not running | id={}", id);
            callback(error_response(drogon::k503ServiceUnavailable,
                                    "process not running, cannot send md_disconnect"));
            return;
        case ControlGuard::Ok:
            break;
    }
    // 契约 rest §1: 写入事件通道失败 -> 503（不再 200 {ok:false}，避免前端 pending 悬挂至超时）
    if (!shm_writer_->write_md_disconnect(source->source_name)) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

// ---------------------------------------------------------------------------
// POST /api/market-sources/{id}/start - 启动行情源进程 (admin)
// 接收可选 JSON body {display_name?: string}; 更新 DB display_name 后发 PROCESS_CONTROL start
// ---------------------------------------------------------------------------
void MarketSourceCtrl::start(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                              int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    // 解析可选 JSON body: display_name (新协议仅透传 display_name 作为 Start config patch,
    // page_size_mb 不再发送, 由 master 用全局默认 md_page_size_); body 为空也允许
    std::string display_name;
    bool has_body = false;
    auto body_text = req->getBody();
    if (!body_text.empty()) {
        try {
            auto body = Json::parse(body_text);
            has_body = true;
            if (body.contains("display_name") && body["display_name"].is_string()) {
                display_name = body["display_name"].get<std::string>();
            }
        } catch (...) {
            callback(error_response(drogon::k400BadRequest, "bad request"));
            return;
        }
    }
    std::string process_name = process_name_for_source_type(source->source_type);
    // 1. (可选) 更新 DB 中的 display_name
    if (has_body && !display_name.empty() && display_name != source->display_name) {
        if (!repo_->update_market_source(id, display_name)) {
            SPDLOG_WARN("failed to update display_name | id={} name={}", id, display_name);
        }
    }
    // 2. 发送 PROCESS_CONTROL action=start 帧 (master 立即拉起子进程)
    std::optional<nlohmann::json> start_config;
    if (!display_name.empty()) {
        start_config = nlohmann::json{{"display_name", display_name}};
    }
    if (!shm_writer_->write_process_control(platform::ProcessAction::Start,
                                            process_name, std::move(start_config))) {
        SPDLOG_WARN("start write failed | source_id={} process={}", id, process_name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("start dispatched | source_id={} process={} display_name={}",
                id, process_name, display_name);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"source", source->source_name}}));
}

// ---------------------------------------------------------------------------
// POST /api/market-sources/{id}/stop - 停止行情源进程 (admin)
// ---------------------------------------------------------------------------
void MarketSourceCtrl::stop(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
                             int64_t id) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    auto source = repo_->get_market_source(id);
    if (!source.has_value()) {
        callback(error_response(drogon::k404NotFound, "market source not found"));
        return;
    }
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        callback(error_response(drogon::k503ServiceUnavailable, "shm not available"));
        return;
    }
    // 单实例模式: PROCESS_CONTROL target = "dzmd_<type>" (master 的 process.name)
    std::string process_name = process_name_for_source_type(source->source_type);
    if (!shm_writer_->write_process_control(platform::ProcessAction::Stop, process_name)) {
        SPDLOG_WARN("stop write failed | source_id={} process={}", id, process_name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("stop dispatched | source_id={} process={}", id, process_name);
    callback(json_response(drogon::k200OK, {{"ok", true}}));
}

}  // namespace dztrader::webui