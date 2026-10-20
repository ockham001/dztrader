#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/core/string_util.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

    /// basic struct 帧 (任意对齐 struct payload)
    template <typename T>
    void emit_struct(DzFrameType type, const T& payload) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        ASSERT_TRUE(writer.write_frame(type, payload));
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
    const DzScheduleEvent* wait_timer_frame(DzTimerId target_id, uint32_t timeout_ms) {
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
                if (view.type() != DZ_FRAME_SCHEDULE) {
                    continue;
                }
                const auto& ev = view.payload<DzScheduleEvent>();
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

    /// 兜底线程: timeout 后 wakeup 唤醒 dz_wait, 防实现缺陷导致测试挂死。
    /// done 置位后线程尽快退出 (不 wakeup)。
    std::thread rescue_wait(std::atomic<bool>& done, uint32_t timeout_ms) {
        return std::thread([&done, this, timeout_ms] {
            const int steps = static_cast<int>(timeout_ms / 100);
            for (int i = 0; i < steps; ++i) {
                if (done.load()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            dz_wakeup(ctx_);
        });
    }

    /// 阻塞等待到 steady_clock 截止点 (定时器到期)。dz_wait 可能被残留信号量计数
    /// 提前唤醒, 故循环至截止点; 不以墙钟 sleep 当同步手段。
    void wait_until(std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            dz_wait(ctx_);
        }
    }

    /// 在 duration_ms 窗口内收集全部定时器触发 ID (守卫定时器保证 dz_wait 会返回)。
    std::vector<DzTimerId> collect_timer_ids(uint32_t duration_ms) {
        std::vector<DzTimerId> ids;
        const DzTimerId guard = dz_schedule_after(ctx_, static_cast<int32_t>(duration_ms) + 500);
        if (guard == DZ_TIMER_INVALID) {
            ADD_FAILURE() << "guard schedule failed: " << dz_errmsg();
            return ids;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            dz_wait(ctx_);
            for (;;) {
                const void* frame = dz_next_event(ctx_);
                if (frame == nullptr) {
                    break;
                }
                const auto view = FrameView(static_cast<const std::byte*>(frame));
                if (view.type() != DZ_FRAME_SCHEDULE) {
                    continue;
                }
                const DzTimerId fired = view.payload<DzScheduleEvent>().timer_id;
                if (fired == guard) {
                    continue;  // 收集窗口的兜底定时器不是被测对象
                }
                ids.push_back(fired);
            }
        }
        (void)dz_schedule_cancel(ctx_, guard);
        return ids;
    }
};

// ── dz_schedule_after / dz_schedule_every ──

TEST_F(ScheduleApiTest, ScheduleAfterFiresOnceWithTimerFrame) {
    const auto scheduled_at = std::chrono::steady_clock::now();
    const DzTimerId id = dz_schedule_after(ctx_, 30);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 1500);
    const auto elapsed = std::chrono::steady_clock::now() - scheduled_at;
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);
    // 触发时刻下界: "到期前/立即触发" 的实现必红
    EXPECT_GE(elapsed, std::chrono::milliseconds(30));
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
    const auto scheduled_at = std::chrono::steady_clock::now();
    const DzTimerId id = dz_schedule_every(ctx_, 30);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    for (int i = 0; i < 3; ++i) {
        const auto* ev = wait_timer_frame(id, 1500);
        const auto elapsed = std::chrono::steady_clock::now() - scheduled_at;
        ASSERT_NE(ev, nullptr);
        EXPECT_EQ(ev->timer_id, id);
        // 第 i+1 次触发不得早于 (i+1)*delay (绝对下界, 不受观测线程调度延迟影响):
        // 立即触发或间隔塌缩的实现必红
        EXPECT_GE(elapsed, std::chrono::milliseconds(30 * (i + 1)));
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

TEST_F(ScheduleApiTest, ScheduleAcceptsTimeOfDayBounds) {
    // 契约 strategy §SCHEDULE: 参数为距午夜毫秒数, 合法区间端点 [0, 86_399_999] 必须接受
    const DzTimerId midnight = dz_schedule_at(ctx_, 0);
    ASSERT_NE(midnight, DZ_TIMER_INVALID) << dz_errmsg();
    const DzTimerId last_ms = dz_schedule_at(ctx_, 86'399'999);
    ASSERT_NE(last_ms, DZ_TIMER_INVALID) << dz_errmsg();
    EXPECT_NE(midnight, last_ms);
    // 均未到期 (排到次日), 仍可取消
    EXPECT_TRUE(dz_schedule_cancel(ctx_, midnight));
    EXPECT_TRUE(dz_schedule_cancel(ctx_, last_ms));
}

TEST_F(ScheduleApiTest, ScheduleAtFiresAtNextOccurrence) {
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    const int32_t target = (now_tod + 1000) % 86'400'000;
    const auto scheduled_at = std::chrono::steady_clock::now();
    const DzTimerId id = dz_schedule_at(ctx_, target);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 3000);
    const auto elapsed = std::chrono::steady_clock::now() - scheduled_at;
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);
    // 触发时刻下界: 目标 = 当前时刻 + ~1s, 立即触发必红
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
    EXPECT_EQ(wait_timer_frame(id, 300), nullptr);  // 一次性
}

