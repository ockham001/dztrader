#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/platform/subscription_manager.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <filesystem>
#include <optional>
#include <random>
#include <string>

using dztrader::platform::InstrumentSubInfo;
using dztrader::platform::SubState;
using dztrader::platform::SubscriptionManager;
using dztrader::platform::SubscriptionDetail;
using dztrader::platform::NotifyUi;
using dztrader::platform::write_ext_inst_json_obj;
using dztrader::platform::write_ext_inst_raw;
using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class SubscriptionManagerTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;
    std::optional<NotifyUi> notify_ui_;
    std::optional<SubscriptionManager> sm_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        // 每进程唯一通道名（PID + 随机数）：ctest -j 并行时同二进制各用例作为独立进程，
        // 固定通道名/目录会互相删建冲突。
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist;
        const std::string uid = std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
                                "_" + std::to_string(dist(gen));
        channel_name_ = "dz_test_submgr_" + uid;
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);

        ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        writer_ = MultiWriter::create(meta_, "test_writer");
        reader_ = Reader::create(meta_, "test_reader");
        notify_ui_.emplace("test_md", *writer_);
        sm_.emplace("test_md", *writer_, *notify_ui_);
    }

    void TearDown() override {
        sm_.reset();
        notify_ui_.reset();
        reader_.reset();
        writer_.reset();
        meta_.reset();
        std::filesystem::remove_all(shm_dir_);
    }

    /// 辅助: 写 JSON 帧到 writer, 从 reader 读回构造 FrameView
    /// 返回 reader 读到的帧指针 (next_frame)
    const std::byte* write_and_read_frame(DzFrameType type, std::string_view inst_id,
                                           const nlohmann::json& payload) {
        write_ext_inst_json_obj(*writer_, type, inst_id, payload);
        return reader_->next_frame();
    }
};

// Test 1: subscribe new instruments returns to_subscribe
TEST_F(SubscriptionManagerTest, SubscribeNewReturnsToSubscribe) {
    auto result = sm_->subscribe("stg.alpha", {"IF2506", "IC2506"});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "IF2506");
    EXPECT_EQ(result[1], "IC2506");
}

// Test 2: subscribe already Subscribed instrument not returned (only inserts subscriber)
TEST_F(SubscriptionManagerTest, SubscribeSubscribedNotReturned) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    auto result = sm_->subscribe("stg.beta", {"IF2506"});
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(sm_->find_instrument("IF2506")->subscribers.size(), 2u);
}

// Test 4: unsubscribe returns instruments to unsub
TEST_F(SubscriptionManagerTest, UnsubscribeReturnsToUnsub) {
    sm_->subscribe("stg.alpha", {"IF2506", "IC2506"});
    sm_->mark_pending({"IF2506", "IC2506"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->on_sub_confirmed("IC2506", true);
    auto result = sm_->unsubscribe("stg.alpha", {"IF2506"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "IF2506");
}

// Test 5: unsubscribe_all batch unsub
TEST_F(SubscriptionManagerTest, UnsubscribeAllBatch) {
    sm_->subscribe("stg.alpha", {"IF2506", "IC2506", "rb2510"});
    sm_->mark_pending({"IF2506", "IC2506", "rb2510"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->on_sub_confirmed("IC2506", true);
    sm_->on_sub_confirmed("rb2510", true);
    auto result = sm_->unsubscribe_all("stg.alpha");
    ASSERT_EQ(result.size(), 3u);
}

// Test 6: optimistic unsubscribe sets NotRequested immediately
TEST_F(SubscriptionManagerTest, OptimisticUnsubscribeSetsNotRequested) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    EXPECT_EQ(sm_->find_instrument("IF2506")->sub_state, SubState::NotRequested);
}

// Test 7: on_sub_confirmed(failed) keeps Pending
TEST_F(SubscriptionManagerTest, OnSubConfirmedFailedKeepsPending) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", false);
    EXPECT_EQ(sm_->find_instrument("IF2506")->sub_state, SubState::Pending);
}

// Test 8: on_unsub_confirmed erases entry with no subscribers
TEST_F(SubscriptionManagerTest, OnUnsubConfirmedErasesNoSubscriber) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    sm_->on_unsub_confirmed("IF2506");
    EXPECT_EQ(sm_->find_instrument("IF2506"), nullptr);
}

// Test 9: handle_query_md_subscriptions unsuccessful mode (frame path)
// Verifies: decode -> query -> RTN frame written to SHM, Pending priority ordering
TEST_F(SubscriptionManagerTest, QueryUnsuccessfulMode) {
    sm_->subscribe("stg.alpha", {"IF2506", "IC2506"});
    sm_->mark_pending({"IF2506"});
    // IF2506 is Pending, IC2506 is NotRequested -> both "unsuccessful"

    // Write QUERY frame, read back, pass to manager
    nlohmann::json req = {{"query", "unsuccessful"}};
    auto* query_frame = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
    ASSERT_NE(query_frame, nullptr);
    FrameView view(query_frame);
    sm_->handle_query_md_subscriptions(view);

    // Read RTN frame and verify
    auto* rtn_frame = reader_->next_frame();
    ASSERT_NE(rtn_frame, nullptr);
    FrameView rtn_view(rtn_frame);
    EXPECT_EQ(rtn_view.type(), DZ_FRAME_RTN_MD_SUBSCRIPTIONS);
    auto rsp = nlohmann::json::parse(
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()),
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()) + rtn_view.ext_inst_payload_size());
    EXPECT_EQ(rsp["returned_count"], 2);
    EXPECT_EQ(rsp["total_matched"], 2);
    EXPECT_FALSE(rsp["truncated"]);
    EXPECT_TRUE(rsp["error"].is_null());
    // 契约: unsuccessful 模式 Pending 优先, NotRequested 次之
    EXPECT_EQ(rsp["subscriptions"][0]["instrument"], "IF2506");
    EXPECT_EQ(rsp["subscriptions"][0]["sub_state"], "Pending");
    EXPECT_EQ(rsp["subscriptions"][1]["instrument"], "IC2506");
    EXPECT_EQ(rsp["subscriptions"][1]["sub_state"], "NotRequested");
}

