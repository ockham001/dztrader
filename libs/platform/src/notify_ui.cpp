#include <dztrader/platform/notify_ui.h>

#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/frame_codec.h>

namespace dztrader::platform {

namespace {

// DzNotifyLevel -> 字符串，与 log level 规范全称一致（见帧契约 01-log）
const char* notify_level_to_string(DzNotifyLevel level) {
    switch (level) {
        case DZ_NOTIFY_INFO:  return "info";
        case DZ_NOTIFY_WARN:  return "warning";
        case DZ_NOTIFY_ERROR: return "error";
        default:               return "error";
    }
}

}  // namespace

void NotifyUi::notify(DzNotifyLevel level, std::string msg, bool popup) {
    try {
        nlohmann::json payload = {
            {"source", source_},
            {"level", notify_level_to_string(level)},
            {"message", std::move(msg)},
            {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"popup", popup},
        };
        if (!shm::write_ext_json(writer_, DZ_FRAME_NOTIFY_UI, payload)) {
            spdlog::error("notify_ui frame write failed | source={}", source_);
            return;
        }
        writer_.notify_subscribers();
    } catch (const std::exception& e) {
        spdlog::error("notify_ui failed | source={} err=\"{}\"", source_, e.what());
    }
}

}  // namespace dztrader::platform
