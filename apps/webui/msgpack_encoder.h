#ifndef DZTRADER_WEBUI_MSGPACK_ENCODER_H_
#define DZTRADER_WEBUI_MSGPACK_ENCODER_H_

#include <dztrader/struct.h>
#include <string>
#include <cstdint>

namespace dztrader::webui {

/// 将 DzTick 编码为 msgpack array: [1, seq, {field: value}]
/// @param tick 行情数据
/// @param seq 服务端推送序列号
/// @return msgpack 字节流
std::string encode_tick_msgpack(const DzTick& tick, uint64_t seq);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MSGPACK_ENCODER_H_