// Test 10: query truncation (exceeds SUBSCRIPTION_QUERY_MAX)
TEST_F(SubscriptionManagerTest, QueryTruncation) {
    // Create 40 instruments (exceeds SUBSCRIPTION_QUERY_MAX=32)
    std::vector<std::string> instruments;
    for (int i = 0; i < 40; ++i) {
        instruments.push_back("IF" + std::to_string(2500 + i));
    }
    sm_->subscribe("stg.alpha", instruments);
    // All NotRequested -> all "unsuccessful"

    nlohmann::json req = {{"query", "unsuccessful"}};
    auto* query_frame = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
    ASSERT_NE(query_frame, nullptr);
    FrameView view(query_frame);
    sm_->handle_query_md_subscriptions(view);

    auto* rtn_frame = reader_->next_frame();
    ASSERT_NE(rtn_frame, nullptr);
    FrameView rtn_view(rtn_frame);
    auto rsp = nlohmann::json::parse(
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()),
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()) + rtn_view.ext_inst_payload_size());
    EXPECT_EQ(rsp["returned_count"], 32);
    EXPECT_EQ(rsp["total_matched"], 40);
    EXPECT_TRUE(rsp["truncated"]);
}

// Test 11: query error responses (unknown_query / bad_json / missing_query_or_instruments / ambiguous_query)
TEST_F(SubscriptionManagerTest, QueryErrorResponses) {
    // unknown_query
    {
        nlohmann::json req = {{"query", "invalid_mode"}};
        auto* f = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
        ASSERT_NE(f, nullptr);
        sm_->handle_query_md_subscriptions(FrameView(f));
        auto* rtn = reader_->next_frame();
        ASSERT_NE(rtn, nullptr);
        FrameView rv(rtn);
        auto rsp = nlohmann::json::parse(
            reinterpret_cast<const char*>(rv.ext_inst_payload()),
            reinterpret_cast<const char*>(rv.ext_inst_payload()) + rv.ext_inst_payload_size());
        EXPECT_EQ(rsp["error"], "unknown_query");
        EXPECT_EQ(rsp["returned_count"], 0);
    }
    // missing_query_or_instruments
    {
        nlohmann::json req = nlohmann::json::object();
        auto* f = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
        ASSERT_NE(f, nullptr);
        sm_->handle_query_md_subscriptions(FrameView(f));
        auto* rtn = reader_->next_frame();
        ASSERT_NE(rtn, nullptr);
        FrameView rv(rtn);
        auto rsp = nlohmann::json::parse(
            reinterpret_cast<const char*>(rv.ext_inst_payload()),
            reinterpret_cast<const char*>(rv.ext_inst_payload()) + rv.ext_inst_payload_size());
        EXPECT_EQ(rsp["error"], "missing_query_or_instruments");
    }
    // ambiguous_query (both query and instruments)
    {
        nlohmann::json req = {{"query", "unsuccessful"}, {"instruments", {"IF2506"}}};
        auto* f = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
        ASSERT_NE(f, nullptr);
        sm_->handle_query_md_subscriptions(FrameView(f));
        auto* rtn = reader_->next_frame();
        ASSERT_NE(rtn, nullptr);
        FrameView rv(rtn);
        auto rsp = nlohmann::json::parse(
            reinterpret_cast<const char*>(rv.ext_inst_payload()),
            reinterpret_cast<const char*>(rv.ext_inst_payload()) + rv.ext_inst_payload_size());
        EXPECT_EQ(rsp["error"], "ambiguous_query");
    }
    // bad_json (invalid JSON payload)
    {
        // Write raw non-JSON bytes as frame payload
        const char* bad = "not valid json";
        dztrader::platform::write_ext_inst_raw(*writer_, DZ_FRAME_QUERY_MD_SUBSCRIPTIONS,
                                               "test_md",
                                               reinterpret_cast<const std::byte*>(bad), 14);
        auto* f = reader_->next_frame();
        ASSERT_NE(f, nullptr);
        sm_->handle_query_md_subscriptions(FrameView(f));
        auto* rtn = reader_->next_frame();
        ASSERT_NE(rtn, nullptr);
        FrameView rv(rtn);
        auto rsp = nlohmann::json::parse(
            reinterpret_cast<const char*>(rv.ext_inst_payload()),
            reinterpret_cast<const char*>(rv.ext_inst_payload()) + rv.ext_inst_payload_size());
        EXPECT_EQ(rsp["error"], "bad_json");
    }
}

