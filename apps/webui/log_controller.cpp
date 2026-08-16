#include "log_controller.h"
#include "log_service.h"

#include <dztrader/platform/log_config.h>
#include <dztrader/core/path.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {

using Json = nlohmann::json;

namespace {
/// drogon's HttpRequest::getParameter(key) returns "" if absent and has no
/// overload taking a default. Wrap the empty-check here.
std::string param_or(const drogon::HttpRequestPtr& req,
                     const std::string& key,
                     const std::string& def) {
    const std::string v = req->getParameter(key);
    return v.empty() ? def : v;
}

/// 解析整型 query 参数：缺失用默认值；非数字/溢出回退 nullopt（caller 回 400）。
/// stoi 会抛 invalid_argument/out_of_range，必须隔离（宁肯乱码不崩溃）
std::optional<int> parse_int_param(const drogon::HttpRequestPtr& req,
                                   const std::string& key,
                                   int def) {
    const std::string v = req->getParameter(key);
    if (v.empty()) {
        return def;
    }
    try {
        return std::stoi(v);
    } catch (...) {
        return std::nullopt;
    }
}
}  // namespace

bool LogCtrl::is_valid_level(const std::string& level) {
    // 统一调用 LogConfig::is_valid_level（契约 log）
    return dztrader::platform::LogConfig::is_valid_level(level);
}

void LogCtrl::get_files(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    LogService svc(dztrader::paths::logs());
    const std::string logger = req->getParameter("logger");
    const std::string date = req->getParameter("date");
    const auto limit_opt = parse_int_param(req, "limit", 30);
    if (!limit_opt.has_value()) {
        callback(error_response(drogon::k400BadRequest, "invalid limit parameter"));
        return;
    }
    const int limit = *limit_opt;
    const auto offset_opt = parse_int_param(req, "offset", 0);
    if (!offset_opt.has_value()) {
        callback(error_response(drogon::k400BadRequest, "invalid offset parameter"));
        return;
    }
    const int offset = *offset_opt;

    auto files = svc.list_files(logger, date, limit, offset);
    Json arr = Json::array();
    for (const auto& f : files) {
        arr.push_back({
            {"name", f.name}, {"logger", f.logger},
            {"size", f.size}, {"mtime", f.mtime}, {"path", f.path}
        });
    }
    callback(json_response(drogon::k200OK, arr));
}

void LogCtrl::get_content(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    LogService svc(dztrader::paths::logs());
    const std::string file = req->getParameter("file");
    if (file.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing file parameter"));
        return;
    }
    const auto offset_opt = parse_int_param(req, "offset", 0);
    if (!offset_opt.has_value()) {
        callback(error_response(drogon::k400BadRequest, "invalid offset parameter"));
        return;
    }
    const int offset = *offset_opt;
    const auto limit_opt = parse_int_param(req, "limit", 500);
    if (!limit_opt.has_value()) {
        callback(error_response(drogon::k400BadRequest, "invalid limit parameter"));
        return;
    }
    const int limit = *limit_opt;
    std::string level = req->getParameter("level");
    // 规范化 level 参数（warn->warning），使 level_severity 能正确匹配
    if (!level.empty()) {
        level = dztrader::platform::LogConfig::canonicalize_level(level);
    }
    const std::string keyword = req->getParameter("keyword");
    const std::string from = req->getParameter("from");
    const std::string to = req->getParameter("to");
    const bool from_end = req->getParameter("from_end") == "true";

    auto content = svc.read_content(file, offset, limit, level, keyword, from, to, from_end);
    Json lines = Json::array();
    for (const auto& l : content.lines) {
        lines.push_back({
            {"n", l.n}, {"ts", l.ts}, {"level", l.level},
            {"logger", l.logger}, {"func", l.func}, {"file", l.file},
            {"line", l.line_no}, {"pid", l.pid}, {"tid", l.tid},
            {"msg", l.msg}, {"raw", l.raw}, {"parsed", l.parsed}
        });
    }
    const Json resp = {{"lines", lines}, {"total", content.total}};
    callback(json_response(drogon::k200OK, resp));
}

