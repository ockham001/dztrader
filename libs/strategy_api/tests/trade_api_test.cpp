#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include "output_limit.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

constexpr uint64_t kMB = 1024 * 1024;

void create_event_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

void create_md_channel(const std::filesystem::path& shm_dir) {
    ChannelConfig cfg{
        .channel_name = "test_md",
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)ChannelMeta::open_or_create(cfg);
}

// 与 notify_ui_test 同款 fixture: 临时 DZTRADER_HOME + 预建事件/md 通道 + dz_init。
// SDK 写出的 TD_ORDER_REQ/TD_ORDER_CANCEL_REQ/SET_LOGICAL_POSITION 等帧在新语义下
// 被 dz_next_event 拦截 (平台内部帧), 故用独立 probe reader 观察写帧。
class TradeApiTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;
    /// dz_init 后创建, 定位其后: 可观察 SDK 写出的全部帧 (Reader 无默认构造, 用 optional)
    std::optional<Reader> probe_;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_trade").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");
        ctx_ = dz_init();
        ASSERT_NE(ctx_, nullptr) << "dz_init failed: " << dz_errmsg();
        probe_.emplace(Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                      home_ + "/shm", "trade_test_probe"));
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    /// 从 probe reader 读取下一帧并断言为 basic struct 帧, 返回 payload 指针 (非拥有)
    const void* read_next_basic(DzFrameType type) {
        const std::byte* frame = probe_->next_frame();
        if (frame == nullptr) {
            return nullptr;
        }
        const auto view = dztrader::shm::FrameView(frame);
        EXPECT_EQ(view.type(), type);
        return frame + sizeof(DzFrameHeader);
    }

    /// 排空本策略信号量的残留计数: 通道写帧 (notify_subscribers) 与告警都会 notify
    /// 本策略信号量, 用例间/跨运行残留会让 dz_wait 提前返回。非阻塞 wait_for(0)
    /// 逐个消费, 直至计数为 0 (排空后才可断言「下一次 dz_wait 必须阻塞」)。
    void drain_signal() {
        dztrader::shm::NamedSemaphore sem(std::string(dztrader::STRATEGY_PREFIX) + "." +
                                          dztrader::this_process::exe_stem());
        while (sem.wait_for(0)) {
        }
    }

    /// 把事件通道推到当前页末尾 (只剩 8 字节), 并在下一页文件名处放一个目录,
    /// 使跨页 get_page 必然失败: 后续 open_frame 返回 nullptr。不删除/改写已映射页
    /// 文件 (Windows 下映射中的文件不可删), 因此对 Windows/Linux 同时有效。
    void break_event_channel_page_growth() {
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
        const uint64_t page_size = meta->page_size();
        const uint64_t offset = meta->next_write_pos()->load() % page_size;
        // 帧长 = 帧头 + data_size; 结束时 nwp 距页尾恰好 8 字节 (不足一帧帧头)
        const uint64_t filler_size = page_size - offset - sizeof(DzFrameHeader) - 8;
        ASSERT_EQ(filler_size % 8, 0u);
        MultiWriter writer = MultiWriter::create(meta, "trade_test_page_filler");
        std::byte* raw = writer.open_frame(DZ_FRAME_NOTIFY_UI, static_cast<uint32_t>(filler_size));
        ASSERT_NE(raw, nullptr);
        writer.close_frame();
        const uint64_t nwp = meta->next_write_pos()->load();
        ASSERT_EQ(nwp % page_size, page_size - 8);

        // 本帧起点仍在当前页, 下一次写必然跨到下一页
        const auto next_page_path =
            meta->page_dir() / std::format("{:08d}.dat", nwp / page_size + 1);
        std::error_code ec;
        ASSERT_TRUE(std::filesystem::create_directory(next_page_path, ec)) << ec.message();
        while (probe_->next_frame() != nullptr) {
        }  // 消费填充帧, 之后 probe 流必须保持为空
    }
};

// ── 成功路径: payload 正确落帧 ──

TEST_F(TradeApiTest, PlaceOrderWritesOrderReq) {
    const DzOrderId id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG, DZ_PRICE_LIMIT,
                                        1234.5, 2, DZ_POSITION_EFFECT_OPEN);
    ASSERT_GE(id, 0);
    const auto* req = static_cast<const DzOrderReq*>(read_next_basic(DZ_FRAME_TD_ORDER_REQ));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->order_id, id);
    EXPECT_STREQ(req->strategy_id, dz_strategy_id(ctx_));
    EXPECT_STREQ(req->account_id, "CTP001");
    EXPECT_STREQ(req->instrument_id, "IF2401");
    EXPECT_EQ(req->direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(req->price_type, DZ_PRICE_LIMIT);
    EXPECT_DOUBLE_EQ(req->price, 1234.5);
    EXPECT_EQ(req->volume, 2);
    EXPECT_EQ(req->position_effect, DZ_POSITION_EFFECT_OPEN);
}

