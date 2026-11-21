#ifndef DZTRADER_CTP_TESTS_SHM_FRAME_READER_H_
#define DZTRADER_CTP_TESTS_SHM_FRAME_READER_H_

// td 会话层测试共用的 SHM 帧读取/解码工具（`td_position_lifecycle_test` /
// `td_place_order_validation_test`）。
//
// 为什么共享：两个测试都要「先建 `shm::Reader` 再从同一通道读帧断言写端行为」，
// 各写一份会漂移（unit-test-standard §5.4）。解码统一走生产读端同款 `shm::FrameView`
// （`type()` / `payload<T>()`），不手写 `reinterpret_cast<DzFrameHeader>` + `memcpy`。
//
// **用法约束**：`Reader::create` 从创建时刻的写位置开始读，测试必须在首个被测写操作
// **之前**建好 Reader，否则「没有帧被写入」的断言会退化成空转（读到 0 帧恒真）。

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/struct.h>  // DZ_FRAME_* / DzPositionInfo / DzTradeReport

namespace dztrader::test {

/// 读出自 `reader` 当前读位置以来的全部帧（header + payload 完整拷贝，写入顺序）。
inline std::vector<std::unique_ptr<std::byte[]>> read_frames(shm::Reader& reader) {
    std::vector<std::unique_ptr<std::byte[]>> frames;
    while (const std::byte* raw = reader.next_frame()) {
        const auto frame_size = shm::FrameView{raw}.header().frame_size;
        auto copy = std::make_unique<std::byte[]>(frame_size);
        std::memcpy(copy.get(), raw, frame_size);
        frames.push_back(std::move(copy));
    }
    return frames;
}

/// 帧副本里的持仓合约代码（`DZ_FRAME_POSITION_INFO`，按出现顺序）。
inline std::vector<std::string> position_frame_instruments(
    const std::vector<std::unique_ptr<std::byte[]>>& frames) {
    std::vector<std::string> out;
    for (const auto& frame : frames) {
        const shm::FrameView view{frame.get()};
        if (view.type() != DZ_FRAME_POSITION_INFO) continue;
        out.emplace_back(view.payload<DzPositionInfo>().instrument_id);
    }
    return out;
}

/// 帧副本里的成交合约代码（`DZ_FRAME_TRADE_REPORT`，按出现顺序）。
inline std::vector<std::string> trade_frame_instruments(
    const std::vector<std::unique_ptr<std::byte[]>>& frames) {
    std::vector<std::string> out;
    for (const auto& frame : frames) {
        const shm::FrameView view{frame.get()};
        if (view.type() != DZ_FRAME_TRADE_REPORT) continue;
        out.emplace_back(view.payload<DzTradeReport>().instrument_id);
    }
    return out;
}

}  // namespace dztrader::test

#endif  // DZTRADER_CTP_TESTS_SHM_FRAME_READER_H_
