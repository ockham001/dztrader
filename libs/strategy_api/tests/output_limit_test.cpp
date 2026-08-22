#include <dztrader/struct.h>

#include "output_limit.h"

#include <gtest/gtest.h>

namespace dztrader {

TEST(OutputLimitTest, OneMbPageYieldsExactCap) {
    EXPECT_EQ(output_ui_max_payload(1024 * 1024), 1048496u);  // (1MB-80)&~7
}

TEST(OutputLimitTest, MinimalFittablePageYieldsEmptyPayloadCap) {
    EXPECT_EQ(output_ui_max_payload(88), 8u);  // 80+8 恰好一帧空 payload
}

TEST(OutputLimitTest, SubMinimalPagesYieldZero) {
    EXPECT_EQ(output_ui_max_payload(87), 0u);
    EXPECT_EQ(output_ui_max_payload(80), 0u);
    EXPECT_EQ(output_ui_max_payload(79), 0u);
    EXPECT_EQ(output_ui_max_payload(0), 0u);
}

TEST(OutputLimitTest, HugePageClampedToHardCap) {
    EXPECT_EQ(output_ui_max_payload(1ull << 40), kOutputUiHardCap);
}

TEST(OutputLimitTest, CapAlwaysFitsPage) {
    for (uint64_t ps : {88ull, 96ull, 4096ull, 1ull << 20, (1ull << 20) + 80,
                        64ull << 20}) {
        const uint64_t cap = output_ui_max_payload(ps);
        const uint64_t padded = (cap + 7) & ~uint64_t{7};
        EXPECT_LE(padded + kOutputUiFrameOverhead, ps) << "page=" << ps;
    }
}

}  // namespace dztrader