TEST_F(TradeApiTest, CancelOrderWritesOrderCancelReq) {
    ASSERT_TRUE(dz_cancel_order(ctx_, "CTP001", 42));
    const auto* req =
        static_cast<const DzOrderCancelReq*>(read_next_basic(DZ_FRAME_TD_ORDER_CANCEL_REQ));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->order_id, 42);
    EXPECT_STREQ(req->account_id, "CTP001");
}

TEST_F(TradeApiTest, SetLogicalPositionWritesFrame) {
    ASSERT_TRUE(dz_set_logical_position(ctx_, "CTP001", "IF2401", 3));
    const auto* pos =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(pos, nullptr);
    EXPECT_STREQ(pos->account_id, "CTP001");
    EXPECT_STREQ(pos->instrument_id, "IF2401");
    EXPECT_STREQ(pos->strategy_id, dz_strategy_id(ctx_));
    EXPECT_EQ(pos->net_volume, 3);
}

TEST_F(TradeApiTest, SetLogicalPositionLargeVolumeRoundTrip) {
    // 逻辑持仓为聚合量 (DzLargeVolume): 超过 int32 上限的值必须原样写入帧
    constexpr DzLargeVolume kNetVolume = 5'000'000'000;
    ASSERT_TRUE(dz_set_logical_position(ctx_, "CTP001", "IF2401", kNetVolume));
    const auto* pos =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(pos, nullptr);
    EXPECT_EQ(pos->net_volume, kNetVolume);
}

TEST_F(TradeApiTest, SetLogicalPositionWildcardZeroesNetVolume) {
    // 契约 strategy: account/instrument 为空串表示通配, net_volume 忽略并固定为 0;
    // 非通配字段与 strategy_id 必须原样保留
    ASSERT_TRUE(dz_set_logical_position(ctx_, "", "IF2401", 5));
    const auto* account_wildcard =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(account_wildcard, nullptr);
    EXPECT_EQ(account_wildcard->net_volume, 0);
    EXPECT_STREQ(account_wildcard->account_id, "");
    EXPECT_STREQ(account_wildcard->instrument_id, "IF2401");
    EXPECT_STREQ(account_wildcard->strategy_id, dz_strategy_id(ctx_));

    ASSERT_TRUE(dz_set_logical_position(ctx_, "CTP001", nullptr, -2));
    const auto* instrument_wildcard =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(instrument_wildcard, nullptr);
    EXPECT_EQ(instrument_wildcard->net_volume, 0);
    EXPECT_STREQ(instrument_wildcard->account_id, "CTP001");
    EXPECT_STREQ(instrument_wildcard->instrument_id, "");
    EXPECT_STREQ(instrument_wildcard->strategy_id, dz_strategy_id(ctx_));

    // 双通配 (NULL/NULL): 仍写帧, net_volume 置 0
    ASSERT_TRUE(dz_set_logical_position(ctx_, nullptr, nullptr, 7));
    const auto* both_wildcard =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(both_wildcard, nullptr);
    EXPECT_EQ(both_wildcard->net_volume, 0);
    EXPECT_STREQ(both_wildcard->strategy_id, dz_strategy_id(ctx_));

    // 非通配负值原样保留
    ASSERT_TRUE(dz_set_logical_position(ctx_, "CTP001", "IF2401", -4));
    const auto* explicit_pos =
        static_cast<const DzLogicalPosition*>(read_next_basic(DZ_FRAME_SET_LOGICAL_POSITION));
    ASSERT_NE(explicit_pos, nullptr);
    EXPECT_EQ(explicit_pos->net_volume, -4);
}

TEST_F(TradeApiTest, QueryAccountStatusWritesBasicFrame) {
    ASSERT_TRUE(dz_query_account_status(ctx_, "CTP001"));
    const auto* req =
        static_cast<const DzAccountStatusReq*>(read_next_basic(DZ_FRAME_TD_QUERY_ACCOUNT_STATUS));
    ASSERT_NE(req, nullptr);
    EXPECT_STREQ(req->account_id, "CTP001");
}

TEST_F(TradeApiTest, QueryAccountStatusNullMeansAll) {
    ASSERT_TRUE(dz_query_account_status(ctx_, nullptr));
    const auto* req =
        static_cast<const DzAccountStatusReq*>(read_next_basic(DZ_FRAME_TD_QUERY_ACCOUNT_STATUS));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->account_id[0], '\0');

    ASSERT_TRUE(dz_query_account_status(ctx_, ""));
    req = static_cast<const DzAccountStatusReq*>(read_next_basic(DZ_FRAME_TD_QUERY_ACCOUNT_STATUS));
    ASSERT_NE(req, nullptr);
    EXPECT_EQ(req->account_id[0], '\0');
}

