#include "market_source_controller.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <dztrader/platform/process.h>

namespace dztrader::webui {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// guard_process_dispatch: 下发守卫（dispatch_op 与 set_auto_login 共用）
// 检查: source 存在 (404) + 进程在镜像中 running (503) + shm_writer 就绪 (503)
// 成功: 返回 nullopt, 通过 process_name 输出目标进程名
// 失败: 返回 HTTP 错误响应 (caller 直接 callback 返回)
// ---------------------------------------------------------------------------
std::optional<drogon::HttpResponsePtr> MarketSourceCtrl::guard_process_dispatch(
    int64_t source_id,
    const std::string& log_label,
    std::string& process_name) {
    auto source = repo_->get_market_source(source_id);
    if (!source.has_value()) {
        SPDLOG_WARN("op dispatch rejected: source not found | source_id={}", source_id);
        return error_response(drogon::k404NotFound, "market source not found");
    }
    process_name = process_name_for_source_type(source->source_type);

    if (!is_process_running_in_mirror(source_id)) {
        SPDLOG_WARN("op dispatch rejected: dzmd_ctp not running | source_id={}", source_id);
        return error_response(drogon::k503ServiceUnavailable,
                              "dzmd_ctp not running, cannot apply config");
    }

    if (!shm_writer_ || !shm_writer_->is_ready()) {
        SPDLOG_ERROR("{} SHM writer not available | source_id={} process={}",
                     log_label, source_id, process_name);
        return error_response(drogon::k503ServiceUnavailable, "shm writer not available");
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// dispatch_op: op-based 配置下发统一 helper
// 检查: source 存在 (404) + 进程在镜像中 running (503) + shm_writer 就绪 (503)
// op_name: MdConfigOp 序列化后的字符串 (如 "AddBroker"); params: op 携带的参数 JSON
// 成功返回 nullopt (caller 应返回 200 ok=dispatched); SHM 写入为 fire-and-forget
// ---------------------------------------------------------------------------
std::optional<drogon::HttpResponsePtr> MarketSourceCtrl::dispatch_op(
    int64_t source_id,
    const std::string& log_label,
    const std::string& op_name,
    const nlohmann::json& params) {
    std::string process_name;
    if (auto err = guard_process_dispatch(source_id, log_label, process_name); err.has_value()) {
        return *err;
    }

    const nlohmann::json op_req = {
        {"op", op_name},
        {"params", params},
    };
    // fire-and-forget: ShmWriter 内部 catch 异常并记录, 不阻断 HTTP 响应
    shm_writer_->write_md_set_config(process_name, op_req);
    SPDLOG_INFO("{} dispatched | source_id={} process={}", log_label, source_id, process_name);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// PUT /api/market-sources/{id}/auto-login - 全量设置自动登录/登出排程 (admin)
// 契约 auto-login: 直发 SET_AUTO_LOGIN 帧 (enabled + schedules 全量; schedules 出现时整体覆盖)
// HTTP 同步响应仅表示 "已下发", 最终状态由 WS auto_login 推送 (RTN_AUTO_LOGIN) 决定
// ---------------------------------------------------------------------------
void MarketSourceCtrl::set_auto_login(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
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
    // 结构校验: enabled 必须为 bool; schedules 必须为数组 (元素由网关权威校验)
    if (body.contains("enabled") && !body["enabled"].is_boolean()) {
        callback(error_response(drogon::k400BadRequest, "'enabled' must be boolean"));
        return;
    }
    if (body.contains("schedules") && !body["schedules"].is_array()) {
        callback(error_response(drogon::k400BadRequest, "'schedules' must be an array"));
        return;
    }
    // 共用守卫: source 存在 (404) + 进程运行 (503) + shm_writer 就绪 (503)
    std::string process_name;
    auto err = guard_process_dispatch(id, "set_auto_login", process_name);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    shm_writer_->write_set_auto_login(process_name, body);
    SPDLOG_INFO("set auto login dispatched | source_id={} process={}", id, process_name);
    // 关闭自动登录是安全敏感操作, 下发成功后打 WARN 审计日志
    if (body.contains("enabled") && !body["enabled"].get<bool>()) {
        SPDLOG_WARN("auto-login disable dispatched | source_id={} process={}", id, process_name);
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// ---------------------------------------------------------------------------
// 经纪商 CRUD 走 op-based 配置变更流程 (契约 md-config SET_MD_CONFIG)
// 通用模式: 下发 op + params → dzmd_ctp 校验+应用+持久化+回传 RTN_MD_CONFIG →
//          dzweb 镜像更新 → WS 推送 md_rtn_config → 前端刷新
// HTTP 同步响应仅表示"已下发", 最终状态由 WS 推送决定 (失败路径 C/D/E)
// ---------------------------------------------------------------------------

// POST /api/market-sources/{id}/brokers - 添加经纪商 (admin) (op-based AddBroker)
// name 为不可变 key; frontends 默认空数组; 子进程校验 name 唯一性
void MarketSourceCtrl::add_broker(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
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
    std::string name = body.value("name", "");
    if (name.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing required field: name"));
        return;
    }
    const Json params = {
        {"name", name},
        {"broker_id", body.value("broker_id", "")},
        {"user_id", body.value("user_id", "")},
        {"password", body.value("password", "")},
        {"product_info", body.value("product_info", "")},
    };
    auto err = dispatch_op(id, "add_broker name=" + name, "AddBroker", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// DELETE /api/market-sources/{id}/brokers/{broker_name} - 删除经纪商 (admin) (op-based RemoveBroker)
// 受状态保护: 子进程非 Idle 时拒绝 (失败路径 C)
void MarketSourceCtrl::remove_broker(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id, const std::string& broker_name) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (broker_name.empty()) {
        callback(error_response(drogon::k400BadRequest, "broker_name is empty"));
        return;
    }
    const Json params = {{"name", broker_name}};
    auto err = dispatch_op(id, "remove_broker name=" + broker_name, "RemoveBroker", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// PUT /api/market-sources/{id}/brokers/{broker_name} - 编辑经纪商字段 (admin) (op-based UpdateBroker)
// password 处理: dzweb 透传 body.password (可能为 "****" 表示未改); 子进程对 "****" 保留旧值
void MarketSourceCtrl::update_broker(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id, const std::string& broker_name) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (broker_name.empty()) {
        callback(error_response(drogon::k400BadRequest, "broker_name is empty"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // body 中的 name 必须与 URL 一致 (name 为不可变 key, 不响应改名请求)
    if (body.value("name", "") != broker_name) {
        callback(error_response(drogon::k400BadRequest,
                                "broker name in body must match URL"));
        return;
    }
    const Json params = {
        {"name", broker_name},
        {"broker_id", body.value("broker_id", "")},
        {"user_id", body.value("user_id", "")},
        {"product_info", body.value("product_info", "")},
        {"password", body.value("password", "****")},
    };
    auto err = dispatch_op(id, "update_broker name=" + broker_name, "UpdateBroker", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// PUT /api/market-sources/{id}/brokers/{broker_name}/frontends - 替换前置地址列表 (admin)
// Body: {frontends: [{address, label, enabled}, ...]}; op: SetFrontends (整体替换)
void MarketSourceCtrl::update_broker_frontends(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,  // NOLINT
    int64_t id, const std::string& broker_name) {
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "forbidden"));
        return;
    }
    if (broker_name.empty()) {
        callback(error_response(drogon::k400BadRequest, "broker_name is empty"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.contains("frontends") || !body["frontends"].is_array()) {
        callback(error_response(drogon::k400BadRequest,
                                "missing or invalid 'frontends' array"));
        return;
    }
    const Json params = {
        {"name", broker_name},
        {"frontends", body["frontends"]},
    };
    auto err = dispatch_op(id, "update_broker_frontends name=" + broker_name,
                           "SetFrontends", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// PUT /api/market-sources/{id}/current-broker - 切换当前选中经纪商 (admin) (op-based SetCurrentBroker)
// Body: {name} 或 {name: ""} 清空选中; 受状态保护 (失败路径 C)
void MarketSourceCtrl::set_current_broker(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
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
    std::string name = body.value("name", "");
    // name 为空表示清空选中 (合法); 非空时由子进程校验存在性
    const Json params = {{"name", name}};
    auto err = dispatch_op(id, "set_current_broker name=" + name, "SetCurrentBroker", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// PUT /api/market-sources/{id}/subscribe-params - 修改订阅参数 (admin) (op-based SetSubscribeParams)
// 无状态保护: 任意时刻可改; Body 4 个可选字段, 缺失=保留旧值 (契约 md-config)
void MarketSourceCtrl::set_subscribe_params(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
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
    // 透传可选字段: 缺失=保留旧值 (契约 md-config SetSubscribeParams), 校验由子进程负责
    Json params = Json::object();
    for (const char* key : {"subscribe_batch_size", "subscribe_batch_delay_ms",
                            "sub_check_interval_ms", "sub_max_retry"}) {
        if (body.contains(key)) {
            params[key] = body[key];
        }
    }
    if (params.empty()) {
        callback(error_response(drogon::k400BadRequest, "no subscribe params in body"));
        return;
    }
    auto err = dispatch_op(id, "set_subscribe_params", "SetSubscribeParams", params);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

// PUT /api/market-sources/{id}/shm-config - 修改 SHM 行情通道配置 (admin)
// 契约 shm: 直发 SET_MD_SHM_CONFIG 帧, HTTP 同步响应仅表示"已下发",
//          最终状态由 WS md_shm_config 推送 (RTN_MD_SHM_CONFIG) 决定
// 注意: 不能复用 dispatch_op (其写 SET_MD_CONFIG 帧); 用 guard_process_dispatch
//       守卫后直接写 SET_MD_SHM_CONFIG 帧
void MarketSourceCtrl::set_shm_config(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id) {  // NOLINT
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
    if (!body.is_object() || body.empty()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 整体透传 (契约 shm SET payload 即 ShmConfig 子集): 含 preload_points null 删除语义,
    // 不做字段级筛选; page_size_mb 透传无害 (网关跳过)
    std::string process_name;
    auto err = guard_process_dispatch(id, "set_shm_config", process_name);
    if (err.has_value()) {
        callback(*err);
        return;
    }
    shm_writer_->write_set_md_shm_config(process_name, body);
    callback(json_response(drogon::k200OK, {{"ok", true}, {"message", "dispatched"}}));
}

}  // namespace dztrader::webui