TEST_F(ScheduleApiTest, ScheduleAtCurrentTimeOfDayDeferredToNextDay) {
    // next_time_of_day_delay 的 delta <= 0 分支 (已过/恰好等于当前时刻) 必须排到次日,
    // 不得立即触发。用「窗口内无定时器帧」确定性锚定, 不依赖午夜前 1 秒。
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    const DzTimerId id = dz_schedule_at(ctx_, now_tod);
    ASSERT_NE(id, DZ_TIMER_INVALID) << dz_errmsg();
    const auto ids = collect_timer_ids(300);
    EXPECT_TRUE(ids.empty()) << "timer fired immediately instead of next day";
    EXPECT_TRUE(dz_schedule_cancel(ctx_, id));  // 仍在注册表 (未触发)
}

TEST_F(ScheduleApiTest, ScheduleDailyFiresFirstOccurrence) {
    const int32_t now_tod = dztrader::DateTime::local_now().millisecs_since_midnight();
    const int32_t target = (now_tod + 1000) % 86'400'000;
    const DzTimerId id = dz_schedule_daily(ctx_, target);
    ASSERT_NE(id, DZ_TIMER_INVALID);
    const auto* ev = wait_timer_frame(id, 3000);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->timer_id, id);

    // 首次触发后按 wall clock 重算次日: 排一个短 after 并排空事件, 窗口内只允许
    // 短定时器触发 — Daily 立即重触发/重排失败导致保持到期, 都会在这里出帧。
    const DzTimerId short_id = dz_schedule_after(ctx_, 30);
    ASSERT_NE(short_id, DZ_TIMER_INVALID);
    const auto ids = collect_timer_ids(300);
    ASSERT_EQ(ids.size(), 1u) << "unexpected timer fires after daily first occurrence";
    EXPECT_EQ(ids.front(), short_id);

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
    emit_struct(DZ_FRAME_PRELOAD_EVENT_SHM, params);
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
        ASSERT_TRUE(
            writer.write_ext_inst_frame(DZ_FRAME_PRELOAD_MD_SHM, "test_md", raw, sizeof(params)));
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

// ── 处理优先级: 回补帧 > 延迟触发帧 > 用户帧 > 定时器帧 > 内部帧 ──

TEST_F(ScheduleApiTest, UserFrameTakesPriorityOverBufferedTimerFrame) {
    const DzTimerId tid = dz_schedule_after(ctx_, 20);
    ASSERT_NE(tid, DZ_TIMER_INVALID);
    // 等到定时器确实到期 (dz_wait 可能被残留信号量计数提前唤醒, 故循环至截止点);
    // +5ms 余量覆盖调度调用本身耗时, 保证 dz_next_event 首次 tick 即可产出触发帧
    wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(25));

    // 用户帧到达: 优先于已到期定时器帧返回
    emit_ext_inst(DZ_FRAME_UI_INPUT, dz_strategy_id(ctx_));
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_UI_INPUT);

    // 通道空后服务计时器: 第二次调用返回定时器帧
    frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    const auto view = FrameView(static_cast<const std::byte*>(frame));
    EXPECT_EQ(view.type(), DZ_FRAME_SCHEDULE);
    EXPECT_EQ(view.payload<DzScheduleEvent>().timer_id, tid);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

// ── 内部帧拦截与 32 上限 ──