// ── 生命周期边界 ──

TEST_F(TradeApiTest, SecondInitReturnsNullWithAlreadyInitialized) {
    // 失败的重复 init 不得破坏活动会话: 身份与下单能力应保持
    const char* const id_before = dz_strategy_id(ctx_);
    EXPECT_EQ(dz_init(), nullptr);
    EXPECT_EQ(dz_errcode(), DZ_EC_STRATEGY_ALREADY_INITIALIZED);
    EXPECT_STREQ(dz_strategy_id(ctx_), id_before);
    EXPECT_GE(dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG, DZ_PRICE_LIMIT, 1, 1,
                             DZ_POSITION_EFFECT_OPEN),
              0);
}

TEST_F(TradeApiTest, ReleaseNullIsNoOp) {
    // release(NULL) no-op: 不得破坏当前会话的可用性
    const char* const id_before = dz_strategy_id(ctx_);
    dz_release(nullptr);
    EXPECT_STREQ(dz_strategy_id(ctx_), id_before);
    const DzOrderId order_id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG,
                                              DZ_PRICE_LIMIT, 1, 1, DZ_POSITION_EFFECT_OPEN);
    EXPECT_GE(order_id, 0);
}

TEST_F(TradeApiTest, ReinitAfterReleaseSucceeds) {
    dz_release(ctx_);
    ctx_ = dz_init();
    ASSERT_NE(ctx_, nullptr);
    // 新句柄应功能正常
    const DzOrderId order_id = dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG,
                                              DZ_PRICE_LIMIT, 1, 1, DZ_POSITION_EFFECT_OPEN);
    EXPECT_GE(order_id, 0);
}

// ── 句柄化会话函数覆盖（防签名回归） ──

TEST_F(TradeApiTest, StrategyHomeNonEmpty) {
    EXPECT_NE(dz_strategy_home(ctx_), nullptr);
    EXPECT_NE(dz_strategy_home(ctx_)[0], '\0');
}

TEST_F(TradeApiTest, StrategyHomeEndsWithSeparator) {
    // api.h: 策略目录 = 可执行文件所在目录 + 平台分隔符; 已有分隔符不重复追加
    const std::string home = dz_strategy_home(ctx_);
    ASSERT_FALSE(home.empty());
    constexpr char kPreferredSeparator =
        static_cast<char>(std::filesystem::path::preferred_separator);
    EXPECT_EQ(home.back(), kPreferredSeparator);
    // 断言完整路径: exe_dir + 单个分隔符 (排除错路径/双分隔符)
    std::string expected = dztrader::this_process::exe_dir().string();
    if (expected.empty() || expected.back() != kPreferredSeparator) {
        expected.push_back(kPreferredSeparator);
    }
    EXPECT_EQ(home, expected);
}

TEST_F(TradeApiTest, OutputUiWritesExtInstFrame) {
    constexpr std::string_view kText = R"({"action":"paused"})";
    ASSERT_TRUE(dz_output_ui(ctx_, std::string(kText).c_str()));
    const std::byte* frame = probe_->next_frame();
    ASSERT_NE(frame, nullptr);
    const auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_OUTPUT_UI);
    EXPECT_STREQ(view.ext_inst_id(), dz_strategy_id(ctx_));
    EXPECT_EQ(view.ext_inst_payload_size(), kText.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(view.ext_inst_payload()),
                               view.ext_inst_payload_size()),
              kText);
}

TEST_F(TradeApiTest, OutputUiTruncatesPayloadAtPageCap) {
    // 1MB 页的 payload 上限 = (1MB - 帧头 80) & ~7; 超出部分必须截断,
    // 否则 open_frame 因 frame_size > page_size 直接失败 (页感知截断的回归锚点)
    const uint64_t cap = dztrader::output_ui_max_payload(kMB);
    const std::string oversized(static_cast<size_t>(cap) + 4096, 'x');
    ASSERT_GT(oversized.size(), cap);
    ASSERT_TRUE(dz_output_ui(ctx_, oversized.c_str()));

    const std::byte* frame = probe_->next_frame();
    ASSERT_NE(frame, nullptr);
    const auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_OUTPUT_UI);
    EXPECT_EQ(view.ext_inst_payload_size(), cap);
    // 截断后的 payload 内容 = 原文前缀 (长度与内容同时可观察)
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(view.ext_inst_payload()),
                               view.ext_inst_payload_size()),
              std::string_view(oversized).substr(0, static_cast<size_t>(cap)));
}

