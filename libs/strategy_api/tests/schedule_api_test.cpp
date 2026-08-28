#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;

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

// 定时器/拦截/上限语义测试: 临时 DZTRADER_HOME + 预建事件/md 通道 + dz_init。
// 测试进程用独立 writer 模拟平台广播帧, 经 dz_next_event 观察 SDK 拦截/放行行为。
class ScheduleApiTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_strategy_schedule").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");
        ctx_ = dz_init();
        ASSERT_NE(ctx_, nullptr) << "dz_init failed: " << dz_errmsg();
    }

    void TearDown() override {
        dz_release(ctx_);
        std::filesystem::remove_all(home_);
    }

    std::shared_ptr<ChannelMeta> open_event_meta() {
        return std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    /// basic struct 帧 (DzFrameHeader + DzShmPreload)
    void emit_basic(DzFrameType type, const DzShmPreload& params) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        ASSERT_TRUE(writer.write_frame(type, params));
        writer.notify_subscribers();
    }

    /// 含 instance_id 的变长帧
    void emit_ext_inst(DzFrameType type, const std::string& instance) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        ASSERT_TRUE(writer.write_ext_inst_frame(type, instance.c_str(), nullptr, 0));
        writer.notify_subscribers();
    }

    /// 无 instance_id 的变长帧
    void emit_ext(DzFrameType type) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        ASSERT_TRUE(writer.write_ext_frame(type, nullptr, 0));
        writer.notify_subscribers();
    }

    /// 等待 timeout_ms 内出现 target_id 的定时器帧, 返回 payload 指针 (ctx 内部缓冲,
    /// 下次 dz_next_event 前有效)。超时返回 nullptr。
    /// 守卫定时器兜底: 即使目标定时器实现异常, dz_wait 也不会永久挂死。
    const DzTimerEvent* wait_timer_frame(DzTimerId target_id, uint32_t timeout_ms) {
        const DzTimerId guard = dz_schedule_after(ctx_, static_cast<int32_t>(timeout_ms) + 500);
        // 注: gtest ASSERT_* 失败分支为 void, 不可用于非 void 函数
        if (guard == DZ_TIMER_INVALID) {
            ADD_FAILURE() << "guard schedule failed: " << dz_errmsg();
            return nullptr;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms + 2000);
        while (std::chrono::steady_clock::now() < deadline) {
            dz_wait(ctx_);
            for (;;) {
                const void* frame = dz_next_event(ctx_);
                if (frame == nullptr) {
                    break;
                }
                const auto view = FrameView(static_cast<const std::byte*>(frame));
                if (view.type() != DZ_FRAME_STG_TIMER) {
                    continue;
                }
                const auto& ev = view.payload<DzTimerEvent>();
                if (ev.timer_id == guard) {
                    return nullptr;
                }
                if (ev.timer_id == target_id) {
                    (void)dz_schedule_cancel(ctx_, guard);  // 守卫已完成使命
                    return &ev;
                }
            }
        }
        (void)dz_schedule_cancel(ctx_, guard);
        return nullptr;
    }

    /// 兜底线程: timeout 后 notify 唤醒 dz_wait, 防实现缺陷导致测试挂死。
    /// done 置位后线程尽快退出 (不 notify)。
    std::thread rescue_wait(std::atomic<bool>& done, uint32_t timeout_ms) {
        return std::thread([&done, this, timeout_ms] {
            const int steps = static_cast<int>(timeout_ms / 100);
            for (int i = 0; i < steps; ++i) {
                if (done.load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            dz_notify_self(ctx_);
        });
    }
};

// ── dz_schedule_after / dz_schedule_every ──

TEST_F(ScheduleApiTest, ScheduleAfterFiresOnceWithTimerFrame) {
    const DzTimerId id = dz_schedule_after(ctx_, 30);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 1500);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);
    // 一次性: 不再触发
    EXPECT_EQ(wait_timer_frame(id, 300), nullptr);
    // 已触发: cancel 返回 false + TIMER_NOT_FOUND
    EXPECT_FALSE(dz_schedule_cancel(ctx_, id));
    EXPECT_EQ(dz_errcode(), DZ_EC_TIMER_NOT_FOUND);
}

