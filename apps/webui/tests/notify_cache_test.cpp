// NotifyCache 去重折叠单测（契约 notify-ui / ADR 0060 决策 4）。
//
// 覆盖口径：
// - 指纹归一化：订单号 / 时间戳 / 价格不影响指纹（这是「同一事件」的判据）
// - 窗口内合并计数、不新增条目
// - 窗口外、冷却内仍合并（同一故障的延续，避免窗口边界抖出两条）
// - 冷却外作为新事件重现（重置计数）
// - 不同 source / level / 文本 不合并
// - max_size 环形淘汰、count 自动补 1
#include "notify_cache.h"

#include <gtest/gtest.h>

namespace dztrader::webui {
namespace {

nlohmann::json make_payload(const std::string& source,
                            const std::string& level,
                            const std::string& message,
                            int64_t ts) {
    return nlohmann::json{{"source", source},
                          {"level", level},
                          {"message", message},
                          {"timestamp", ts},
                          {"popup", false}};
}

// === 归一化 ===

TEST(NotifyNormalizeTest, DigitsReplacedWithSinglePlaceholder) {
    EXPECT_EQ(NotifyCache::normalize("order 12345 rejected"), "order # rejected");
}

TEST(NotifyNormalizeTest, ConsecutiveDigitsCollapseToOne) {
    // 同一事件不同订单号 → 同一归一化文本
    EXPECT_EQ(NotifyCache::normalize("order 1 rejected"),
              NotifyCache::normalize("order 99999 rejected"));
}

TEST(NotifyNormalizeTest, TimeAndPriceTokensNormalized) {
    // 时间串 14:05:23 / 价格 1,234.56 / 日期 2026-11-27 都被收敛成一个 #
    EXPECT_EQ(NotifyCache::normalize("at 14:05:23"), "at #");
    EXPECT_EQ(NotifyCache::normalize("price 1,234.56"), "price #");
    EXPECT_EQ(NotifyCache::normalize("date 2026-11-27"), "date #");
}

TEST(NotifyNormalizeTest, NonDigitsPreserved) {
    // 中文与标点原样保留：归一化只动数字，不得过度合并不同事件
    EXPECT_EQ(NotifyCache::normalize("CTP:平今仓位不足"), "CTP:平今仓位不足");
}

// === 指纹 ===

TEST(NotifyFingerprintTest, SameEventDifferentIdSameFingerprint) {
    const auto a = make_payload("dztd_ctp", "warning", "下单被拒：cu2412 错误码 30", 100);
    const auto b = make_payload("dztd_ctp", "warning", "下单被拒：cu2412 错误码 31", 200);
    EXPECT_EQ(NotifyCache::fingerprint(a), NotifyCache::fingerprint(b));
}

TEST(NotifyFingerprintTest, DifferentSourceDiffersFingerprint) {
    const auto a = make_payload("dztd_ctp", "warning", "连接失败", 100);
    const auto b = make_payload("dzmd_ctp", "warning", "连接失败", 100);
    EXPECT_NE(NotifyCache::fingerprint(a), NotifyCache::fingerprint(b));
}

TEST(NotifyFingerprintTest, DifferentLevelDiffersFingerprint) {
    // ADR 0060 决策 2：同一事件只有一个级别；级别不同即不同事件（不会互相折叠）
    const auto a = make_payload("dztd_ctp", "warning", "连接异常", 100);
    const auto b = make_payload("dztd_ctp", "error", "连接异常", 100);
    EXPECT_NE(NotifyCache::fingerprint(a), NotifyCache::fingerprint(b));
}

TEST(NotifyFingerprintTest, EmptyPayloadReturnsEmptyFingerprint) {
    EXPECT_TRUE(NotifyCache::fingerprint(nlohmann::json::object()).empty());
}

// === 折叠 ===

TEST(NotifyCacheFoldTest, FirstOccurrenceIsNotFolded) {
    NotifyCache cache(100, 60, 300);
    const auto p = make_payload("dztd_ctp", "warning", "平今仓位不足 1", 1000);
    EXPECT_FALSE(cache.try_fold(p, 1000).has_value());
    cache.add(p);
    ASSERT_EQ(cache.get_all().size(), 1U);
    EXPECT_EQ(cache.get_all()[0].value("count", 0), 1);
}

TEST(NotifyCacheFoldTest, WithinWindowMergesAndIncrementsCount) {
    NotifyCache cache(100, 60, 300);
    const auto p1 = make_payload("dztd_ctp", "warning", "平今仓位不足 1", 1000);
    cache.add(p1);
    // 30 秒后同一事件（订单号不同）→ 命中折叠
    const auto p2 = make_payload("dztd_ctp", "warning", "平今仓位不足 2", 1030);
    const auto folded = cache.try_fold(p2, 1030);
    ASSERT_TRUE(folded.has_value());
    EXPECT_EQ(folded->value("count", 0), 2);
    EXPECT_EQ(folded->value("timestamp", static_cast<int64_t>(0)), 1030);  // 最后发生时间更新
    EXPECT_EQ(cache.get_all().size(), 1U);                                 // 不新增条目
}

TEST(NotifyCacheFoldTest, WithinWindowDoesNotAddNewEntry) {
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("dztd_ctp", "warning", "错误 1", 1000));
    const auto folded = cache.try_fold(make_payload("dztd_ctp", "warning", "错误 2", 1030), 1030);
    ASSERT_TRUE(folded.has_value());  // 命中折叠：调用方不应再 add
    EXPECT_EQ(cache.get_all().size(), 1U);
    EXPECT_EQ(cache.get_all()[0].value("count", 0), 2);
}

TEST(NotifyCacheFoldTest, BeyondWindowButWithinCooldownStillMerges) {
    // 窗口 60s 已过、冷却 300s 未到：同一故障的延续，仍合并（避免窗口边界抖出两条）
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("dztd_ctp", "error", "持久化失败 1", 1000));
    const auto folded =
        cache.try_fold(make_payload("dztd_ctp", "error", "持久化失败 2", 1200), 1200);
    ASSERT_TRUE(folded.has_value());
    EXPECT_EQ(folded->value("count", 0), 2);
    EXPECT_EQ(cache.get_all().size(), 1U);
}