// Test 12: handle_subscribe_req decode failure -> notify_ui + return empty result
TEST_F(SubscriptionManagerTest, HandleSubscribeReqDecodeFailure) {
    // Write invalid JSON as QUERY frame payload (wrong type for SubscribeReq)
    nlohmann::json bad_req = {{"not_a_field", 123}};
    auto* f = write_and_read_frame(DZ_FRAME_REQUEST_MD_SUBSCRIBE, "test_md", bad_req);
    ASSERT_NE(f, nullptr);
    // Should not throw, should return empty result
    auto result = sm_->handle_subscribe_req(FrameView(f));
    EXPECT_TRUE(result.instance_id.empty());
    EXPECT_TRUE(result.to_subscribe.empty());
    EXPECT_TRUE(result.to_unsubscribe.empty());
}

// Test 3: handle_subscribe_req action=Subscribe replace=true
TEST_F(SubscriptionManagerTest, HandleSubscribeReqReplaceTrue) {
    // First subscribe
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);

    // Now replace with new instruments
    nlohmann::json req = {
        {"instance_id", "stg.alpha"},
        {"action", 0},  // Subscribe
        {"replace", true},
        {"instruments", {"IC2506", "rb2510"}}
    };
    auto* f = write_and_read_frame(DZ_FRAME_REQUEST_MD_SUBSCRIBE, "test_md", req);
    ASSERT_NE(f, nullptr);
    auto result = sm_->handle_subscribe_req(FrameView(f));
    EXPECT_EQ(result.instance_id, "stg.alpha");
    // to_unsubscribe should contain IF2506 (was Subscribed, now no subscriber)
    ASSERT_EQ(result.to_unsubscribe.size(), 1u);
    EXPECT_EQ(result.to_unsubscribe[0], "IF2506");
    // to_subscribe should contain IC2506, rb2510
    ASSERT_EQ(result.to_subscribe.size(), 2u);
}

// Test 13: on_idle resets sub_state + cleans empty subscribers
TEST_F(SubscriptionManagerTest, OnIdleResetsAndCleans) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->on_idle();
    // Entry should remain (has subscriber), but sub_state = NotRequested
    auto* info = sm_->find_instrument("IF2506");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->sub_state, SubState::NotRequested);
    EXPECT_EQ(sm_->subscribed_count(), 0u);
}

