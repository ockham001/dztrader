// 帧类型测试：本文件只写帧名, 不写帧值。
//
// 帧值的唯一书写位置是 libs/strategy_api/include/dztrader/data_type.h（策略可见帧）
// 与 libs/core/include/dztrader/core/core_data_type.h（平台帧）;
// 因此改帧号不需要动这里, 这里也不允许出现字面帧值。

#include <gtest/gtest.h>

#include <dztrader/core/core_data_type.h>

#include <cstddef>
#include <set>

// 编译期钉子: 合约推送帧已退役 (ADR 0011), 任何头文件重新定义它都会在此引爆。
#if defined(DZ_FRAME_TD_INSTRUMENT)
#error "DZ_FRAME_TD_INSTRUMENT 已退役"
#endif

// 编译期钉子: 合约状态与费率帧已退役 (ADR 0013), 任何头文件重新定义它都会在此引爆。
#if defined(DZ_FRAME_TD_INSTRUMENT_STATUS) || defined(DZ_FRAME_TD_MARGIN_RATE) || \
    defined(DZ_FRAME_TD_COMMISSION_RATE)
#error "TD_INSTRUMENT_STATUS/TD_MARGIN_RATE/TD_COMMISSION_RATE 已退役 (ADR 0013)"
#endif
#if defined(DZ_FRAME_TD_QUERY_FEE_RATE)
#error "DZ_FRAME_TD_QUERY_FEE_RATE 已退役 (ADR 0013)"
#endif

// 编译期钉子: 合约定向刷新帧已退役 (ADR 0014), 任何头文件重新定义它都会在此引爆。
#if defined(DZ_FRAME_TD_QUERY_INSTRUMENT)
#error "DZ_FRAME_TD_QUERY_INSTRUMENT 已退役 (ADR 0014)"
#endif

namespace {

/// 策略可见帧清单（策略经 dz_next_event / dz_next_md 识别消费）。
/// 增删策略可见帧属于对外契约变更, 必须显式改这里。
constexpr DzFrameType kStrategyVisibleFrames[] = {
    DZ_FRAME_SHUTDOWN,           DZ_FRAME_TICK,
    DZ_FRAME_ORDER_REPORT,       DZ_FRAME_TRADE_REPORT,
    DZ_FRAME_POSITION_INFO,      DZ_FRAME_TRADING_ACCOUNT,
    DZ_FRAME_ACCOUNT_STATUS,
    DZ_FRAME_UI_INPUT,           DZ_FRAME_SCHEDULE,
};

constexpr std::size_t kStrategyVisibleCount =
    sizeof(kStrategyVisibleFrames) / sizeof(kStrategyVisibleFrames[0]);

}  // namespace

// ── 策略可见面: 集合稳定（增删策略可见帧必须显式改测试, 属对外契约变更） ──

TEST(FrameTypes, StrategyVisibleSetIsStable) {
    EXPECT_EQ(kStrategyVisibleCount, 9u);
    const std::set<DzFrameType> unique(kStrategyVisibleFrames,
                                       kStrategyVisibleFrames + kStrategyVisibleCount);
    EXPECT_EQ(unique.size(), kStrategyVisibleCount) << "策略可见帧存在重复值";
}

// ── 策略热路径: 交易推送帧连号（接收方 switch 才能生成单张跳转表） ──

TEST(FrameTypes, TradePushClusterIsContiguous) {
    EXPECT_EQ(DZ_FRAME_TRADE_REPORT, DZ_FRAME_ORDER_REPORT + 1);
    EXPECT_EQ(DZ_FRAME_POSITION_INFO, DZ_FRAME_ORDER_REPORT + 2);
    EXPECT_EQ(DZ_FRAME_TRADING_ACCOUNT, DZ_FRAME_ORDER_REPORT + 3);
    // 行情/交易生命周期帧与交易推送同段（同一段判定即进跳转表）
    EXPECT_LT(DZ_FRAME_NOTIFY_MD_STARTED, DZ_FRAME_ORDER_REPORT);
    // 策略簇连号
    EXPECT_EQ(DZ_FRAME_SCHEDULE, DZ_FRAME_UI_INPUT + 3);
}
