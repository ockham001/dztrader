// md_api_pure_test: 单元测试 make_batches / find_current_broker_in / decide_batch_check_action
// 这些自由函数实现在 md_api_pure.cpp 或 md_batch_check.h, 不依赖 MdApi 类和 CTP 头文件,
// 因此测试目标无需链接 dzmd_ctp_api, 也无需 include md_api.h (避免拉入 CTP 头)。
// 自由函数声明原本在 md_api.h 中, 此处手动 forward declare 以解耦 CTP 头依赖。
// 注: 旧 MdConfig 的 apply_config_op 逻辑已迁移至平台库 (apply_ctp_config_op),
// 由 libs/platform/tests/ctp_md_config_test.cpp 覆盖, 本文件不再重复测试。

#include <dztrader/platform/ctp_md_config.h>  // CtpBrokerEntry
#include "md/md_batch_check.h"  // BatchCheckAction, decide_batch_check_action

#include <deque>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dztrader::ctp {

// === 自由函数声明 (与 md_api.h 中声明一致, 仅用于测试编译) ===

/// 将合约列表分批为多个批次 (纯函数)。
std::deque<std::vector<std::string>> make_batches(std::vector<std::string> instruments,
                                                  int batch_size);

/// 在 brokers 中查找 name 对应的 broker (纯函数)。
const dztrader::platform::CtpBrokerEntry* find_current_broker_in(
    const std::vector<dztrader::platform::CtpBrokerEntry>& brokers,
    const std::string& name);

namespace {

// 构造带 name/broker_id 的 CtpBrokerEntry (designated initializers 省略其他字段触发
// -Wmissing-field-initializers, 测试目标已 -Wno-missing-field-initializers)
dztrader::platform::CtpBrokerEntry make_broker(const std::string& name,
                                               const std::string& broker_id = "",
                                               const std::string& user_id = "",
                                               const std::string& password = "") {
    dztrader::platform::CtpBrokerEntry b;
    b.name = name;
    b.broker_id = broker_id;
    b.user_id = user_id;
    b.password = password;
    return b;
}

// ============================================================================
// make_batches: 边界覆盖
// ============================================================================

TEST(MakeBatchesTest, EmptyInputReturnsEmpty) {
    auto batches = make_batches({}, 10);
    EXPECT_TRUE(batches.empty());
}

TEST(MakeBatchesTest, SingleBatchExactFit) {
    std::vector<std::string> insts = {"a", "b", "c"};
    auto batches = make_batches(insts, 3);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a", "b", "c"}));
}

TEST(MakeBatchesTest, MultipleBatchesEvenlyDivided) {
    std::vector<std::string> insts = {"a", "b", "c", "d", "e", "f"};
    auto batches = make_batches(insts, 2);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a", "b"}));
    EXPECT_EQ(batches[1], std::vector<std::string>({"c", "d"}));
    EXPECT_EQ(batches[2], std::vector<std::string>({"e", "f"}));
}

TEST(MakeBatchesTest, LastBatchSmaller) {
    std::vector<std::string> insts = {"a", "b", "c", "d", "e"};
    auto batches = make_batches(insts, 2);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a", "b"}));
    EXPECT_EQ(batches[1], std::vector<std::string>({"c", "d"}));
    EXPECT_EQ(batches[2], std::vector<std::string>({"e"}));
}

TEST(MakeBatchesTest, BatchSizeOne) {
    std::vector<std::string> insts = {"a", "b", "c"};
    auto batches = make_batches(insts, 1);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a"}));
    EXPECT_EQ(batches[1], std::vector<std::string>({"b"}));
    EXPECT_EQ(batches[2], std::vector<std::string>({"c"}));
}

TEST(MakeBatchesTest, BatchSizeLargerThanInput) {
    std::vector<std::string> insts = {"a", "b"};
    auto batches = make_batches(insts, 100);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a", "b"}));
}

TEST(MakeBatchesTest, BatchSizeZeroFallbackToOne) {
    // batch_size=0 会死循环, 必须兜底为 1
    std::vector<std::string> insts = {"a", "b", "c"};
    auto batches = make_batches(insts, 0);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a"}));
    EXPECT_EQ(batches[1], std::vector<std::string>({"b"}));
    EXPECT_EQ(batches[2], std::vector<std::string>({"c"}));
}

TEST(MakeBatchesTest, BatchSizeNegativeFallbackToOne) {
    std::vector<std::string> insts = {"a", "b"};
    auto batches = make_batches(insts, -5);
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"a"}));
    EXPECT_EQ(batches[1], std::vector<std::string>({"b"}));
}

TEST(MakeBatchesTest, ReturnsDeque) {
    // 验证返回类型是 deque (支持前向 pop_front, 后向 push_back)
    std::vector<std::string> insts = {"a", "b", "c", "d"};
    std::deque<std::vector<std::string>> batches = make_batches(insts, 2);
    ASSERT_EQ(batches.size(), 2u);
    // 模拟 enqueue_batches 的 push_back 行为 + send_next_batch 的 pop_front 行为
    batches.pop_front();
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0], std::vector<std::string>({"c", "d"}));
}

