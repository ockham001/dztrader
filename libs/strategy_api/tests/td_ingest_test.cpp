#include <gtest/gtest.h>

#include <dztrader/trading/td_ingest.h>

#include <optional>
#include <string>

namespace {

using dztrader::TdIngestGate;

// spec §5.1: W 过滤 — seq ≤ W 跳过 (快照已含), seq > W 放行推进, 已应用 seq 防重。
TEST(TdIngestGate, WatermarkFilter) {
    TdIngestGate g;
    g.set_watermark("A", 100);
    EXPECT_EQ(TdIngestGate::Verdict::kSkip, g.admit("A", 100));  // spec: seq≤W 跳过
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("A", 101));
    EXPECT_EQ(TdIngestGate::Verdict::kSkip, g.admit("A", 101));  // last_applied 防重
}

// spec §5.2: 断档 — 首帧 seq > W+1 时标记 gap, 且只报一次 (take 后清空)。
TEST(TdIngestGate, GapDetectedOnFirstFrameOnly) {
    TdIngestGate g;
    g.set_watermark("A", 100);
    (void)g.admit("A", 103);  // 101/102 缺 → gap
    auto gap = g.take_pending_gap();
    ASSERT_TRUE(gap);
    EXPECT_EQ("A", gap->account_id);
    EXPECT_EQ(101u, gap->from);
    EXPECT_EQ(102u, gap->to);
    EXPECT_FALSE(g.take_pending_gap());  // 只报一次
}

// spec §5.5: 倒退重置 — seq < last_applied 检测; 重置后新水位下正常应用。
TEST(TdIngestGate, ResetDetectedOnRegression) {
    TdIngestGate g;
    g.set_watermark("A", 5000);
    (void)g.admit("A", 5001);
    EXPECT_TRUE(g.detect_reset("A", 3));  // spec §5.5 倒退
    g.reset_account("A", 2);
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("A", 3));  // 新水位下正常应用
}

// 倒退检测: 无已应用基准 (首帧/从未见过) 不误报; 多账户隔离。
TEST(TdIngestGate, ResetNotDetectedWithoutAppliedBaseline) {
    TdIngestGate g;
    EXPECT_FALSE(g.detect_reset("A", 3));  // 无基准
    g.set_watermark("A", 100);
    EXPECT_FALSE(g.detect_reset("A", 3));  // 未应用过帧
    (void)g.admit("A", 101);
    EXPECT_FALSE(g.detect_reset("A", 101));  // 相等不算倒退
    EXPECT_TRUE(g.detect_reset("A", 100));
    EXPECT_FALSE(g.detect_reset("B", 500));  // 他账户不受影响
}

// spec §5.4: 成交去重二道防线 — (account_id, trading_day, trade_id) 存在性。
// 跨日同 trade_id 必须放行 (新唯一键同语义)。
TEST(TdIngestGate, TradeDedupSecondLine) {
    TdIngestGate g;
    EXPECT_TRUE(g.admit_trade("A", "20260901", "T1"));
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));
    EXPECT_TRUE(g.admit_trade("A", "20260902", "T1"));  // 跨日共存 (trades 新唯一键同语义)
    EXPECT_TRUE(g.admit_trade("B", "20260901", "T1"));  // 账户隔离
}

// spec §5.4: 交易日切换清理 — 新 day 清旧段, 不被旧段拦截。
TEST(TdIngestGate, TradeDedupClearedOnTradingDayChange) {
    TdIngestGate g;
    (void)g.admit_trade("A", "20260901", "T1");
    g.on_trading_day_changed("A", "20260902");  // 新 day → 清旧段
    EXPECT_TRUE(g.admit_trade("A", "20260902", "T1"));  // 不被旧段拦截
}

// spec §5.4: admit_trade 自身检测交易日变化即自动清旧段 (集合增长有界)。
TEST(TdIngestGate, TradeDedupAutoCleanOnDayChangeInAdmitTrade) {
    TdIngestGate g;
    (void)g.admit_trade("A", "20260901", "T1");
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));
    EXPECT_TRUE(g.admit_trade("A", "20260902", "T1"));  // 新 day 自动清旧段
    // 旧段已清: 跨日同 trade_id 共存
    EXPECT_FALSE(g.admit_trade("A", "20260902", "T1"));
    EXPECT_TRUE(g.admit_trade("B", "20260901", "T1"));  // 他账户旧日段不受影响
}

// reset_account 清掉该账户去重段 (重置 = 新基准, spec §5.5)。
TEST(TdIngestGate, ResetClearsTradeDedup) {
    TdIngestGate g;
    (void)g.admit_trade("A", "20260901", "T1");
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));
    g.reset_account("A", 10);
    EXPECT_TRUE(g.admit_trade("A", "20260901", "T1"));  // 重置后重建集合
}

