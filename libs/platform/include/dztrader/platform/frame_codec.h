#ifndef DZTRADER_PLATFORM_FRAME_CODEC_H_
#define DZTRADER_PLATFORM_FRAME_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/frame_codec.h>  // 复用 shm 纯机制版本

namespace dztrader::platform {

// T → JSON → 含 instance_id 帧（无 error 字段）。返回 true = 写入 + 唤醒订阅者成功。
template <typename T>
bool write_ext_inst_json(shm::MultiWriter& w, DzFrameType type,
                         std::string_view instance_id, const T& obj) {
    try {
        if (!shm::write_ext_inst_json(w, type, instance_id, obj)) {
            spdlog::error("frame write failed: type={} target={}",
                          static_cast<int>(type), instance_id);
            return false;
        }
        // 契约 shm: 写入任何帧都必须唤醒等待进程 (接收方阻塞在 NamedSemaphore::wait)
        w.notify_subscribers();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("frame serialize failed: type={} err={}",
                      static_cast<int>(type), e.what());
        return false;
    }
}

// T → JSON → 无 instance_id 帧。返回 true = 写入 + 唤醒订阅者成功。
template <typename T>
bool write_ext_json(shm::MultiWriter& w, DzFrameType type, const T& obj) {
    try {
        if (!shm::write_ext_json(w, type, obj)) {
            spdlog::error("frame write failed: type={}", static_cast<int>(type));
            return false;
        }
        // 契约 shm: 写入任何帧都必须唤醒等待进程 (接收方阻塞在 NamedSemaphore::wait)
        w.notify_subscribers();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("frame serialize failed: type={} err={}",
                      static_cast<int>(type), e.what());
        return false;
    }
}

// T → JSON → 含 instance_id 帧（可选 error 字段）。返回 true = 写入 + 唤醒订阅者成功。
template <typename T>
bool write_ext_inst_json_opt_error(shm::MultiWriter& w, DzFrameType type,
                                   std::string_view instance_id, const T& obj,
                                   std::optional<std::string> error) {
    try {
        nlohmann::json j = obj;
        if (error) j["error"] = *error;
        const auto str = j.dump();
        if (!w.write_ext_inst_frame(type, instance_id.data(),
                reinterpret_cast<const std::byte*>(str.data()),
                static_cast<uint32_t>(str.size()))) {
            spdlog::error("frame write failed: type={} target={} size={}",
                          static_cast<int>(type), instance_id, str.size());
            return false;
        }
        w.notify_subscribers();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("frame serialize failed: type={} err={}",
                      static_cast<int>(type), e.what());
        return false;
    }
}

// 已构造的 json 对象 → 含 instance_id 帧（可选 error，CTP 帧用）。
// 返回 true = 写入 + 唤醒订阅者成功。
inline bool write_ext_inst_json_obj(shm::MultiWriter& w, DzFrameType type,
                                    std::string_view instance_id,
                                    const nlohmann::json& payload,
                                    std::optional<std::string> error = std::nullopt) {
    try {
        nlohmann::json j = payload;
        if (error) j["error"] = *error;
        const auto str = j.dump();
        if (!w.write_ext_inst_frame(type, instance_id.data(),
                reinterpret_cast<const std::byte*>(str.data()),
                static_cast<uint32_t>(str.size()))) {
            spdlog::error("frame write failed: type={} target={} size={}",
                          static_cast<int>(type), instance_id, str.size());
            return false;
        }
        w.notify_subscribers();
        return true;
    } catch (const std::exception& e) {
        spdlog::error("frame serialize failed: type={} err={}",
                      static_cast<int>(type), e.what());
        return false;
    }
}

// 空/二进制 payload → 含 instance_id 帧（控制信号用，默认空 payload）。
// 返回 true = 写入 + 唤醒订阅者成功。
inline bool write_ext_inst_raw(shm::MultiWriter& w, DzFrameType type,
                               std::string_view instance_id,
                               const std::byte* data = nullptr, uint32_t size = 0) {
    if (!w.write_ext_inst_frame(type, instance_id.data(), data, size)) {
        spdlog::error("frame write failed: type={} target={}",
                      static_cast<int>(type), instance_id);
        return false;
    }
    w.notify_subscribers();
    return true;
}

// 空/二进制 payload → 无 instance_id 帧。返回 true = 写入 + 唤醒订阅者成功。
inline bool write_ext_raw(shm::MultiWriter& w, DzFrameType type,
                          const std::byte* data = nullptr, uint32_t size = 0) {
    if (!w.write_ext_frame(type, data, size)) {
        spdlog::error("frame write failed: type={}", static_cast<int>(type));
        return false;
    }
    w.notify_subscribers();
    return true;
}

// 定长结构体 → 无 instance_id 帧（走 Writer::write_frame<T>）。
// 返回 true = 写入 + 唤醒订阅者成功。
template <typename T>
bool write_struct(shm::MultiWriter& w, DzFrameType type, const T& payload) {
    if (!w.write_frame(type, payload)) {
        spdlog::error("struct frame write failed: type={} size={}",
                      static_cast<int>(type), sizeof(T));
        return false;
    }
    w.notify_subscribers();
    return true;
}

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_FRAME_CODEC_H_