TEST_F(ScheduleApiTest, ScheduleRejectsInvalidDelay) {
    EXPECT_EQ(dz_schedule_after(ctx_, 0), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(dz_schedule_after(ctx_, -5), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(dz_schedule_every(ctx_, 0), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(ScheduleApiTest, ScheduleEveryFiresRepeatedlyAndCancelStops) {
    const DzTimerId id = dz_schedule_every(ctx_, 30);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    for (int i = 0; i < 3; ++i) {
        const auto* ev = wait_timer_frame(id, 1500);
        ASSERT_NE(ev, nullptr);
        EXPECT_EQ(ev->timer_id, id);
    }
    EXPECT_TRUE(dz_schedule_cancel(ctx_, id));
    EXPECT_EQ(wait_timer_frame(id, 300), nullptr);
}

// ── dz_schedule_at / dz_schedule_daily ──

TEST_F(ScheduleApiTest, ScheduleRejectsInvalidTimeOfDay) {
    EXPECT_EQ(dz_schedule_at(ctx_, -1), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(dz_schedule_at(ctx_, 86'400'000), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
    EXPECT_EQ(dz_schedule_daily(ctx_, -1), DZ_TIMER_INVALID);
    EXPECT_EQ(dz_errcode(), DZ_EC_INVALID_PARAM);
}

TEST_F(ScheduleApiTest, ScheduleAtFiresAtNextOccurrence) {
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    const int32_t target = (now_tod + 1000) % 86'400'000;
    const DzTimerId id = dz_schedule_at(ctx_, target);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 3000);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);
    EXPECT_EQ(wait_timer_frame(id, 300), nullptr);  // 一次性
}

TEST_F(ScheduleApiTest, ScheduleDailyFiresFirstOccurrence) {
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    const int32_t target = (now_tod + 1000) % 86'400'000;
    const DzTimerId id = dz_schedule_daily(ctx_, target);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 3000);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);
    EXPECT_TRUE(dz_schedule_cancel(ctx_, id));  // 已重排到次日, cancel 停掉
}

// ── cancel / cancel_all ──

TEST_F(ScheduleApiTest, ScheduleCancelUnknownIdFailsWithTimerNotFound) {
    EXPECT_FALSE(dz_schedule_cancel(ctx_, 12345));
    EXPECT_EQ(dz_errcode(), DZ_EC_TIMER_NOT_FOUND);
    EXPECT_FALSE(dz_schedule_cancel(ctx_, DZ_TIMER_INVALID));
    EXPECT_EQ(dz_errcode(), DZ_EC_TIMER_NOT_FOUND);
}

TEST_F(ScheduleApiTest, ScheduleCancelAllClearsPendingTimers) {
    const DzTimerId a = dz_schedule_after(ctx_, 30);
    const DzTimerId b = dz_schedule_every(ctx_, 30);
    ASSERT_NE(a, DZ_TIMER_INVALID);
    ASSERT_NE(b, DZ_TIMER_INVALID);
    EXPECT_TRUE(dz_schedule_cancel_all(ctx_));
    EXPECT_EQ(wait_timer_frame(a, 200), nullptr);
    EXPECT_EQ(wait_timer_frame(b, 200), nullptr);
    EXPECT_FALSE(dz_schedule_cancel(ctx_, a));
    EXPECT_EQ(dz_errcode(), DZ_EC_TIMER_NOT_FOUND);
}

TEST_F(ScheduleApiTest, CancelAllDoesNotAffectInternalTimers) {
    // 触发内部 preload 定时器 (随机 0-5s)
    DzShmPreload params{};
    emit_basic(DZ_FRAME_PRELOAD_EVENT_SHM, params);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);  // 广播帧被内部消费, 用户不可见

    // 用户层 cancel_all: 不得影响内部定时器
    EXPECT_TRUE(dz_schedule_cancel_all(ctx_));

    // 内部 preload 定时器应在 <=5s 内唤醒 dz_wait (rescue 7s 兜底防挂死)
    std::atomic<bool> done{false};
    std::thread rescue = rescue_wait(done, 7000);
    const auto t0 = std::chrono::steady_clock::now();
    dz_wait(ctx_);
    done = true;
    rescue.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(6));
}

TEST_F(ScheduleApiTest, PreloadMdShmOwnSourceSchedulesInternalTimer) {
    // PRELOAD_MD_SHM 仅 instance_id == 本策略行情源 才排内部随机延迟
    DzShmPreload params{};
    {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        const auto* raw = reinterpret_cast<const std::byte*>(&params);
        ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_PRELOAD_MD_SHM, "test_md", raw,
                                                sizeof(params)));
        writer.notify_subscribers();
    }
    EXPECT_EQ(dz_next_event(ctx_), nullptr);  // 用户不可见

    std::atomic<bool> done{false};
    std::thread rescue = rescue_wait(done, 7000);
    const auto t0 = std::chrono::steady_clock::now();
    dz_wait(ctx_);  // 由内部 md 预加载定时器在 <=5s 内唤醒
    done = true;
    rescue.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(6));
}

// ── 处理优先级: 用户帧 > 定时器帧 > 内部帧 ──