TEST_F(TradeApiTest, OutputUiRejectsPageWithoutPayloadRoom) {
    // 真实通道页容量下限 1MB (shm ChannelMeta::validate_config), cap==0 经公开
    // 入口不可达; 经 api.cpp 的页容量注入点覆盖防御分支: false + BUFFER_TOO_SMALL
    EXPECT_FALSE(dztrader::output_ui_write(ctx_, "x", dztrader::kOutputUiFrameOverhead));
    EXPECT_EQ(dz_errcode(), DZ_EC_BUFFER_TOO_SMALL);
    EXPECT_EQ(probe_->next_frame(), nullptr);  // 拒绝时不得写帧

    // 空 payload 不占 payload 空间: len==0 不触发 cap==0 拒绝
    ASSERT_TRUE(dztrader::output_ui_write(ctx_, "", dztrader::kOutputUiFrameOverhead));
    const std::byte* frame = probe_->next_frame();
    ASSERT_NE(frame, nullptr);
    const auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_OUTPUT_UI);
    EXPECT_EQ(view.ext_inst_payload_size(), 0u);
}

TEST_F(TradeApiTest, QueryBarReturnsNotImplemented) {
    // 归档域未实现: 保留接口但恒失败, 不是"看起来能用"的查询
    EXPECT_EQ(dz_db_query_bar(nullptr, "IF2401", 60, DZ_ADJUST_NONE, 0, 0), nullptr);
    EXPECT_EQ(dz_errcode(), DZ_EC_NOT_IMPLEMENTED);
}

TEST_F(TradeApiTest, DbCloseIsNoFail) {
    EXPECT_TRUE(dz_db_close(nullptr));  // NULL 安全 no-op
}

TEST(DzErrstrTest, MapsDbAndNotImplementedCodes) {
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_OPEN_FAILED), "db open failed");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_CLOSED), "db closed");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_QUERY_FAILED), "db query failed");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_WRITE_FAILED), "db write failed");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_MIGRATE_FAILED), "db migrate failed");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_UNSUPPORTED_BACKEND), "db unsupported backend");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_UNSUPPORTED_CAPABILITY), "db unsupported capability");
    EXPECT_STREQ(dz_errstr(DZ_EC_DB_TRANSACTION_FAILED), "db transaction failed");
    EXPECT_STREQ(dz_errstr(DZ_EC_NOT_IMPLEMENTED), "not implemented");
    EXPECT_STREQ(dz_errstr(-99999), "unknown error");
}

TEST_F(TradeApiTest, PlaceOrderFailsWhenEventChannelCannotGrow) {
    break_event_channel_page_growth();
    // open_frame 跨页失败 (通道无法扩展): 返回 -1 + 底层错误码, 不写帧
    EXPECT_EQ(dz_place_order(ctx_, "CTP001", "IF2401", DZ_DIRECTION_LONG, DZ_PRICE_LIMIT, 1, 1,
                             DZ_POSITION_EFFECT_OPEN),
              -1);
    EXPECT_EQ(dz_errcode(), DZ_EC_SHM_OPEN_FAILED);
    EXPECT_EQ(probe_->next_frame(), nullptr);
}

TEST_F(TradeApiTest, CancelOrderFailsWhenEventChannelCannotGrow) {
    break_event_channel_page_growth();
    EXPECT_FALSE(dz_cancel_order(ctx_, "CTP001", 42));
    EXPECT_EQ(dz_errcode(), DZ_EC_SHM_OPEN_FAILED);
    EXPECT_EQ(probe_->next_frame(), nullptr);
}

TEST_F(TradeApiTest, WakeupWakesWait) {
    drain_signal();
    dz_wakeup(ctx_);  // 投递一次信号量计数
    const auto t0 = std::chrono::steady_clock::now();
    dz_wait(ctx_);  // 已有计数: 必须立即返回 (阻塞即证明计数丢失)
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));

    // 计数已被消费: 下一次 dz_wait 必须阻塞 (未阻塞说明信号量未计数/消费无效)。
    // 断死由 ctest TIMEOUT 兜底; 正常路径下方 dz_wakeup 必定解除阻塞。
    std::atomic<bool> returned{false};
    std::thread waiter([this, &returned] {
        dz_wait(ctx_);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(returned.load());
    dz_wakeup(ctx_);
    waiter.join();
    EXPECT_TRUE(returned.load());
}

TEST_F(TradeApiTest, WaitReturnsAfterWakeup) {
    drain_signal();
    // waker 在 delay 后置位并投递信号量: 返回时 notified 必为 true, 且耗时不低于
    // 下界 — dz_wait 未阻塞/立即返回两种缺陷都会变红。
    constexpr auto kDelay = std::chrono::milliseconds(100);
    std::atomic<bool> notified{false};
    std::thread waker([this, &notified, kDelay]() {
        std::this_thread::sleep_for(kDelay);
        notified = true;
        dz_wakeup(ctx_);
    });
    const auto t0 = std::chrono::steady_clock::now();
    dz_wait(ctx_);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    waker.join();
    EXPECT_TRUE(notified.load());
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
}

}  // namespace
