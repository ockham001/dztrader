#include <dztrader/core/random.h>

#include <gtest/gtest.h>

#include <set>

using namespace dztrader::core;

TEST(RandomJitterTest, WithinRange) {
    for (int i = 0; i < 100; ++i) {
        auto delay = random_jitter(0, 5000);
        EXPECT_GE(delay.count(), 0);
        EXPECT_LE(delay.count(), 5000);
    }
}

TEST(RandomJitterTest, ProducesVariedValues) {
    // 100 次调用至少产生 10 个不同值 (概率极高)
    std::set<int> values;
    for (int i = 0; i < 100; ++i) {
        values.insert(static_cast<int>(random_jitter(0, 5000).count()));
    }
    EXPECT_GE(values.size(), 10);
}

TEST(RandomJitterTest, SingleValueRange) {
    auto delay = random_jitter(100, 100);
    EXPECT_EQ(delay.count(), 100);
}
