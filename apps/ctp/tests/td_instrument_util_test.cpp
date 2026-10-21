#include <gtest/gtest.h>
#include <dztrader/instrument_util.h>

namespace {

using dztrader::instrument::from_native_volume;
using dztrader::instrument::to_native_volume;
using dztrader::instrument::volume_step_of;

TEST(InstrumentUtilTest, StepSentinel) {
    EXPECT_DOUBLE_EQ(volume_step_of(0.0), 1.0);     // volume_step = 0 -> 视为 1
    EXPECT_DOUBLE_EQ(volume_step_of(-0.001), 1.0);  // 契约: <= 0 视为 1
    EXPECT_DOUBLE_EQ(volume_step_of(0.001), 0.001);
}

TEST(InstrumentUtilTest, ToNative) {
    EXPECT_DOUBLE_EQ(to_native_volume(0.0, 5), 5.0);      // step 0 -> 视为 1
    EXPECT_DOUBLE_EQ(to_native_volume(-0.001, 5), 5.0);   // 负步长 -> 视为 1
    EXPECT_DOUBLE_EQ(to_native_volume(0.001, 10), 0.01);  // 币圈: 1 平台单位 = 0.001 BTC
}

TEST(InstrumentUtilTest, FromNativeFloorToGrid) {
    EXPECT_EQ(from_native_volume(0.0, 7.9), 7);
    EXPECT_EQ(from_native_volume(0.0, -3.0), -3);      // 负数按同规则 (floor)
    EXPECT_EQ(from_native_volume(0.0, -3.5), -4);      // 负半格必须向下取整, 不是向零截断
    EXPECT_EQ(from_native_volume(0.001, 0.0105), 10);  // 0.0105/0.001=10.5 -> 向下取整 10
    // 1e-9 容差: 商为 9.999999999 时仍应落在 10 网格上 (去掉容差会退化为 9)
    EXPECT_EQ(from_native_volume(0.001, 0.009999999999), 10);
    EXPECT_EQ(from_native_volume(-0.001, 7.9), 7);  // 负步长 -> 视为 1
}

}  // namespace
