#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <dztrader/struct.h>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "msgpack_encoder.h"

using dztrader::webui::encode_tick_msgpack;

TEST(MsgpackEncoderTest, EncodesTickWithFields) {
    DzTick tick{};
    std::strncpy(tick.instrument_id, "rb2510", sizeof(tick.instrument_id));
    tick.date = 20000;
    tick.time = 33000;
    tick.last_price = 3500.5;
    tick.volume = 100000;
    tick.subseconds = 500000;
    tick.open_interest = 500000;
    tick.turnover = 350050000.0;
    tick.pre_close_price = 3490.0;
    tick.open_price = 3495.0;
    tick.highest_price = 3510.0;
    tick.lowest_price = 3488.0;
    tick.upper_limit_price = 3850.0;
    tick.lower_limit_price = 3150.0;
    for (int i = 0; i < 5; ++i) {
        tick.bid_price[i] = 3499.0 - i;
        tick.ask_price[i] = 3501.0 + i;
        tick.bid_volume[i] = 100 - (i * 10);
        tick.ask_volume[i] = 200 - (i * 20);
    }

    std::string packed = encode_tick_msgpack(tick, 42);

    const msgpack::object_handle oh = msgpack::unpack(packed.data(), packed.size());
    const msgpack::object obj = oh.get();
    ASSERT_EQ(obj.type, msgpack::type::ARRAY);
    ASSERT_EQ(obj.via.array.size, 3u);
    EXPECT_EQ(obj.via.array.ptr[0].as<int>(), 1);         // msg_type = tick
    EXPECT_EQ(obj.via.array.ptr[1].as<uint64_t>(), 42u);  // seq

    auto payload = obj.via.array.ptr[2].as<std::map<std::string, msgpack::object>>();
    EXPECT_EQ(payload["instrument_id"].as<std::string>(), "rb2510");
    EXPECT_EQ(payload["date"].as<int32_t>(), 20000);
    EXPECT_EQ(payload["time"].as<int32_t>(), 33000);
    EXPECT_DOUBLE_EQ(payload["last_price"].as<double>(), 3500.5);
    EXPECT_EQ(payload["volume"].as<int32_t>(), 100000);
    EXPECT_EQ(payload["subseconds"].as<int32_t>(), 500000);
    EXPECT_EQ(payload["open_interest"].as<int64_t>(), 500000);
    EXPECT_DOUBLE_EQ(payload["turnover"].as<double>(), 350050000.0);
    EXPECT_DOUBLE_EQ(payload["pre_close_price"].as<double>(), 3490.0);
    EXPECT_DOUBLE_EQ(payload["open_price"].as<double>(), 3495.0);
    EXPECT_DOUBLE_EQ(payload["highest_price"].as<double>(), 3510.0);
    EXPECT_DOUBLE_EQ(payload["lowest_price"].as<double>(), 3488.0);
    EXPECT_DOUBLE_EQ(payload["upper_limit_price"].as<double>(), 3850.0);
    EXPECT_DOUBLE_EQ(payload["lower_limit_price"].as<double>(), 3150.0);

    auto bid_price = payload["bid_price"].as<std::vector<double>>();
    ASSERT_EQ(bid_price.size(), 5u);
    EXPECT_DOUBLE_EQ(bid_price[0], 3499.0);
    EXPECT_DOUBLE_EQ(bid_price[4], 3495.0);

    auto ask_price = payload["ask_price"].as<std::vector<double>>();
    ASSERT_EQ(ask_price.size(), 5u);
    EXPECT_DOUBLE_EQ(ask_price[0], 3501.0);
    EXPECT_DOUBLE_EQ(ask_price[4], 3505.0);

    auto bid_volume = payload["bid_volume"].as<std::vector<int32_t>>();
    ASSERT_EQ(bid_volume.size(), 5u);
    EXPECT_EQ(bid_volume[0], 100);
    EXPECT_EQ(bid_volume[4], 60);

    auto ask_volume = payload["ask_volume"].as<std::vector<int32_t>>();
    ASSERT_EQ(ask_volume.size(), 5u);
    EXPECT_EQ(ask_volume[0], 200);
    EXPECT_EQ(ask_volume[4], 120);
}

