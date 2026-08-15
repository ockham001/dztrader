#ifndef DZTRADER_WEBUI_LOG_CONTROLLER_H_
#define DZTRADER_WEBUI_LOG_CONTROLLER_H_

#include <drogon/HttpController.h>
#include "controller_base.h"
#include "log_domain_service.h"

#include <memory>

namespace dztrader::webui {

class LogCtrl : public drogon::HttpController<LogCtrl, false>,
                public ControllerBase {
public:
    explicit LogCtrl(std::shared_ptr<Repository> repo,
                     std::shared_ptr<LogDomainService> log_domain)
        : ControllerBase(std::move(repo)),
          log_domain_(std::move(log_domain)) {}

public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(LogCtrl::get_files,      "/api/logs/files",      drogon::Get);
        ADD_METHOD_TO(LogCtrl::get_content,    "/api/logs/content",    drogon::Get);
        ADD_METHOD_TO(LogCtrl::get_stats,      "/api/logs/stats",      drogon::Get);
        ADD_METHOD_TO(LogCtrl::get_aggregate,  "/api/logs/aggregate",  drogon::Get);
        ADD_METHOD_TO(LogCtrl::get_timeline,   "/api/logs/timeline",   drogon::Get);
        ADD_METHOD_TO(LogCtrl::set_level,      "/api/logs/level",      drogon::Post);
        ADD_METHOD_TO(LogCtrl::flush_log,      "/api/logs/flush",      drogon::Post);
    METHOD_LIST_END

    void get_files(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get_content(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get_stats(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get_aggregate(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get_timeline(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void set_level(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void flush_log(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    /// 日志领域服务（自 P4 Task 6 归并：set_level/flush 经 handle_log_control 分发，
    /// 内部直调 LogConfig + 写 SHM + NOTIFY_UI + publish 回推，不再经 log_control 自由函数）
    std::shared_ptr<LogDomainService> log_domain_;

    /// Validate that level is a valid log level (lowercase, per contract 00-log.md).
    static bool is_valid_level(const std::string& level);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_LOG_CONTROLLER_H_