// ============================================================================
// find_current_broker_in: 边界覆盖
// ============================================================================

TEST(FindCurrentBrokerInTest, EmptyNameReturnsNull) {
    std::vector<dztrader::platform::CtpBrokerEntry> brokers = {make_broker("b1")};
    EXPECT_EQ(find_current_broker_in(brokers, ""), nullptr);
}

TEST(FindCurrentBrokerInTest, FoundByName) {
    std::vector<dztrader::platform::CtpBrokerEntry> brokers = {make_broker("b1"),
                                                               make_broker("b2", "9999")};
    const auto* found = find_current_broker_in(brokers, "b2");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "b2");
    EXPECT_EQ(found->broker_id, "9999");
}

TEST(FindCurrentBrokerInTest, NotFoundReturnsNull) {
    std::vector<dztrader::platform::CtpBrokerEntry> brokers = {make_broker("b1"), make_broker("b2")};
    EXPECT_EQ(find_current_broker_in(brokers, "nonexistent"), nullptr);
}

TEST(FindCurrentBrokerInTest, EmptyBrokersReturnsNull) {
    std::vector<dztrader::platform::CtpBrokerEntry> brokers;
    EXPECT_EQ(find_current_broker_in(brokers, "anything"), nullptr);
}

TEST(FindCurrentBrokerInTest, EmptyBrokersAndEmptyNameReturnsNull) {
    std::vector<dztrader::platform::CtpBrokerEntry> brokers;
    EXPECT_EQ(find_current_broker_in(brokers, ""), nullptr);
}

TEST(FindCurrentBrokerInTest, ReturnsPointerIntoVector) {
    // 验证返回的指针指向 vector 内元素 (可被调用方用作引用)
    std::vector<dztrader::platform::CtpBrokerEntry> brokers = {make_broker("b1", "id1"),
                                                               make_broker("b2", "id2")};
    const auto* ptr = find_current_broker_in(brokers, "b1");
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr, &brokers[0]);
}

TEST(FindCurrentBrokerInTest, FindsFirstMatch) {
    // 多个同名 broker 时返回第一个 (虽然实际配置中不允许重名, 但函数本身取第一个)
    std::vector<dztrader::platform::CtpBrokerEntry> brokers = {make_broker("dup", "first"),
                                                               make_broker("dup", "second")};
    const auto* found = find_current_broker_in(brokers, "dup");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->broker_id, "first");
}

// ============================================================================
// decide_batch_check_action: on_batch_complete 决策 (问题2修复: 滞留批次不算重试)
// ============================================================================

TEST(DecideBatchCheckActionTest, QueueNonEmptyContinuesSend) {
    // 补订检查窗口内新订阅已入队未发送: 继续发送链, 不消耗重试次数
    EXPECT_EQ(decide_batch_check_action(true, true, 0, 3), BatchCheckAction::ContinueSend);
    EXPECT_EQ(decide_batch_check_action(true, false, 0, 3), BatchCheckAction::ContinueSend);
    // 滞留批次优先于超限判定: 即使重试已达上限也不判 GiveUp
    EXPECT_EQ(decide_batch_check_action(true, true, 3, 3), BatchCheckAction::ContinueSend);
    EXPECT_EQ(decide_batch_check_action(true, true, 99, 3), BatchCheckAction::ContinueSend);
}

TEST(DecideBatchCheckActionTest, EmptyQueueNoPendingIsDone) {
    EXPECT_EQ(decide_batch_check_action(false, false, 0, 3), BatchCheckAction::Done);
    EXPECT_EQ(decide_batch_check_action(false, false, 2, 3), BatchCheckAction::Done);
}

TEST(DecideBatchCheckActionTest, EmptyQueuePendingRetriesUntilLimit) {
    // 与既有语义一致: retry_count+1 > max_retry 才 GiveUp (max=3 时最多 3 次 Retry)
    EXPECT_EQ(decide_batch_check_action(false, true, 0, 3), BatchCheckAction::Retry);
    EXPECT_EQ(decide_batch_check_action(false, true, 1, 3), BatchCheckAction::Retry);
    EXPECT_EQ(decide_batch_check_action(false, true, 2, 3), BatchCheckAction::Retry);
    EXPECT_EQ(decide_batch_check_action(false, true, 3, 3), BatchCheckAction::GiveUp);
    EXPECT_EQ(decide_batch_check_action(false, true, 4, 3), BatchCheckAction::GiveUp);
}

TEST(DecideBatchCheckActionTest, ZeroMaxRetryGivesUpImmediately) {
    // max_retry=0: 第一次检查到 Pending 即 GiveUp (retry_count+1=1 > 0)
    EXPECT_EQ(decide_batch_check_action(false, true, 0, 0), BatchCheckAction::GiveUp);
}

}  // namespace
}  // namespace dztrader::ctp