TEST_F(ScheduleApiTest, InternalFramesInterceptedAndCappedAt32) {
    // 5 内部 + 1 用户: 同一次调用返回用户帧
    for (int i = 0; i < 5; ++i) {
        emit_ext(DZ_FRAME_QUERY_FULL_SNAPSHOT);
    }
    emit_ext_inst(DZ_FRAME_UI_INPUT, dz_strategy_id(ctx_));
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_UI_INPUT);

    // 33 内部 + 1 用户: 首次调用最多消费 32 条内部帧后返回 NULL, 第二次拿到用户帧
    for (int i = 0; i < 33; ++i) {
        emit_ext(DZ_FRAME_QUERY_FULL_SNAPSHOT);
    }
    emit_ext_inst(DZ_FRAME_UI_INPUT, dz_strategy_id(ctx_));
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_UI_INPUT);
}

// ── TD 回报按 strategy_id 定向过滤 ──

TEST_F(ScheduleApiTest, OwnOrderReportDelivered) {
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    const auto view = FrameView(static_cast<const std::byte*>(frame));
    EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
    const auto& got = view.payload<DzOrderReport>();
    EXPECT_EQ(std::string_view(got.strategy_id), std::string_view(dz_strategy_id(ctx_)));
}

TEST_F(ScheduleApiTest, OtherStrategyOrderReportIntercepted) {
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.strategy_id, "other_strategy", true);
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

