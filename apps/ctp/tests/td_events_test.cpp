#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#include "td/td_events.h"

using namespace dztrader::ctp;

namespace {

// 释放计数: 替换全局 operator new/delete, 精确统计 td_delete_event_data
// 是否对每种类型恰好 delete 一次. 若 switch 漏 case 走 default, 对象不会被释放,
// 仅凭 e.data == nullptr 的旧断言无法发现 (实现无条件置空).
std::atomic<bool> g_track_allocations{false};
std::atomic<int> g_live_allocations{0};
std::atomic<int> g_delete_calls{0};

/// 在统计窗口内分配并释放一个事件 data, 断言恰好释放一次.
template <typename Field>
void expect_freed_exactly_once(EventType type) {
    g_live_allocations.store(0, std::memory_order_relaxed);
    g_delete_calls.store(0, std::memory_order_relaxed);

    Event e{};
    e.type = type;
    g_track_allocations.store(true, std::memory_order_relaxed);
    e.data = new Field{};
    td_delete_event_data(e);
    g_track_allocations.store(false, std::memory_order_relaxed);

    EXPECT_EQ(e.data, nullptr) << "type=" << static_cast<int>(type);
    EXPECT_EQ(g_live_allocations.load(std::memory_order_relaxed), 0)
        << "type=" << static_cast<int>(type) << " data not freed";
    EXPECT_EQ(g_delete_calls.load(std::memory_order_relaxed), 1)
        << "type=" << static_cast<int>(type) << " delete count";
}

/// 统计窗口内的分配/释放原语 (malloc/free 成对, 避免 -Wmismatched-new-delete).
void* tracked_alloc(std::size_t size) {
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_live_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return p;
}

void tracked_free(void* p) noexcept {
    if (p != nullptr && g_track_allocations.load(std::memory_order_relaxed)) {
        g_live_allocations.fetch_sub(1, std::memory_order_relaxed);
        g_delete_calls.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

}  // namespace

void* operator new(std::size_t size) { return tracked_alloc(size); }

void* operator new[](std::size_t size) { return tracked_alloc(size); }

void operator delete(void* p) noexcept { tracked_free(p); }

void operator delete[](void* p) noexcept { tracked_free(p); }

void operator delete(void* p, std::size_t) noexcept { tracked_free(p); }

void operator delete[](void* p, std::size_t) noexcept { tracked_free(p); }

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
    static_assert(offsetof(OnRtnOrderField, order) == 0, "order should be first field");
    OnRtnOrderField f{};
    f.order.OrderRef[0] = '1';
    f.rsp_time = std::chrono::system_clock::now();
    EXPECT_EQ(f.order.OrderRef[0], '1');
}

TEST(TdEventsTest, OnRspAuthenticateFieldLayout) {
    static_assert(offsetof(OnRspAuthenticateField, request_id) > 0,
                  "request_id should not be first (after optionals)");
    static_assert(offsetof(OnRspAuthenticateField, is_last) > 0, "is_last should not be first");
    OnRspAuthenticateField f{};
    f.request_id = 42;
    f.is_last = true;
    EXPECT_EQ(f.request_id, 42);
    EXPECT_TRUE(f.is_last);
}

// --- td_delete_event_data 测试 ---

TEST(TdEventsTest, TdDeleteEventDataOnRtnOrder) {
    // 恰好释放一次 (不是"不崩溃"或"指针被置空")
    expect_freed_exactly_once<OnRtnOrderField>(EventType::OnRtnOrder);
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

// 所有 TD 类型均恰好释放一次 (批量验证; 漏 case 会泄漏并在此失败)
TEST(TdEventsTest, TdDeleteEventDataAllTdTypes) {
    expect_freed_exactly_once<OnRspAuthenticateField>(EventType::OnRspAuthenticate);
    expect_freed_exactly_once<OnRspTdUserLoginField>(EventType::OnRspTdUserLogin);
    expect_freed_exactly_once<OnRspSettlementInfoConfirmField>(
        EventType::OnRspSettlementInfoConfirm);
    expect_freed_exactly_once<OnRspQryInstrumentField>(EventType::OnRspQryInstrument);
    expect_freed_exactly_once<OnRspQryTradingAccountField>(EventType::OnRspQryTradingAccount);
    expect_freed_exactly_once<OnRspQryInvestorPositionField>(EventType::OnRspQryInvestorPosition);
    expect_freed_exactly_once<OnRspQryOrderField>(EventType::OnRspQryOrder);
    expect_freed_exactly_once<OnRtnOrderField>(EventType::OnRtnOrder);
    expect_freed_exactly_once<OnRtnTradeField>(EventType::OnRtnTrade);
    expect_freed_exactly_once<OnRspOrderInsertField>(EventType::OnRspOrderInsert);
    expect_freed_exactly_once<OnRspOrderActionField>(EventType::OnRspOrderAction);
    expect_freed_exactly_once<OnErrRtnOrderInsertField>(EventType::OnErrRtnOrderInsert);
    expect_freed_exactly_once<OnErrRtnOrderActionField>(EventType::OnErrRtnOrderAction);
    expect_freed_exactly_once<OnRspFromBankToFutureByFutureField>(
        EventType::OnRspFromBankToFutureByFuture);
    expect_freed_exactly_once<OnRtnFromBankToFutureByFutureField>(
        EventType::OnRtnFromBankToFutureByFuture);
    expect_freed_exactly_once<OnRspUserPasswordUpdateField>(EventType::OnRspUserPasswordUpdate);
    expect_freed_exactly_once<OnRspTradingAccountPasswordUpdateField>(
        EventType::OnRspTradingAccountPasswordUpdate);
}

// td 版本 FrontConnected/Disconnected (含 account_id) 不能走 md 的 Event::delete_data,
// 必须由 td_delete_event_data 释放; 这是该函数存在的首要特例.
TEST(TdEventsTest, TdDeleteEventDataFrontConnectedAndDisconnected) {
    expect_freed_exactly_once<OnTdFrontConnectedField>(EventType::OnFrontConnected);
    expect_freed_exactly_once<OnTdFrontDisconnectedField>(EventType::OnFrontDisconnected);
}
