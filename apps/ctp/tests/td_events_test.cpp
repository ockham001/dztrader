#include <gtest/gtest.h>

#include <cstddef>

#include "td/td_events.h"

using namespace dztrader::ctp;

// ============================================================================
// TD 事件类型 + Field 结构测试
// ============================================================================

TEST(TdEventsTest, EventTypeValues) {
    EXPECT_EQ(static_cast<int>(EventType::OnRspAuthenticate), 100);
    EXPECT_EQ(static_cast<int>(EventType::OnRtnOrder), 110);
    EXPECT_EQ(static_cast<int>(EventType::OnRtnTrade), 111);
    EXPECT_EQ(static_cast<int>(EventType::OnRspTradingAccountPasswordUpdate), 120);
}

// --- Layout 测试: sizeof + offsetof 断言 (Plan 1 审查 Minor 修复) ---

TEST(TdEventsTest, OnRtnOrderFieldLayout) {
    static_assert(sizeof(OnRtnOrderField) >= sizeof(CThostFtdcOrderField),
                  "OnRtnOrderField must hold CThostFtdcOrderField");
    static_assert(offsetof(OnRtnOrderField, order) == 0,
                  "order should be first field");
    OnRtnOrderField f{};
    f.order.OrderRef[0] = '1';
    f.rsp_time = std::chrono::system_clock::now();
    EXPECT_EQ(f.order.OrderRef[0], '1');
}

TEST(TdEventsTest, OnRspAuthenticateFieldLayout) {
    static_assert(offsetof(OnRspAuthenticateField, request_id) > 0,
                  "request_id should not be first (after optionals)");
    static_assert(offsetof(OnRspAuthenticateField, is_last) > 0,
                  "is_last should not be first");
    OnRspAuthenticateField f{};
    f.request_id = 42;
    f.is_last = true;
    EXPECT_EQ(f.request_id, 42);
    EXPECT_TRUE(f.is_last);
}

// --- td_delete_event_data 测试 ---

TEST(TdEventsTest, TdDeleteEventDataOnRtnOrder) {
    Event e{};
    e.type = EventType::OnRtnOrder;
    e.data = new OnRtnOrderField{};
    td_delete_event_data(e);  // 不应崩溃, 应释放 data
    EXPECT_EQ(e.data, nullptr);
}

TEST(TdEventsTest, TdDeleteEventDataOnUnknown) {
    Event e{};
    e.type = EventType::Unknown;
    e.data = nullptr;
    td_delete_event_data(e);  // 不应崩溃, data 保持 nullptr
    EXPECT_EQ(e.data, nullptr);
}

// 错误路径: md 类型误传 + 非 nullptr data (Plan 1 审查 Minor 修复)
TEST(TdEventsTest, TdDeleteEventDataOnMdTypeWithNonNullData) {
    Event e{};
    e.type = EventType::OnRspUserLogin;  // md 类型 (值=4), td_delete_event_data 不处理
    // 分配一个占位指针 (不实际使用, 仅验证不崩溃且 data 被置 nullptr)
    // 注意: td_delete_event_data 对 md 类型走 default 分支, 不 delete, 但置 nullptr
    // 为避免内存泄漏, 用 new int 然后在调用前保存指针
    int* leaked = new int(42);
    e.data = leaked;
    td_delete_event_data(e);
    // td_delete_event_data 不释放 md 类型数据, 但会置 e.data=nullptr
    // 调用方应负责释放 md 类型数据 (通过 Event::delete_data())
    // 此处手动释放避免泄漏
    delete leaked;
    EXPECT_EQ(e.data, nullptr);
}

// 所有 TD 类型均能正确释放 (批量验证)
TEST(TdEventsTest, TdDeleteEventDataAllTdTypes) {
    auto test_type = [](EventType type, void* data) {
        Event e{};
        e.type = type;
        e.data = data;
        td_delete_event_data(e);
        EXPECT_EQ(e.data, nullptr) << "type=" << static_cast<int>(type);
    };
    test_type(EventType::OnRspAuthenticate, new OnRspAuthenticateField{});
    test_type(EventType::OnRspTdUserLogin, new OnRspTdUserLoginField{});
    test_type(EventType::OnRspSettlementInfoConfirm, new OnRspSettlementInfoConfirmField{});
    test_type(EventType::OnRspQryInstrument, new OnRspQryInstrumentField{});
    test_type(EventType::OnRspQryTradingAccount, new OnRspQryTradingAccountField{});
    test_type(EventType::OnRspQryInvestorPosition, new OnRspQryInvestorPositionField{});
    test_type(EventType::OnRspQryInvestorPositionDetail, new OnRspQryInvestorPositionDetailField{});
    test_type(EventType::OnRspQryInstrumentMarginRate, new OnRspQryInstrumentMarginRateField{});
    test_type(EventType::OnRspQryInstrumentCommissionRate, new OnRspQryInstrumentCommissionRateField{});
    test_type(EventType::OnRspQryOrder, new OnRspQryOrderField{});
    test_type(EventType::OnRtnOrder, new OnRtnOrderField{});
    test_type(EventType::OnRtnTrade, new OnRtnTradeField{});
    test_type(EventType::OnRtnInstrumentStatus, new OnRtnInstrumentStatusField{});
    test_type(EventType::OnRspOrderInsert, new OnRspOrderInsertField{});
    test_type(EventType::OnRspOrderAction, new OnRspOrderActionField{});
    test_type(EventType::OnErrRtnOrderInsert, new OnErrRtnOrderInsertField{});
    test_type(EventType::OnErrRtnOrderAction, new OnErrRtnOrderActionField{});
    test_type(EventType::OnRspFromBankToFutureByFuture, new OnRspFromBankToFutureByFutureField{});
    test_type(EventType::OnRtnFromBankToFutureByFuture, new OnRtnFromBankToFutureByFutureField{});
    test_type(EventType::OnRspUserPasswordUpdate, new OnRspUserPasswordUpdateField{});
    test_type(EventType::OnRspTradingAccountPasswordUpdate, new OnRspTradingAccountPasswordUpdateField{});
}