TEST(NotifyCacheFoldTest, SustainedFailureCadenceFoldsAndAccumulatesCount) {
    // #530 M1 回归：td_persist_writer 的持续通知节奏（60s）必须小于冷却（300s）时，
    // 同一故障折叠成一条并持续累加 count（每来一条 +1），不得每 5 分钟冒一条新条目。
    NotifyCache cache(100, 60, 300);
    const auto payload = [](int64_t ts) {
        return make_payload("dztd_ctp", "error",
                            "trade db persist failed | path=/x error=\"disk full\"", ts);
    };
    cache.add(payload(1000));
    for (int i = 1; i <= 5; ++i) {
        const int64_t ts = 1000 + 60 * i;
        const auto folded = cache.try_fold(payload(ts), ts);
        ASSERT_TRUE(folded.has_value()) << "i=" << i;
        EXPECT_EQ(folded->value("count", 0), i + 1);
    }
    ASSERT_EQ(cache.get_all().size(), 1U);
    EXPECT_EQ(cache.get_all()[0].value("count", 0), 6);
}

TEST(NotifyCacheFoldTest, BeyondCooldownBecomesNewEvent) {
    // 冷却 300s 已过：允许作为新条目重现，计数重置
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("dztd_ctp", "error", "持久化失败 1", 1000));
    const auto folded =
        cache.try_fold(make_payload("dztd_ctp", "error", "持久化失败 2", 1400), 1400);
    EXPECT_FALSE(folded.has_value());  // 不折叠 → 调用方 add() 新增一条
    cache.add(make_payload("dztd_ctp", "error", "持久化失败 2", 1400));
    EXPECT_EQ(cache.get_all().size(), 2U);
    EXPECT_EQ(cache.get_all()[1].value("count", 0), 1);  // 新条目计数重置
}

TEST(NotifyCacheFoldTest, DifferentEventsDoNotMerge) {
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("dztd_ctp", "warning", "下单被拒", 1000));
    const auto folded = cache.try_fold(make_payload("dztd_ctp", "warning", "撤单被拒", 1010), 1010);
    EXPECT_FALSE(folded.has_value());
}

TEST(NotifyCacheFoldTest, EmptySourceStillFoldsOnText) {
    // source 缺失但文本相同：仍按消息内容折叠（剥掉空 source 后的指纹一致）
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("", "error", "未知错误 1", 1000));
    const auto folded = cache.try_fold(make_payload("", "error", "未知错误 2", 1010), 1010);
    ASSERT_TRUE(folded.has_value());
    EXPECT_EQ(folded->value("count", 0), 2);
}

// === 开关与上限 ===

TEST(NotifyCacheFoldTest, DisabledWhenWindowNonPositive) {
    NotifyCache cache(100, 0, 300);  // 去重关闭
    cache.add(make_payload("dztd_ctp", "warning", "错误 1", 1000));
    EXPECT_FALSE(
        cache.try_fold(make_payload("dztd_ctp", "warning", "错误 2", 1010), 1010).has_value());
}

TEST(NotifyCacheFoldTest, DisabledWhenMaxSizeZero) {
    NotifyCache cache(0, 60, 300);  // 缓存禁用
    EXPECT_FALSE(cache.try_fold(make_payload("a", "error", "x 1", 1), 1).has_value());
    cache.add(make_payload("a", "error", "x 1", 1));
    EXPECT_TRUE(cache.get_all().empty());
}

TEST(NotifyCacheFoldTest, MaxSizeEvictsOldest) {
    NotifyCache cache(2, 60, 300);
    cache.add(make_payload("s", "info", "消息 1", 1000));
    cache.add(make_payload("s", "info", "消息 2", 1001));
    cache.add(make_payload("s", "info", "消息 3", 1002));
    ASSERT_EQ(cache.get_all().size(), 2U);
    EXPECT_EQ(cache.get_all()[0].value("message", ""), "消息 2");
    EXPECT_EQ(cache.get_all()[1].value("message", ""), "消息 3");
}

TEST(NotifyCacheFoldTest, SetDedupUpdatesParameters) {
    NotifyCache cache(100, 60, 300);
    cache.add(make_payload("s", "error", "故障 1", 1000));
    // 关闭去重后不再折叠
    cache.set_dedup(0, 300);
    EXPECT_FALSE(cache.try_fold(make_payload("s", "error", "故障 2", 1010), 1010).has_value());
}

}  // namespace
}  // namespace dztrader::webui