TEST_F(ScheduleApiTest, EmptyStrategyOrderReportIntercepted) {
    DzOrderReport rpt{};  // strategy_id 空 (外部单/手工单)
    emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

TEST_F(ScheduleApiTest, OwnTradeReportDelivered) {
    DzTradeReport rpt{};
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    const auto view = FrameView(static_cast<const std::byte*>(frame));
    EXPECT_EQ(view.type(), DZ_FRAME_TRADE_REPORT);
    const auto& got = view.payload<DzTradeReport>();
    EXPECT_EQ(std::string_view(got.strategy_id), std::string_view(dz_strategy_id(ctx_)));
}

TEST_F(ScheduleApiTest, OtherStrategyTradeReportIntercepted) {
    DzTradeReport rpt{};
    dztrader::copy_string(rpt.strategy_id, "other_strategy", true);
    emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

TEST_F(ScheduleApiTest, EmptyStrategyTradeReportIntercepted) {
    DzTradeReport rpt{};  // strategy_id 空 (外部单/手工单)
    emit_struct(DZ_FRAME_TRADE_REPORT, rpt);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

TEST_F(ScheduleApiTest, OtherInstanceUserInputIntercepted) {
    emit_ext_inst(DZ_FRAME_UI_INPUT, "other_strategy");
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

// ── 持仓/资金推送: 不按策略过滤, 全量放行 (类型级; 完整帧 ingest 见 td_ingest_wiring_test) ──

struct TdUnfilteredFrameParam {
    DzFrameType type;
};

class TdUnfilteredFramesTest : public ScheduleApiTest,
                               public ::testing::WithParamInterface<TdUnfilteredFrameParam> {};

TEST_P(TdUnfilteredFramesTest, StillDelivered) {
    const auto& p = GetParam();
    DzShmPreload dummy{};
    emit_struct(p.type, dummy);
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr) << "frame type " << p.type << " should be delivered";
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), p.type);
}

INSTANTIATE_TEST_SUITE_P(TdUnfiltered,
                         TdUnfilteredFramesTest,
                         ::testing::Values(TdUnfilteredFrameParam{DZ_FRAME_POSITION_INFO},
                                           TdUnfilteredFrameParam{DZ_FRAME_TRADING_ACCOUNT}),
                         [](const ::testing::TestParamInfo<TdUnfilteredFrameParam>& info) {
                             return std::format("frame_{}", info.param.type);
                         });

// ── UI 域 TD 帧 (风控拒绝/出入金/改密): 非策略可见, SDK 直接丢弃 ──

TEST_F(ScheduleApiTest, UiDomainTdFramesIntercepted) {
    constexpr DzFrameType kIntercepted[] = {
        DZ_FRAME_TD_RISK_REJECT,  DZ_FRAME_TD_TRANSFER_REQ,        DZ_FRAME_TD_TRANSFER_RSP,
        DZ_FRAME_TD_TRANSFER_RTN, DZ_FRAME_TD_PASSWORD_UPDATE_REQ, DZ_FRAME_TD_PASSWORD_UPDATE_RSP};
    for (const DzFrameType type : kIntercepted) {
        DzShmPreload dummy{};
        emit_struct(type, dummy);
        EXPECT_EQ(dz_next_event(ctx_), nullptr)
            << "frame type " << type << " should be intercepted";
    }
}

// ── 契约 strategy《策略 SDK 事件拦截总则》逐项锚定 ──

namespace {

enum class FrameLayout {
    kBasic,    ///< 仅帧头 + struct payload
    kExt,      ///< 帧头 + 无 instance_id 扩展头
    kExtInst,  ///< 帧头 + 含 instance_id 扩展头
};

struct InterceptedFrameCase {
    const char* name;
    DzFrameType type;
    FrameLayout layout;
    const char* instance;     ///< kExtInst: 帧头 instance_id
    const char* strategy_id;  ///< kBasic 定向帧: payload strategy_id
};

}  // namespace

TEST_F(ScheduleApiTest, PlatformFramesInterceptedPerContract) {
    // 拦截总则 (契约 strategy §策略 SDK 事件拦截总则): 非本策略不可见的帧一律丢弃,
    // 逐项 emit 并断言 dz_next_event 为 nullptr。
    const InterceptedFrameCase cases[] = {
        {"NOTIFY_MD_STOPPED_own", DZ_FRAME_NOTIFY_MD_STOPPED, FrameLayout::kExtInst, "test_md",
         nullptr},
        {"NOTIFY_MD_STOPPED_other", DZ_FRAME_NOTIFY_MD_STOPPED, FrameLayout::kExtInst, "other_md",
         nullptr},
        {"ORDER_REPORT_other_strategy", DZ_FRAME_ORDER_REPORT, FrameLayout::kBasic, nullptr,
         "other_strategy"},
        {"ORDER_REPORT_empty_strategy", DZ_FRAME_ORDER_REPORT, FrameLayout::kBasic, nullptr, ""},
        {"TRADE_REPORT_other_strategy", DZ_FRAME_TRADE_REPORT, FrameLayout::kBasic, nullptr,
         "other_strategy"},
        {"TRADE_REPORT_empty_strategy", DZ_FRAME_TRADE_REPORT, FrameLayout::kBasic, nullptr, ""},
        {"UI_INPUT_other_strategy", DZ_FRAME_UI_INPUT, FrameLayout::kExtInst, "other_strategy",
         nullptr},
        {"SHUTDOWN_other_strategy", DZ_FRAME_SHUTDOWN, FrameLayout::kExtInst, "other_strategy",
         nullptr},
        {"OUTPUT_UI_echo_other_strategy", DZ_FRAME_OUTPUT_UI, FrameLayout::kExtInst,
         "other_strategy", nullptr},
        {"SET_LOGICAL_POSITION_echo_other_strategy", DZ_FRAME_SET_LOGICAL_POSITION,
         FrameLayout::kBasic, nullptr, "other_strategy"},
        // 其余平台帧: 日志/SHM 配置、进程控制、md 控制、TD 控制
        {"NOTIFY_UI", DZ_FRAME_NOTIFY_UI, FrameLayout::kExt, nullptr, nullptr},
        {"SET_LOG_CONFIG", DZ_FRAME_SET_LOG_CONFIG, FrameLayout::kBasic, nullptr, nullptr},
        {"RTN_PROCESS_STATUS", DZ_FRAME_RTN_PROCESS_STATUS, FrameLayout::kExtInst, "dzmaster",
         nullptr},
        {"REQUEST_MD_CONNECT", DZ_FRAME_REQUEST_MD_CONNECT, FrameLayout::kExtInst, "test_md",
         nullptr},
        {"NOTIFY_TD_STARTED", DZ_FRAME_NOTIFY_TD_STARTED, FrameLayout::kExtInst, "dztd_ctp",
         nullptr},
        {"TD_RTN_STATUS", DZ_FRAME_TD_RTN_STATUS, FrameLayout::kExtInst, "dztd_ctp", nullptr},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        switch (c.layout) {
            case FrameLayout::kExtInst:
                emit_ext_inst(c.type, c.instance);
                break;
            case FrameLayout::kExt:
                emit_ext(c.type);
                break;
            case FrameLayout::kBasic:
                if (c.type == DZ_FRAME_ORDER_REPORT) {
                    DzOrderReport rpt{};
                    dztrader::copy_string(rpt.strategy_id, c.strategy_id, true);
                    emit_struct(c.type, rpt);
                } else if (c.type == DZ_FRAME_TRADE_REPORT) {
                    DzTradeReport rpt{};
                    dztrader::copy_string(rpt.strategy_id, c.strategy_id, true);
                    emit_struct(c.type, rpt);
                } else if (c.type == DZ_FRAME_SET_LOGICAL_POSITION) {
                    DzLogicalPosition pos{};
                    dztrader::copy_string(pos.strategy_id, c.strategy_id, true);
                    emit_struct(c.type, pos);
                } else {
                    DzShmPreload dummy{};
                    emit_struct(c.type, dummy);
                }
                break;
        }
        EXPECT_EQ(dz_next_event(ctx_), nullptr)
            << "frame type " << c.type << " should be intercepted";
    }
}

// ── 截断帧防御: frame_size 不足 payload 的 TD 回报被拦截而非越界读 ──

TEST_F(ScheduleApiTest, TruncatedOrderReportIntercepted) {
    // 用 open_frame 手动构造 payload 过短的 ORDER_REPORT 帧 (frame_size < 完整 DzOrderReport)
    static_assert(sizeof(DzOrderReport) > 8);
    MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
    constexpr uint32_t kTruncatedPayload = 8;  // frame_size = 8 + header < sizeof(DzOrderReport)
    std::byte* raw = writer.open_frame(DZ_FRAME_ORDER_REPORT, kTruncatedPayload);
    ASSERT_NE(raw, nullptr);
    writer.close_frame();
    writer.notify_subscribers();

    EXPECT_EQ(dz_next_event(ctx_), nullptr);  // 截断帧被拦截, 不越界读 payload
}

// ── 他策略回报洪峰 + 32 上限让位: 本策略回报仍可取到 ──

TEST_F(ScheduleApiTest, OtherStrategyReportFloodYieldsThenOwnDelivered) {
    // 33 条他策略 ORDER_REPORT (被拦截) + 1 条本策略 ORDER_REPORT
    // 注: 各帧须带递增 seq (账户级 ingest 去重按 (account_id, seq), 同值会被判已应用拦截)
    for (int i = 0; i < 33; ++i) {
        DzOrderReport rpt{};
        rpt.seq = static_cast<uint64_t>(i + 1);
        dztrader::copy_string(rpt.strategy_id, "other_strategy", true);
        emit_struct(DZ_FRAME_ORDER_REPORT, rpt);
    }
    DzOrderReport own{};
    own.seq = 100;
    dztrader::copy_string(own.strategy_id, dz_strategy_id(ctx_), true);
    emit_struct(DZ_FRAME_ORDER_REPORT, own);

    // 首次调用: 最多消费 32 条被拦截帧后让位, 返回 NULL (定时器帧由 32 让位逻辑返回)
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
    // 第二次调用: 消费剩余被拦截帧后放行本策略回报
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    const auto view = FrameView(static_cast<const std::byte*>(frame));
    EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
    const auto& got = view.payload<DzOrderReport>();
    EXPECT_EQ(std::string_view(got.strategy_id), std::string_view(dz_strategy_id(ctx_)));
}

TEST_F(ScheduleApiTest, ShutdownFrameDirectedOwnInstanceDeliveredAfterCleanup) {
    // 制造 pending 内部预加载定时器
    DzShmPreload params{};
    emit_struct(DZ_FRAME_PRELOAD_EVENT_SHM, params);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);  // 内部消费, 随机延迟已排

    // 用户周期定时器: SHUTDOWN 内部清理不得影响
    const DzTimerId tid = dz_schedule_every(ctx_, 200);
    ASSERT_NE(tid, DZ_TIMER_INVALID);

    // 依次写: 非本策略定向 SHUTDOWN + 本策略定向 SHUTDOWN
    emit_ext_inst(DZ_FRAME_SHUTDOWN, "other_strategy");
    emit_ext_inst(DZ_FRAME_SHUTDOWN, dz_strategy_id(ctx_));

    // 一次 dz_next_event: other 被拦截, own 内部清理后放行
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_SHUTDOWN);
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
    constexpr int kCount = 200;  // > 32 槽, 同时覆盖堆在较大 n 下的顺序性
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
    while (fired.size() < static_cast<size_t>(kCount) &&
           std::chrono::steady_clock::now() < deadline) {
        dz_wait(ctx_);
        for (;;) {
            const void* frame = dz_next_event(ctx_);
            if (frame == nullptr) {
                break;
            }
            const auto view = FrameView(static_cast<const std::byte*>(frame));
            if (view.type() != DZ_FRAME_SCHEDULE) {
                continue;
            }
            const auto& ev = view.payload<DzScheduleEvent>();
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

// ── SDK 层帧边界防御 (valid_ext_inst_frame) ──

TEST_F(ScheduleApiTest, SdkRejectsMalformedUiInputFrames) {
    const std::string strategy_id = dz_strategy_id(ctx_);
    auto write_raw_ext = [&](uint32_t payload_bytes, const char* instance, uint32_t declared_size,
                             bool terminate) {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        std::byte* raw = writer.open_frame(DZ_FRAME_UI_INPUT, payload_bytes);
        ASSERT_NE(raw, nullptr);
        auto* ext = reinterpret_cast<DzExtInstFrameHeader*>(raw);
        if (terminate && instance != nullptr) {
            dztrader::copy_string(ext->instance_id, instance, true);
        } else {
            std::memset(ext->instance_id, 'x', sizeof(DzInstanceId));
        }
        ext->data_size = declared_size;
        writer.close_frame();
        writer.notify_subscribers();
    };

    // 1) 帧长不足 ext 头 (仅帧头)
    {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        std::byte* raw = writer.open_frame(DZ_FRAME_UI_INPUT, 0);
        ASSERT_NE(raw, nullptr);
        writer.close_frame();
        writer.notify_subscribers();
    }
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 2) data_size 越出帧尾
    write_raw_ext(static_cast<uint32_t>(sizeof(DzExtInstFrameHeader)), strategy_id.c_str(), 512,
                  true);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 3) instance_id 无 NUL 终止符
    write_raw_ext(static_cast<uint32_t>(sizeof(DzExtInstFrameHeader)), nullptr, 0, false);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 4) 合法帧仍正常送达
    const std::string payload = R"({"k":1})";
    {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, strategy_id.c_str(),
                                                reinterpret_cast<const std::byte*>(payload.data()),
                                                static_cast<uint32_t>(payload.size())));
        writer.notify_subscribers();
    }
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_UI_INPUT);
}

