#include <gtest/gtest.h>
#include <dztrader/instrument_util.h>

namespace {

using dztrader::instrument::volume_step_of;
using dztrader::instrument::to_native_volume;
using dztrader::instrument::from_native_volume;

TEST(InstrumentUtilTest, StepSentinel) {
    EXPECT_DOUBLE_EQ(volume_step_of(0.0), 1.0);     // volume_step = 0 -> 视为 1
    EXPECT_DOUBLE_EQ(volume_step_of(0.001), 0.001);
}

TEST(InstrumentUtilTest, ToNative) {
    EXPECT_DOUBLE_EQ(to_native_volume(0.0, 5), 5.0);      // step 0 -> 视为 1
    EXPECT_DOUBLE_EQ(to_native_volume(0.001, 10), 0.01);  // 币圈: 1 平台单位 = 0.001 BTC
}

TEST(InstrumentUtilTest, FromNativeFloorToGrid) {
    EXPECT_EQ(from_native_volume(0.0, 7.9), 7);
    EXPECT_EQ(from_native_volume(0.0, -3.0), -3);      // 负数按同规则 (floor)
    EXPECT_EQ(from_native_volume(0.001, 0.0105), 10);  // 0.0105/0.001=10.5 -> 向下取整 10
}

}  // namespace