TEST(MsgpackEncoderTest, EncodesBoundaryValuesAndFullLengthInstrumentId) {
    DzTick tick{};
    // 满长 88 字节且无 NUL：fixed_to_string 必须读满 size，不得越界找 NUL
    const std::string full_id(88, 'X');
    std::memcpy(tick.instrument_id, full_id.data(), full_id.size());
    tick.last_price = -3500.5;  // 负价格合法（如原油期货）
    tick.volume = -1;
    tick.subseconds = 0;
    tick.open_interest = -9007199254740993LL;
    tick.turnover = -1.5;
    tick.highest_price = 0.0;
    for (int i = 0; i < 5; ++i) {
        tick.bid_price[i] = -static_cast<double>(i);
        tick.ask_price[i] = -100.0 - static_cast<double>(i);
        tick.bid_volume[i] = -i;
        tick.ask_volume[i] = 0;
    }

    const uint64_t seq = std::numeric_limits<uint64_t>::max();
    const std::string packed = encode_tick_msgpack(tick, seq);

    const msgpack::object_handle oh = msgpack::unpack(packed.data(), packed.size());
    const msgpack::object obj = oh.get();
    ASSERT_EQ(obj.type, msgpack::type::ARRAY);
    ASSERT_EQ(obj.via.array.size, 3u);
    EXPECT_EQ(obj.via.array.ptr[1].as<uint64_t>(), seq);

    auto payload = obj.via.array.ptr[2].as<std::map<std::string, msgpack::object>>();
    EXPECT_EQ(payload["instrument_id"].as<std::string>(), full_id);
    EXPECT_EQ(payload["instrument_id"].as<std::string>().size(), 88u);
    EXPECT_DOUBLE_EQ(payload["last_price"].as<double>(), -3500.5);
    EXPECT_EQ(payload["volume"].as<int32_t>(), -1);
    EXPECT_EQ(payload["subseconds"].as<int32_t>(), 0);
    EXPECT_EQ(payload["open_interest"].as<int64_t>(), -9007199254740993LL);
    EXPECT_DOUBLE_EQ(payload["turnover"].as<double>(), -1.5);
    EXPECT_DOUBLE_EQ(payload["highest_price"].as<double>(), 0.0);

    const auto bid_price = payload["bid_price"].as<std::vector<double>>();
    const auto ask_price = payload["ask_price"].as<std::vector<double>>();
    const auto bid_volume = payload["bid_volume"].as<std::vector<int32_t>>();
    const auto ask_volume = payload["ask_volume"].as<std::vector<int32_t>>();
    ASSERT_EQ(bid_price.size(), 5u);
    ASSERT_EQ(ask_price.size(), 5u);
    ASSERT_EQ(bid_volume.size(), 5u);
    ASSERT_EQ(ask_volume.size(), 5u);
    EXPECT_DOUBLE_EQ(bid_price[0], 0.0);
    EXPECT_DOUBLE_EQ(bid_price[4], -4.0);
    EXPECT_DOUBLE_EQ(ask_price[0], -100.0);
    EXPECT_DOUBLE_EQ(ask_price[4], -104.0);
    EXPECT_EQ(bid_volume[0], 0);
    EXPECT_EQ(bid_volume[4], -4);
    EXPECT_EQ(ask_volume[4], 0);
}

TEST(MsgpackEncoderTest, PreservesNonAsciiInstrumentId) {
    DzTick tick{};
    const std::string id = "沪深300";
    std::memcpy(tick.instrument_id, id.data(), id.size());  // 其余字节为 0
    const std::string packed = encode_tick_msgpack(tick, 7);
    const msgpack::object_handle oh = msgpack::unpack(packed.data(), packed.size());
    auto payload = oh.get().via.array.ptr[2].as<std::map<std::string, msgpack::object>>();
    EXPECT_EQ(payload["instrument_id"].as<std::string>(), id);
}

TEST(MsgpackEncoderTest, StripsTrailingNullsFromInstrumentId) {
    DzTick tick{};
    std::strncpy(tick.instrument_id, "IF2506", sizeof(tick.instrument_id));
    std::string packed = encode_tick_msgpack(tick, 1);
    const msgpack::object_handle oh = msgpack::unpack(packed.data(), packed.size());
    auto payload = oh.get().via.array.ptr[2].as<std::map<std::string, msgpack::object>>();
    EXPECT_EQ(payload["instrument_id"].as<std::string>(), "IF2506");
}