TEST_F(ScheduleApiTest, UserFrameTakesPriorityOverBufferedTimerFrame) {
    const DzTimerId tid = dz_schedule_after(ctx_, 20);
    ASSERT_NE(tid, DZ_TIMER_INVALID);
    // 先确保定时器已到期 (dz_wait 可能被残留信号量计数提前唤醒, 不能依赖其等待)
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    dz_wait(ctx_);  // tick: 定时器帧合成进缓冲 (未领取)

    // 用户帧到达: 优先于已缓冲定时器帧返回
    emit_ext_inst(DZ_FRAME_STG_USER_INPUT, dz_strategy_id(ctx_));
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_STG_USER_INPUT);

    // 通道空后服务计时器: 第二次调用返回定时器帧
    frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    const auto view = FrameView(static_cast<const std::byte*>(frame));
    EXPECT_EQ(view.type(), DZ_FRAME_STG_TIMER);
    EXPECT_EQ(view.payload<DzTimerEvent>().timer_id, tid);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

// ── 内部帧拦截与 32 上限 ──

TEST_F(ScheduleApiTest, InternalFramesInterceptedAndCappedAt32) {
    // 5 内部 + 1 用户: 同一次调用返回用户帧
    for (int i = 0; i < 5; ++i) {
        emit_ext(DZ_FRAME_QUERY_FULL_SNAPSHOT);
    }
    emit_ext_inst(DZ_FRAME_STG_USER_INPUT, dz_strategy_id(ctx_));
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_STG_USER_INPUT);

    // 33 内部 + 1 用户: 首次调用最多消费 32 条内部帧后返回 NULL, 第二次拿到用户帧
    for (int i = 0; i < 33; ++i) {
        emit_ext(DZ_FRAME_QUERY_FULL_SNAPSHOT);
    }
    emit_ext_inst(DZ_FRAME_STG_USER_INPUT, dz_strategy_id(ctx_));
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_STG_USER_INPUT);
}

TEST_F(ScheduleApiTest, ShutdownFrameDirectedOwnInstanceDeliveredAfterCleanup) {
    // 制造 pending 内部预加载定时器
    DzShmPreload params{};
    emit_basic(DZ_FRAME_PRELOAD_EVENT_SHM, params);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);  // 内部消费, 随机延迟已排

    // 用户周期定时器: SHUTDOWN 内部清理不得影响
    const DzTimerId tid = dz_schedule_every(ctx_, 200);
    ASSERT_NE(tid, DZ_TIMER_INVALID);

    // 依次写: 非本策略定向 SHUTDOWN + 本策略定向 SHUTDOWN
    emit_ext_inst(DZ_FRAME_REQUEST_SHUTDOWN, "other_strategy");
    emit_ext_inst(DZ_FRAME_REQUEST_SHUTDOWN, dz_strategy_id(ctx_));

    // 一次 dz_next_event: other 被拦截, own 内部清理后放行
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_REQUEST_SHUTDOWN);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 用户定时器不受清理影响: 仍周期触发
    const auto* ev = wait_timer_frame(tid, 1500);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, tid);
    EXPECT_TRUE(dz_schedule_cancel(ctx_, tid));

    // 内部预加载定时器已清 + 无用户定时器: dz_wait 无定时目标,
    // rescue 唤醒后超时 (7s) 证明 SHUTDOWN 清理生效
    std::atomic<bool> done{false};
    std::thread rescue = rescue_wait(done, 7000);
    const auto t0 = std::chrono::steady_clock::now();
    dz_wait(ctx_);
    done = true;
    rescue.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::seconds(6));
}

// ── 定时器帧缓冲满: 推迟投递, 绝不丢帧 ──

TEST_F(ScheduleApiTest, TimerBurstBeyondBufferIsNotLost) {
    constexpr int kCount = 40;  // > 32 槽
    std::set<DzTimerId> ids;
    for (int i = 0; i < kCount; ++i) {
        const DzTimerId id = dz_schedule_after(ctx_, 50);
        ASSERT_NE(id, DZ_TIMER_INVALID);
        ids.insert(id);
    }

    std::set<DzTimerId> fired;
    // 正确实现: 40 个定时器同拍触发, 32 帧进缓冲 + 8 deferred, 一次 dz_wait + 排干即集齐;
    // 缺陷实现: 丢失帧时 rescue 兜底唤醒防挂死。
    std::atomic<bool> done{false};
    std::thread rescue = rescue_wait(done, 6000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fired.size() < static_cast<size_t>(kCount) && std::chrono::steady_clock::now() < deadline) {
        dz_wait(ctx_);
        for (;;) {
            const void* frame = dz_next_event(ctx_);
            if (frame == nullptr) {
                break;
            }
            const auto view = FrameView(static_cast<const std::byte*>(frame));
            if (view.type() != DZ_FRAME_STG_TIMER) {
                continue;
            }
            const auto& ev = view.payload<DzTimerEvent>();
            if (ids.count(ev.timer_id) == 0) {
                continue;
            }
            EXPECT_EQ(fired.count(ev.timer_id), 0u);  // 每个 ID 恰好一帧
            fired.insert(ev.timer_id);
        }
    }
    done = true;
    rescue.join();
    EXPECT_EQ(fired, ids);  // 40 帧无丢失 (32 缓冲 + 8 deferred 补投)
}

}  // namespace
