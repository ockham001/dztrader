#include "notify_controller.h"

namespace dztrader::webui {

void NotifyCtrl::get_recent(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {  // NOLINT
    (void)req;
    if (!notify_cache_) {
        callback(json_response(drogon::k200OK, {{"notifications", nlohmann::json::array()}}));
        return;
    }
    auto msgs = notify_cache_->get_all();
    nlohmann::json arr = nlohmann::json::array();
    for (auto& m : msgs) {
        arr.push_back(std::move(m));
    }
    callback(json_response(drogon::k200OK, {{"notifications", std::move(arr)}}));
}

}  // namespace dztrader::webui
