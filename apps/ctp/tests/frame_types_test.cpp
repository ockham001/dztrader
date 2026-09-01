#include <gtest/gtest.h>
#include <dztrader/data_type.h>
#include <dztrader/core/core_data_type.h>

TEST(FrameTypes, SystemFrames) {
    EXPECT_EQ(DZ_FRAME_SHUTDOWN, static_cast<DzFrameType>(12));
    EXPECT_EQ(DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER, static_cast<DzFrameType>(13));
}

TEST(FrameTypes, ShmPreloadFrames) {
    EXPECT_EQ(DZ_FRAME_PRELOAD_EVENT_SHM, static_cast<DzFrameType>(11));
    EXPECT_EQ(DZ_FRAME_PRELOAD_MD_SHM, static_cast<DzFrameType>(17));
}

TEST(FrameTypes, TdPlaceholderFrames) {
    EXPECT_EQ(DZ_FRAME_TD_REQ_MODIFY_CONFIG, static_cast<DzFrameType>(2102));
    EXPECT_EQ(DZ_FRAME_TD_CONNECT,           static_cast<DzFrameType>(2108));
    EXPECT_EQ(DZ_FRAME_TD_DISCONNECT,        static_cast<DzFrameType>(2109));
    EXPECT_EQ(DZ_FRAME_TD_SUBSCRIBE,         static_cast<DzFrameType>(2110));
}

TEST(FrameTypes, MdServiceLifecycleFrames) {
    EXPECT_EQ(DZ_FRAME_NOTIFY_MD_STARTED,  static_cast<DzFrameType>(1007));
    EXPECT_EQ(DZ_FRAME_NOTIFY_MD_STOPPED,  static_cast<DzFrameType>(1008));
}

TEST(FrameTypes, MdSubscriptionQueryFrames) {
    EXPECT_EQ(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, static_cast<DzFrameType>(1011));
    EXPECT_EQ(DZ_FRAME_RTN_MD_SUBSCRIPTIONS,   static_cast<DzFrameType>(1012));
}

TEST(FrameTypes, MdReaderRegisterFrames) {
    EXPECT_EQ(DZ_FRAME_REQUEST_MD_READER_REGISTER,   static_cast<DzFrameType>(1013));
    EXPECT_EQ(DZ_FRAME_REQUEST_MD_READER_UNREGISTER, static_cast<DzFrameType>(1014));
    EXPECT_EQ(DZ_FRAME_RTN_MD_READER_REGISTER,       static_cast<DzFrameType>(1015));
    EXPECT_EQ(DZ_FRAME_RTN_MD_READER_UNREGISTER,     static_cast<DzFrameType>(1016));
}

TEST(FrameTypes, StgFrames) {
    EXPECT_EQ(DZ_FRAME_UI_INPUT, static_cast<DzFrameType>(3001));
    EXPECT_EQ(DZ_FRAME_OUTPUT_UI, static_cast<DzFrameType>(3002));
}

TEST(FrameTypes, LogicalPositionFrame) {
    EXPECT_EQ(DZ_FRAME_SET_LOGICAL_POSITION, static_cast<DzFrameType>(103));
}

TEST(FrameTypes, LogConfigFramesHaveCorrectIds) {
    EXPECT_EQ(DZ_FRAME_SET_LOG_CONFIG, static_cast<DzFrameType>(14));
    EXPECT_EQ(DZ_FRAME_FLUSH_LOG, static_cast<DzFrameType>(15));
    EXPECT_EQ(DZ_FRAME_RTN_LOG_CONFIG, static_cast<DzFrameType>(16));
}

TEST(FrameTypes, BroadcastAllFrames) {
    EXPECT_EQ(DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER, static_cast<DzFrameType>(21));
}

TEST(FrameTypes, LogConfigFramesAreInSystemRange) {
    EXPECT_LE(DZ_FRAME_SET_LOG_CONFIG, static_cast<DzFrameType>(99));
    EXPECT_LE(DZ_FRAME_FLUSH_LOG, static_cast<DzFrameType>(99));
    EXPECT_LE(DZ_FRAME_RTN_LOG_CONFIG, static_cast<DzFrameType>(99));
}

TEST(FrameTypes, ShmConfigFramesHaveCorrectIds) {
    // 拆分后: EVENT (22/23, 沿用原 ID) + MD (24/25, 新增)
    EXPECT_EQ(DZ_FRAME_SET_EVENT_SHM_CONFIG, static_cast<DzFrameType>(22));
    EXPECT_EQ(DZ_FRAME_RTN_EVENT_SHM_CONFIG, static_cast<DzFrameType>(23));
    EXPECT_EQ(DZ_FRAME_SET_MD_SHM_CONFIG,    static_cast<DzFrameType>(24));
    EXPECT_EQ(DZ_FRAME_RTN_MD_SHM_CONFIG,    static_cast<DzFrameType>(25));
}

TEST(FrameTypes, MdConfigFramesRenamed) {
    EXPECT_EQ(DZ_FRAME_SET_MD_CONFIG, static_cast<DzFrameType>(1001));
    EXPECT_EQ(DZ_FRAME_RTN_MD_CONFIG, static_cast<DzFrameType>(1002));
    EXPECT_EQ(DZ_FRAME_RTN_MD_STATUS, static_cast<DzFrameType>(1003));
    EXPECT_EQ(DZ_FRAME_REQUEST_MD_CONNECT, static_cast<DzFrameType>(1004));
    EXPECT_EQ(DZ_FRAME_REQUEST_MD_DISCONNECT, static_cast<DzFrameType>(1005));
    EXPECT_EQ(DZ_FRAME_REQUEST_MD_SUBSCRIBE, static_cast<DzFrameType>(1006));
}

