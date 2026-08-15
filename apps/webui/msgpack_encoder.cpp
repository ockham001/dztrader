#include "msgpack_encoder.h"

#include <msgpack.hpp>
#include <cstring>
#include <string>

namespace dztrader::webui {

namespace {

/// 将定长字符数组转为 std::string（去尾部 \0）
std::string fixed_to_string(const char* buf, size_t size) {
    size_t len = 0;
    while (len < size && buf[len] != '\0') { ++len; }
    return {buf, len};
}

}  // namespace

std::string encode_tick_msgpack(const DzTick& tick, uint64_t seq) {
    // API 注：msgpack-cxx 7.0.0 的 packer 默认构造已删除，且无 set_buffer 方法；
    // 必须通过构造函数传入 stream 指针。
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);

    // pack_str 仅接受 uint32_t 长度，需配合 pack_str_body 写入字符串体
    auto pack_str = [&pk](const std::string& s) {
        pk.pack_str(static_cast<uint32_t>(s.size()));
        pk.pack_str_body(s.data(), static_cast<uint32_t>(s.size()));
    };

    pk.pack_array(3);  // [msg_type, seq, payload_map]
    pk.pack_int(1);    // msg_type = tick
    pk.pack_uint64(seq);
    pk.pack_map(18);   // 18 个字段（14 标量 + 4 数组）

    pack_str("instrument_id"); pack_str(fixed_to_string(tick.instrument_id, sizeof(tick.instrument_id)));
    pack_str("date"); pk.pack_int32(tick.date);
    pack_str("time"); pk.pack_int32(tick.time);
    pack_str("last_price"); pk.pack_double(tick.last_price);
    pack_str("volume"); pk.pack_int32(tick.volume);
    pack_str("subseconds"); pk.pack_int32(tick.subseconds);
    pack_str("open_interest"); pk.pack_int64(tick.open_interest);
    pack_str("turnover"); pk.pack_double(tick.turnover);
    pack_str("pre_close_price"); pk.pack_double(tick.pre_close_price);
    pack_str("open_price"); pk.pack_double(tick.open_price);
    pack_str("highest_price"); pk.pack_double(tick.highest_price);
    pack_str("lowest_price"); pk.pack_double(tick.lowest_price);
    pack_str("upper_limit_price"); pk.pack_double(tick.upper_limit_price);
    pack_str("lower_limit_price"); pk.pack_double(tick.lower_limit_price);

    pack_str("bid_price"); pk.pack_array(5);
    for (int i = 0; i < 5; ++i) {
        pk.pack_double(tick.bid_price[i]);
    }
    pack_str("ask_price"); pk.pack_array(5);
    for (int i = 0; i < 5; ++i) {
        pk.pack_double(tick.ask_price[i]);
    }
    pack_str("bid_volume"); pk.pack_array(5);
    for (int i = 0; i < 5; ++i) {
        pk.pack_int32(tick.bid_volume[i]);
    }
    pack_str("ask_volume"); pk.pack_array(5);
    for (int i = 0; i < 5; ++i) {
        pk.pack_int32(tick.ask_volume[i]);
    }

    return {sbuf.data(), sbuf.size()};
}

}  // namespace dztrader::webui