// 评审发现 1 (Task 8 Fix): 重置后重新 admit — 成交去重段已清 (reset_account),
// 同 (account, day, trade_id) 在重置后再次 admit_trade 必须放行。
TEST(TdIngestGate, TradeDedupReAdmitAfterReset) {
    TdIngestGate g;
    (void)g.admit_trade("A", "20260901", "T1");
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));
    g.reset_account("A", 5);
    EXPECT_TRUE(g.admit_trade("A", "20260901", "T1"));  // 重置后重建集合, 不拦截
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));  // 重建后的二道防线仍生效
}

// 评审发现 1: on_trading_day_changed 显式调用后, 旧日段清空 — 新日同 trade_id 放行,
// 且旧日同 trade_id 不再拦截新日 (与 admit_trade 内部自清一致)。
TEST(TdIngestGate, TradeDedupExplicitDayChangeAllowsSameTradeAcrossDays) {
    TdIngestGate g;
    (void)g.admit_trade("A", "20260901", "T1");
    EXPECT_FALSE(g.admit_trade("A", "20260901", "T1"));
    g.on_trading_day_changed("A", "20260902");
    EXPECT_TRUE(g.admit_trade("A", "20260902", "T1"));  // 显式切换后新日放行
    EXPECT_FALSE(g.admit_trade("A", "20260902", "T1"));  // 新日段正常去重
}

// 首帧 seq == W+1 (无空洞) 不记 gap; seq ≤ W 的首帧不记 gap。
TEST(TdIngestGate, NoGapWhenSequentialOrBelowWatermark) {
    TdIngestGate g;
    g.set_watermark("A", 100);
    (void)g.admit("A", 101);
    EXPECT_FALSE(g.take_pending_gap());

    TdIngestGate g2;
    g2.set_watermark("B", 100);
    (void)g2.admit("B", 90);  // 快照已含, 不算断档
    EXPECT_FALSE(g2.take_pending_gap());
}

// 无水位账户 (DB 库不可用降级 / 账户无快照): 不过滤全放行, 仅 last_applied 防重。
// 这是"打开失败降级为全放行"的 gate 语义 (否则 seq=0 帧被误过滤)。
TEST(TdIngestGate, NoWatermarkPassesThrough) {
    TdIngestGate g;
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("A", 0));
    EXPECT_EQ(TdIngestGate::Verdict::kSkip, g.admit("A", 0));  // 仅 last_applied 防重
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("A", 1));
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("B", 5));  // 他账户独立
}

// 设水位后, 未设水位账户的放行不受影响 (隔离)。
TEST(TdIngestGate, WatermarkDoesNotAffectOtherAccounts) {
    TdIngestGate g;
    g.set_watermark("A", 100);
    EXPECT_EQ(TdIngestGate::Verdict::kApply, g.admit("B", 0));  // B 无快照全放行
    EXPECT_EQ(TdIngestGate::Verdict::kSkip, g.admit("A", 100));
}

// 前缀账户去重段隔离: 清 "ctp1" 不得误删 "ctp12" 的段 (段键 = account_id + '\x1f' + day,
// 裸前缀匹配会让 "ctp12\x1f..." 也以 "ctp1" 开头)。
TEST(TdIngestGate, TradeDedupSegmentsIsolatedForPrefixAccounts) {
    TdIngestGate g;
    EXPECT_TRUE(g.admit_trade("ctp1", "20260901", "T1"));
    EXPECT_TRUE(g.admit_trade("ctp12", "20260901", "T1"));
    g.on_trading_day_changed("ctp1", "20260902");
    EXPECT_FALSE(g.admit_trade("ctp12", "20260901", "T1"));  // ctp12 段未被误删
    g.reset_account("ctp1", 0);
    EXPECT_FALSE(g.admit_trade("ctp12", "20260901", "T1"));  // reset 同样不得误伤
}

// admit_trade 自动清段路径的前缀隔离 (同一条 erase 循环的第三个调用点)。
TEST(TdIngestGate, TradeDedupAutoCleanDoesNotTouchPrefixAccount) {
    TdIngestGate g;
    EXPECT_TRUE(g.admit_trade("ctp1", "20260901", "T1"));
    EXPECT_TRUE(g.admit_trade("ctp12", "20260901", "T2"));
    EXPECT_TRUE(g.admit_trade("ctp1", "20260902", "T3"));    // ctp1 切日 → 自动清段
    EXPECT_FALSE(g.admit_trade("ctp12", "20260901", "T2"));  // ctp12 段未被误删
}

}  // namespace