TEST(FrameTypes, NewProcessControlFrames) {
    EXPECT_EQ(DZ_FRAME_REQUEST_PROCESS_CONTROL, static_cast<DzFrameType>(115));
    EXPECT_EQ(DZ_FRAME_RTN_PROCESS_STATUS,      static_cast<DzFrameType>(116));
    EXPECT_EQ(DZ_FRAME_SET_PROCESS_CONFIG,      static_cast<DzFrameType>(117));
    EXPECT_EQ(DZ_FRAME_RTN_PROCESS_CONFIG,      static_cast<DzFrameType>(118));
    EXPECT_EQ(DZ_FRAME_QUERY_FULL_SNAPSHOT, static_cast<DzFrameType>(113));
}

TEST(FrameTypes, AutoLoginFrames) {
    EXPECT_EQ(DZ_FRAME_SET_AUTO_LOGIN, static_cast<DzFrameType>(119));
    EXPECT_EQ(DZ_FRAME_RTN_AUTO_LOGIN, static_cast<DzFrameType>(120));
}

TEST(FrameTypes, ProgressFrame) {
    EXPECT_EQ(DZ_FRAME_RTN_PROGRESS, static_cast<DzFrameType>(121));
}

// ============================================================================
// TD 帧类型测试
// DzFrameType 为 md/td 共享枚举, 集中在此验证一致性 (td 测试 glob 为 td_*_test.cpp,
// 本文件 frame_types_test.cpp 由 md 侧 CMakeLists.txt 显式包含编译)
// ============================================================================

TEST(FrameTypes, TickFrame) {
    // 行情数据帧 (1000)
    EXPECT_EQ(DZ_FRAME_TICK, static_cast<DzFrameType>(1000));
}

TEST(FrameTypes, TdPushFrames) {
    // 交易通用推送帧 (2000-2003, data_type.h)
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT,    static_cast<DzFrameType>(2000));
    EXPECT_EQ(DZ_FRAME_TRADE_REPORT,    static_cast<DzFrameType>(2001));
    EXPECT_EQ(DZ_FRAME_POSITION_INFO,   static_cast<DzFrameType>(2002));
    EXPECT_EQ(DZ_FRAME_TRADING_ACCOUNT, static_cast<DzFrameType>(2003));
}

TEST(FrameTypes, TdBusinessFrames) {
    // 交易业务帧 (2005-2017)
    EXPECT_EQ(DZ_FRAME_TD_INSTRUMENT,          static_cast<DzFrameType>(2005));
    EXPECT_EQ(DZ_FRAME_TD_INSTRUMENT_STATUS,   static_cast<DzFrameType>(2006));
    EXPECT_EQ(DZ_FRAME_TD_ERROR_REPORT,        static_cast<DzFrameType>(2007));
    EXPECT_EQ(DZ_FRAME_TD_RISK_REJECT,         static_cast<DzFrameType>(2008));
    EXPECT_EQ(DZ_FRAME_TD_TRANSFER_REQ,        static_cast<DzFrameType>(2009));
    EXPECT_EQ(DZ_FRAME_TD_TRANSFER_RSP,       static_cast<DzFrameType>(2010));
    EXPECT_EQ(DZ_FRAME_TD_TRANSFER_RTN,        static_cast<DzFrameType>(2011));
    EXPECT_EQ(DZ_FRAME_TD_PASSWORD_UPDATE_REQ, static_cast<DzFrameType>(2012));
    EXPECT_EQ(DZ_FRAME_TD_PASSWORD_UPDATE_RSP, static_cast<DzFrameType>(2013));
    EXPECT_EQ(DZ_FRAME_TD_SETTLEMENT_INFO,     static_cast<DzFrameType>(2014));
    EXPECT_EQ(DZ_FRAME_TD_MARGIN_RATE,         static_cast<DzFrameType>(2015));
    EXPECT_EQ(DZ_FRAME_TD_COMMISSION_RATE,     static_cast<DzFrameType>(2016));
    EXPECT_EQ(DZ_FRAME_TD_POSITION_DETAIL,     static_cast<DzFrameType>(2017));
}

TEST(FrameTypes, TdConfigAndStatusFrames) {
    // 交易配置/状态帧 (2103-2114)
    EXPECT_EQ(DZ_FRAME_TD_RTN_CONFIG,             static_cast<DzFrameType>(2103));
    EXPECT_EQ(DZ_FRAME_TD_RTN_STATUS,             static_cast<DzFrameType>(2104));
    EXPECT_EQ(DZ_FRAME_TD_QUERY_ALL,              static_cast<DzFrameType>(2105));
    EXPECT_EQ(DZ_FRAME_TD_NOTIFY_UI,              static_cast<DzFrameType>(2106));
    EXPECT_EQ(DZ_FRAME_NOTIFY_TD_STARTED,         static_cast<DzFrameType>(2111));
    EXPECT_EQ(DZ_FRAME_NOTIFY_TD_STOPPED,         static_cast<DzFrameType>(2112));
    EXPECT_EQ(DZ_FRAME_NOTIFY_TD_CONNECTED,       static_cast<DzFrameType>(2113));
    EXPECT_EQ(DZ_FRAME_NOTIFY_TD_DISCONNECTED,    static_cast<DzFrameType>(2114));
}