// Test 14: on_session_lost resets sub_state (keeps entries)
TEST_F(SubscriptionManagerTest, OnSessionLostKeepsEntries) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->on_session_lost();
    auto* info = sm_->find_instrument("IF2506");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->sub_state, SubState::NotRequested);
    EXPECT_EQ(sm_->subscribed_count(), 0u);
}

// Test 15: is_instrument_subscribed returns correct state
TEST_F(SubscriptionManagerTest, IsInstrumentSubscribed) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    EXPECT_FALSE(sm_->is_instrument_subscribed("IF2506"));
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    EXPECT_TRUE(sm_->is_instrument_subscribed("IF2506"));
    EXPECT_FALSE(sm_->is_instrument_subscribed("IC2506"));
}

// Test 16: check_subscribe_status updates cached_subscribed_count_
TEST_F(SubscriptionManagerTest, CheckSubscribeStatusUpdatesCount) {
    sm_->subscribe("stg.alpha", {"IF2506", "IC2506", "rb2510"});
    sm_->mark_pending({"IF2506", "IC2506", "rb2510"});
    sm_->on_sub_confirmed("IF2506", true);
    sm_->on_sub_confirmed("IC2506", true);
    auto pending = sm_->check_subscribe_status();
    EXPECT_EQ(pending.size(), 1u);
    EXPECT_EQ(sm_->subscribed_count(), 2u);
    EXPECT_EQ(sm_->expected_subscribe_count(), 3u);
}

// Test 17: on_logged_in returns plan
TEST_F(SubscriptionManagerTest, OnLoggedInReturnsPlan) {
    sm_->subscribe("stg.alpha", {"IF2506", "IC2506"});
    sm_->mark_pending({"IF2506", "IC2506"});
    sm_->on_sub_confirmed("IF2506", true);
    // IF2506 is Subscribed with subscriber; IC2506 is Pending with subscriber
    auto plan = sm_->on_logged_in();
    // After on_logged_in: cleanup collects Subscribed orphans, but IF2506 has subscriber (not orphan)
    // So to_unsubscribe should be empty
    EXPECT_TRUE(plan.to_unsubscribe.empty());
    // to_resubscribe: both have subscribers and are now NotRequested (after reset)
    ASSERT_EQ(plan.to_resubscribe.size(), 2u);
    EXPECT_EQ(sm_->subscribed_count(), 0u);
}

// Test 18: replace with other subscriber intersection
TEST_F(SubscriptionManagerTest, ReplacePreservesOtherSubscriber) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->subscribe("stg.beta", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    // stg.alpha replaces with [IC2506]
    auto to_unsub = sm_->unsubscribe_all("stg.alpha");
    // IF2506 should NOT be in to_unsub because stg.beta still holds it
    EXPECT_TRUE(to_unsub.empty());
    auto to_sub = sm_->subscribe("stg.alpha", {"IC2506"});
    ASSERT_EQ(to_sub.size(), 1u);
    EXPECT_EQ(to_sub[0], "IC2506");
}

// Test 19: Pending instrument subscribed by second instance -> appears in to_subscribe again
TEST_F(SubscriptionManagerTest, PendingSubscribedAgainBySecond) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    auto result = sm_->subscribe("stg.beta", {"IF2506"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "IF2506");
}

// Test 20: optimistic unsubscribe window - new subscriber triggers resubscribe
TEST_F(SubscriptionManagerTest, OptimisticUnsubWindowResubscribe) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);
    // stg.alpha unsubscribes (optimistic -> NotRequested)
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    // stg.beta subscribes same instrument -> should appear in to_subscribe
    auto result = sm_->subscribe("stg.beta", {"IF2506"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "IF2506");
}

