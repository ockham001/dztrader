#include <gtest/gtest.h>
#include <dztrader/instrument_util.h>
#include <dztrader/struct.h>

namespace {

using dztrader::instrument::volume_step_of;
using dztrader::instrument::to_native_volume;
using dztrader::instrument::from_native_volume;

TEST(InstrumentUtilTest, StepSentinel) {
    DzInstrumentInfo c{};          // volume_step = 0 -> 视为 1
    EXPECT_DOUBLE_EQ(volume_step_of(c), 1.0);
    c.volume_step = 0.001;
    EXPECT_DOUBLE_EQ(volume_step_of(c), 0.001);
}

TEST(InstrumentUtilTest, ToNative) {
    DzInstrumentInfo c{};          // step 1
    EXPECT_DOUBLE_EQ(to_native_volume(c, 5), 5.0);
    c.volume_step = 0.001;         // 币圈: 1 平台单位 = 0.001 BTC
    EXPECT_DOUBLE_EQ(to_native_volume(c, 10), 0.01);
}

TEST(InstrumentUtilTest, FromNativeFloorToGrid) {
    DzInstrumentInfo c{};
    EXPECT_EQ(from_native_volume(c, 7.9), 7);
    EXPECT_EQ(from_native_volume(c, -3.0), -3);     // 负数按同规则 (floor)
    c.volume_step = 0.001;
    EXPECT_EQ(from_native_volume(c, 0.0105), 10);   // 0.0105/0.001=10.5 -> 向下取整 10
}

}  // namespace
