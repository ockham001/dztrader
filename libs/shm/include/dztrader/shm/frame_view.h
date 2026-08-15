#ifndef DZTRADER_SHM_FRAME_VIEW_H_
#define DZTRADER_SHM_FRAME_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <dztrader/struct.h>

namespace dztrader::shm {

class FrameView {
    const std::byte* frame_;

public:
    explicit FrameView(const std::byte* frame) noexcept : frame_(frame) {}

    const DzFrameHeader& header() const noexcept {
        return *reinterpret_cast<const DzFrameHeader*>(frame_);
    }

    DzFrameType type() const noexcept { return header().frame_type; }

    uint32_t frame_size() const noexcept { return header().frame_size; }

    template <typename T>
    const T& payload() const noexcept {
        static_assert(sizeof(T) % 8 == 0);
        return *reinterpret_cast<const T*>(frame_ + sizeof(DzFrameHeader));
    }

    const DzExtInstFrameHeader& ext_inst_header() const noexcept {
        return *reinterpret_cast<const DzExtInstFrameHeader*>(
            frame_ + sizeof(DzFrameHeader));
    }

    const std::byte* ext_inst_payload() const noexcept {
        return frame_ + sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader);
    }

    uint32_t ext_inst_payload_size() const noexcept {
        return ext_inst_header().data_size;
    }

    const char* ext_inst_id() const noexcept {
        return ext_inst_header().instance_id;
    }

    const DzExtFrameHeader& ext_header() const noexcept {
        return *reinterpret_cast<const DzExtFrameHeader*>(
            frame_ + sizeof(DzFrameHeader));
    }

    const std::byte* ext_payload() const noexcept {
        return frame_ + sizeof(DzFrameHeader) + sizeof(DzExtFrameHeader);
    }

    uint32_t ext_payload_size() const noexcept {
        return ext_header().data_size;
    }

    const DzExtStgFrameHeader& stg_header() const noexcept {
        return *reinterpret_cast<const DzExtStgFrameHeader*>(
            frame_ + sizeof(DzFrameHeader));
    }

    const std::byte* stg_payload() const noexcept {
        return frame_ + sizeof(DzFrameHeader) + sizeof(DzExtStgFrameHeader);
    }

    uint32_t stg_payload_size() const noexcept {
        return stg_header().data_size;
    }

    const char* stg_strategy_id() const noexcept {
        return stg_header().strategy_id;
    }
};

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_FRAME_VIEW_H_