void LogCtrl::get_stats(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    LogService svc(dztrader::paths::logs());
    const std::string file = req->getParameter("file");
    if (file.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing file parameter"));
        return;
    }
    const std::string from = req->getParameter("from");
    const std::string to = req->getParameter("to");
    const std::string logger = req->getParameter("logger");

    auto stats = svc.get_stats(file, from, to, logger);
    Json by_level = Json::object();
    for (const auto& [k, v] : stats.by_level) {
        by_level[k] = v;
    }
    const Json resp = {
        {"by_level", by_level}, {"total", stats.total}, {"timespan", stats.timespan}};
    callback(json_response(drogon::k200OK, resp));
}

void LogCtrl::get_aggregate(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    LogService svc(dztrader::paths::logs());
    const std::string file = req->getParameter("file");
    if (file.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing file parameter"));
        return;
    }
    const std::string level = param_or(req, "level", "error");
    const auto limit_opt = parse_int_param(req, "limit", 20);
    if (!limit_opt.has_value()) {
        callback(error_response(drogon::k400BadRequest, "invalid limit parameter"));
        return;
    }
    const int limit = *limit_opt;

    auto agg = svc.get_aggregate(file, level, limit);
    Json arr = Json::array();
    for (const auto& a : agg) {
        arr.push_back({
            {"msg_pattern", a.msg_pattern}, {"count", a.count},
            {"first_ts", a.first_ts}, {"last_ts", a.last_ts},
            {"samples", a.samples}
        });
    }
    callback(json_response(drogon::k200OK, arr));
}

void LogCtrl::get_timeline(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    LogService svc(dztrader::paths::logs());
    const std::string file = req->getParameter("file");
    if (file.empty()) {
        callback(error_response(drogon::k400BadRequest, "missing file parameter"));
        return;
    }
    const std::string bucket = param_or(req, "bucket", "minute");

    auto timeline = svc.get_timeline(file, bucket);
    Json arr = Json::array();
    for (const auto& b : timeline) {
        Json counts = Json::object();
        for (const auto& [k, v] : b.counts) {
            counts[k] = v;
        }
        arr.push_back({{"ts", b.ts}, {"counts", counts}});
    }
    callback(json_response(drogon::k200OK, arr));
}

void LogCtrl::set_level(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "admin required"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "invalid JSON body"));
        return;
    }
    std::string level = body.value("level", "");
    if (!is_valid_level(level)) {
        callback(error_response(drogon::k400BadRequest, "invalid level: " + level));
        return;
    }
    // 规范化为 spdlog 全称（warn->warning, err->error），确保缓存/HTTP响应/SET patch 一致
    level = dztrader::platform::LogConfig::canonicalize_level(level);
    auto targets = body.value("targets", std::vector<std::string>{});
    if (targets.empty()) {
        callback(error_response(drogon::k400BadRequest, "targets must be non-empty"));
        return;
    }

    Json results = Json::array();
    for (const auto& target : targets) {
        // 统一分发（LogDomainService::handle_log_control）：dzweb 自身直调 LogConfig
        //（publish 回推 WS），其他进程写 SHM 帧。ok = 已应用/已写入事件通道（契约 rest §1）；
        // 生效以 RTN_LOG_CONFIG / log_config WS 推送为准
        const auto r = log_domain_->handle_log_control(
            target, DZ_FRAME_SET_LOG_CONFIG, {{"level", level}});
        results.push_back({
            {"name", target},
            {"ok", r == LogControlResult::Ok},
            {"old", nullptr},
            {"new", level}
        });
    }
    callback(json_response(drogon::k200OK, {{"results", results}}));
}

void LogCtrl::flush_log(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    if (!is_admin(req)) {
        callback(error_response(drogon::k403Forbidden, "admin required"));
        return;
    }
    Json body;
    try {
        body = Json::parse(req->getBody());
    } catch (...) {
        callback(error_response(drogon::k400BadRequest, "invalid JSON body"));
        return;
    }
    auto targets = body.value("targets", std::vector<std::string>{});
    if (targets.empty()) {
        callback(error_response(drogon::k400BadRequest, "targets must be non-empty"));
        return;
    }

    Json results = Json::array();
    for (const auto& target : targets) {
        const auto r = log_domain_->handle_log_control(
            target, DZ_FRAME_FLUSH_LOG, nlohmann::json::object());
        results.push_back({{"name", target}, {"ok", r == LogControlResult::Ok}});
    }
    callback(json_response(drogon::k200OK, {{"results", results}}));
}

}  // namespace dztrader::webui
