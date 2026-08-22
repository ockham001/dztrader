#ifndef DZTRADER_SHM_FRAME_CODEC_H_
#define DZTRADER_SHM_FRAME_CODEC_H_

#include <cstdint>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/frame_view.h>
#include <nlohmann/json.hpp>
#include <string_view>

namespace dztrader::shm {

template <typename T>
T decode_ext_inst_json(const FrameView& view) {
    const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
    return nlohmann::json::parse(data, data + view.ext_inst_payload_size()).get<T>();
}

template <typename T>
T decode_ext_json(const FrameView& view) {
    const auto* data = reinterpret_cast<const char*>(view.ext_payload());
    return nlohmann::json::parse(data, data + view.ext_payload_size()).get<T>();
}

template <AllowedWriteLock WriteLock, typename T>
bool write_ext_inst_json(Writer<WriteLock>& writer,
                         DzFrameType frame_type,
                         std::string_view instance_id,
                         const T& obj) {
    const auto json_str = nlohmann::json(obj).dump();
    return writer.write_ext_inst_frame(
        frame_type, instance_id.data(),
        reinterpret_cast<const std::byte*>(json_str.c_str()),
        static_cast<uint32_t>(json_str.size()));
}

template <AllowedWriteLock WriteLock, typename T>
bool write_ext_json(Writer<WriteLock>& writer,
                          DzFrameType frame_type,
                          const T& obj) {
    const auto json_str = nlohmann::json(obj).dump();
    return writer.write_ext_frame(
        frame_type,
        reinterpret_cast<const std::byte*>(json_str.c_str()),
        static_cast<uint32_t>(json_str.size()));
}

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_FRAME_CODEC_H_