TEST_F(ScheduleApiTest, SdkRejectsMalformedShutdownFrame) {
    // 帧长不足 ext 头: SDK 丢弃, 不做内部清理, 会话仍可用
    {
        MultiWriter writer = MultiWriter::create(open_event_meta(), "schedule_test_writer");
        std::byte* raw = writer.open_frame(DZ_FRAME_SHUTDOWN, 0);
        ASSERT_NE(raw, nullptr);
        writer.close_frame();
        writer.notify_subscribers();
    }
    EXPECT_EQ(dz_next_event(ctx_), nullptr);

    // 合法 SHUTDOWN 仍送达 (证明上一条不是把通道/游标弄坏)
    emit_ext_inst(DZ_FRAME_SHUTDOWN, dz_strategy_id(ctx_));
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_SHUTDOWN);
}

TEST_F(ScheduleApiTest, ShutdownClearsDeferredTimerFrames) {
    // 32 槽 + 1 deferred: 制造 deferred 队列非空的现场, SHUTDOWN 必须一并清掉
    constexpr int kBufferSlots = 32;
    constexpr int kCount = kBufferSlots + 1;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_NE(dz_schedule_after(ctx_, 20), DZ_TIMER_INVALID);
    }
    // 等到全部定时器到期 (dz_wait 可能被残留计数提前唤醒, 循环至截止点);
    // +5ms 余量覆盖 33 次调度调用耗时, 保证首次 tick 全部到期进缓冲
    wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(25));

    int delivered = 0;
    while (delivered < kBufferSlots) {
        const void* frame = dz_next_event(ctx_);
        ASSERT_NE(frame, nullptr) << "delivered=" << delivered;
        EXPECT_EQ(FrameView(static_cast<const std::byte*>(frame)).type(), DZ_FRAME_SCHEDULE);
        ++delivered;
    }
    // 第 33 个触发此刻留在 deferred 队列; SHUTDOWN 清理后不得再投递
    emit_ext_inst(DZ_FRAME_SHUTDOWN, dz_strategy_id(ctx_));
    const void* shutdown = dz_next_event(ctx_);
    ASSERT_NE(shutdown, nullptr);
    EXPECT_EQ(FrameView(static_cast<const std::byte*>(shutdown)).type(), DZ_FRAME_SHUTDOWN);
    EXPECT_EQ(dz_next_event(ctx_), nullptr);
}

}  // namespace