// Test 21: query instruments mode - skips non-string, non-existent returns NotRequested
TEST_F(SubscriptionManagerTest, QueryInstrumentsModeSkipsNonString) {
    // Subscribe IF2506 so it exists; IC2506 and NONEXISTENT won't exist
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->on_sub_confirmed("IF2506", true);

    // instruments 模式: 请求为对象, 内含 instruments 数组 (混合类型: string, number, bool, string, string)
    nlohmann::json req = {{"instruments", {"IF2506", 123, "IC2506", true, "NONEXISTENT"}}};

    auto* query_frame = write_and_read_frame(DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, "test_md", req);
    ASSERT_NE(query_frame, nullptr);
    FrameView view(query_frame);
    sm_->handle_query_md_subscriptions(view);

    auto* rtn_frame = reader_->next_frame();
    ASSERT_NE(rtn_frame, nullptr);
    FrameView rtn_view(rtn_frame);
    auto rsp = nlohmann::json::parse(
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()),
        reinterpret_cast<const char*>(rtn_view.ext_inst_payload()) + rtn_view.ext_inst_payload_size());

    // 3 string elements (IF2506, IC2506, NONEXISTENT), 2 non-string skipped
    EXPECT_EQ(rsp["returned_count"], 3);
    EXPECT_EQ(rsp["total_matched"], 3);
    EXPECT_FALSE(rsp["truncated"]);
    EXPECT_TRUE(rsp["error"].is_null());

    // 契约: instruments 模式按请求顺序
    EXPECT_EQ(rsp["subscriptions"][0]["instrument"], "IF2506");
    EXPECT_EQ(rsp["subscriptions"][0]["sub_state"], "Subscribed");
    EXPECT_FALSE(rsp["subscriptions"][0]["subscribers"].empty());

    // 契约: 不存在的合约返回 NotRequested + 空 subscribers
    EXPECT_EQ(rsp["subscriptions"][1]["instrument"], "IC2506");
    EXPECT_EQ(rsp["subscriptions"][1]["sub_state"], "NotRequested");
    EXPECT_TRUE(rsp["subscriptions"][1]["subscribers"].empty());

    EXPECT_EQ(rsp["subscriptions"][2]["instrument"], "NONEXISTENT");
    EXPECT_EQ(rsp["subscriptions"][2]["sub_state"], "NotRequested");
    EXPECT_TRUE(rsp["subscriptions"][2]["subscribers"].empty());
}

// Test 22: confirmed orphan (last subscriber left while Pending) -> optimistic unsub signal,
// state-first NotRequested (新订阅者在退订在途窗口内必须能触发重发, 否则少订)
TEST_F(SubscriptionManagerTest, OnSubConfirmedOrphanNeedsUnsub) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->unsubscribe("stg.alpha", {"IF2506"});  // Pending + empty subscribers -> entry kept
    bool need_unsub = sm_->on_sub_confirmed("IF2506", true);
    EXPECT_TRUE(need_unsub);
    auto* info = sm_->find_instrument("IF2506");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->sub_state, SubState::NotRequested);
}

// Test 23: confirmed with subscribers -> no unsub signal, Subscribed
TEST_F(SubscriptionManagerTest, OnSubConfirmedWithSubscribersNoUnsub) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    EXPECT_FALSE(sm_->on_sub_confirmed("IF2506", true));
    EXPECT_EQ(sm_->find_instrument("IF2506")->sub_state, SubState::Subscribed);
}

// Test 24: orphan unsub in-flight window - new subscriber triggers resubscribe
// (确认后乐观退订把状态先置 NotRequested, 窗口内新订阅返回 to_subscribe 重发,
//  依赖 CTP 同连接 FIFO: unsub 先处理 sub 后处理, 最终一致)
TEST_F(SubscriptionManagerTest, ConfirmedOrphanWindowResubscribe) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    ASSERT_TRUE(sm_->on_sub_confirmed("IF2506", true));  // orphan -> NotRequested + unsub signal
    auto result = sm_->subscribe("stg.beta", {"IF2506"});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], "IF2506");
}

// Test 25: confirm-failed orphan -> erased (CTP 明确拒绝订阅, 侧无订阅, 无需退订)
TEST_F(SubscriptionManagerTest, OnSubConfirmedFailedOrphanErased) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    EXPECT_FALSE(sm_->on_sub_confirmed("IF2506", false));
    EXPECT_EQ(sm_->find_instrument("IF2506"), nullptr);
}

// Test 26: orphan optimistic unsub completes - unsub confirmed erases entry
TEST_F(SubscriptionManagerTest, OrphanOptimisticUnsubConfirmedErases) {
    sm_->subscribe("stg.alpha", {"IF2506"});
    sm_->mark_pending({"IF2506"});
    sm_->unsubscribe("stg.alpha", {"IF2506"});
    ASSERT_TRUE(sm_->on_sub_confirmed("IF2506", true));
    sm_->on_unsub_confirmed("IF2506");
    EXPECT_EQ(sm_->find_instrument("IF2506"), nullptr);
}

}  // namespace
