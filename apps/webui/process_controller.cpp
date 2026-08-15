#include "process_controller.h"
#include <nlohmann/json.hpp>

namespace dztrader::webui {

void ProcessCtrl::get_all(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;  // 认证由 JWT advice 处理,本端点不要求 admin (所有登录用户可读)
    auto statuses = process_mirror_->get_all();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : statuses) {
        arr.push_back(s);  // platform::ProcessStatus 有手写 to_json (DZ_JSON_ENUM), 可直接转 JSON
    }
    callback(json_response(drogon::k200OK, {{"processes", arr}}));
}

}  // namespace dztrader::webui
