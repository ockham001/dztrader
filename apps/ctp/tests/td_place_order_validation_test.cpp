/**
 * @file td_place_order_validation_test.cpp
 * @brief 出账前枚举校验与「未知枚举不猜值」的**会话级**行为（issue #367 §三/§五）。
 *
 * 为什么需要这个目标（与 `td_ctp_mapping_test` / `td_position_lifecycle_test` 的分工）：
 * - `td_ctp_mapping_test` 只测纯函数：能证明「不支持的枚举返回失败」，但证明不了
 *   `place_order` **真的**会拒单、以及拒单后**没有**任何一张「已提交」的委托被写进 SHM
 *   或落库。那两句断言必须在真会话上做，否则就是 `docs/unit-test-standard.md` §1.1
 *   判定的失效测试（在替身上断言恒真）。
 * - `td_position_lifecycle_test` 覆盖持仓快照生命周期；本文件覆盖委托准入与未知回报，
 *   两者共用同一套「真 SQLite + 真 `PersistWriter` + 真 `AccountSession`」基建。
 *
 * 边界（诚实声明）：本目标不覆盖登录链（`open()` 需要连柜台）。因此会话停在 `Idle`，
 * 校验必然先于就绪检查命中——这恰好是本票要钉的顺序（永久缺陷优先于暂态未就绪），
 * 「已知枚举穿过校验」的用例据「是否变成未就绪拒单」间接证明。
 *
 * SHM 帧用真 `Reader` 读同一条通道：断言的是「写进去了什么」，不是「某函数被调过」。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/timer_queue.h>
#include <dztrader/date_time/date.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/tdstore/trade_db.h>

#include "common/ctp_events.h"
#include "td/td_account_session.h"
#include "td/td_events.h"
#include "td/td_position.h"
#include "td/td_persist_writer.h"
#include "test_temp_dir.h"
#include "test_util.h"

namespace dztrader::ctp {
namespace {

class TdPlaceOrderValidationTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    std::unique_ptr<shm::NamedSemaphore> sem_;
    /// 帧读取器：`Reader::create` 从**创建时刻**的写位置开始读，所以必须在
    /// `place_order` 之前建好（否则「没有帧被写入」恒真，断言退化成空转）。
    std::optional<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    std::unique_ptr<shm::OrderIdMeta> order_id_meta_;
    std::unique_ptr<PersistWriter> persist_;
    dztrader::core::TimerQueue timer_queue_;
    MpmcQueuePtr queue_;
    std::unique_ptr<AccountSession> session_;

    /// notify_ui 收到的通知（校验 level 与 popup）。
    struct Notification {
        DzNotifyLevel level = DZ_NOTIFY_INFO;
        std::string message;
        bool popup = true;
    };
    std::vector<Notification> notifications_;

    static constexpr int32_t kDay = 20735;  ///< 20261009

    void SetUp() override {
        ASSERT_EQ(kDay, day(2026, 10, 9)) << "kDay constant drifted from 2026-10-09";
        dir_ = dztrader::test::unique_temp_dir("dz_td_place_order_validation_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
        (void)dztrader::tdstore::ensure_trade_db(db_path_);

        channel_name_ = dztrader::shm::test::unique_channel_name("dz_td_order_validation");
        shm_dir_ = dir_ / "shm";
        std::filesystem::create_directories(shm_dir_);
        const shm::ChannelConfig cfg{.channel_name = channel_name_,
                                     .shm_dir = shm_dir_,
                                     .meta_file_size = 4 * 1024 * 1024,
                                     .page_size = 1 * 1024 * 1024,
                                     .lock_memory = false,
                                     .prefetch_memory = false};
        auto meta = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer_ = std::make_unique<shm::MultiWriter>(shm::MultiWriter::create(meta, "td_val_test"));
        order_id_meta_ = std::make_unique<shm::OrderIdMeta>(
            shm::OrderIdMeta::open_or_create("dz_order_id_val_test", shm_dir_));
        sem_ = std::make_unique<shm::NamedSemaphore>(channel_name_ + "_sem");
        queue_ = std::make_shared<MpmcQueue>(sem_.get(), 1024);

        persist_ = std::make_unique<PersistWriter>(db_path_, "dztd_test");
        persist_->open();
        persist_->start_writer();

        session_ = std::make_unique<AccountSession>(
            "acc1", *order_id_meta_, *writer_, *persist_, queue_, timer_queue_,
            /*account_state_cb=*/nullptr, /*seq_allocator=*/nullptr,
            [this](DzNotifyLevel level, const std::string& message, bool popup) {
                notifications_.push_back(Notification{level, message, popup});
            });
        session_->set_trading_day(kDay);
        // Reader 从创建时刻的写位置起读, 必须在第一条帧产生之前建好.
        reader_.emplace(shm::Reader::create(channel_name_, shm_dir_, "val_reader"));
    }

    void TearDown() override {
        reader_.reset();
        session_.reset();
        if (persist_) {
            persist_->stop();
        }
        persist_.reset();
        queue_.reset();
        sem_.reset();
        order_id_meta_.reset();
        writer_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        dztrader::shm::test::cleanup_channel(channel_name_, shm_dir_);
    }

    static int32_t day(int y, int m, int d) {
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    }

    int64_t scalar_int(const std::string& sql) {
        SQLite::Database db(db_path_, SQLite::OPEN_READONLY);
        db.exec("PRAGMA busy_timeout=5000");
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
        return 0;
    }

    /// 等 persist 队列排空且末批已提交（确定性同步点，不用 sleep）。
    void flush_persist() {
        const uint64_t token = persist_->enqueue_flush_signal();
        ASSERT_TRUE(persist_->wait_flush(token, std::chrono::seconds(5)));
    }

    /// 读出自 reader_ 创建以来的全部帧（Reader 只前进不回头）。
    std::vector<std::unique_ptr<std::byte[]>> read_frames() {
        std::vector<std::unique_ptr<std::byte[]>> frames;
        while (const std::byte* raw = reader_->next_frame()) {
            const auto* hdr = reinterpret_cast<const DzFrameHeader*>(raw);
            auto copy = std::make_unique<std::byte[]>(hdr->frame_size);
            std::memcpy(copy.get(), raw, hdr->frame_size);
            frames.push_back(std::move(copy));
        }
        return frames;
    }

    /// 一条委托请求（缺省 = 一条完全合法的限价开仓多单）。
    static DzOrderReq make_req(DzPriceType price_type = DZ_PRICE_LIMIT,
                               DzDirection direction = DZ_DIRECTION_LONG,
                               DzPositionEffect effect = DZ_POSITION_EFFECT_OPEN,
                               DzOrderId order_id = 42) {
        DzOrderReq r{};
        r.order_id = order_id;
        std::strcpy(r.strategy_id, "strat1");
        std::strcpy(r.account_id, "acc1");
        std::strcpy(r.instrument_id, "IF2506");
        r.price = 3900.0;
        r.volume = 1;
        r.direction = direction;
        r.price_type = price_type;
        r.position_effect = effect;
        return r;
    }

    /// 一条持仓查询响应行（`volume` 含昨仓；`cost` 为持仓成本金额）。
    static OnRspQryInvestorPositionField position_row(const std::string& instrument,
                                                      int32_t volume,
                                                      int32_t yd,
                                                      double cost,
                                                      char posi_direction,
                                                      bool is_last) {
        OnRspQryInvestorPositionField f{};
        CThostFtdcInvestorPositionField p{};
        std::strcpy(p.InstrumentID, instrument.c_str());
        std::strcpy(p.ExchangeID, "CFFEX");
        std::strcpy(p.InvestorID, "acc1");
        p.Position = volume;
        p.YdPosition = yd;
        p.PositionCost = cost;
        p.PosiDirection = posi_direction;
        f.investor_position = p;
        f.is_last = is_last;
        // 未登录时会话的 position_query_request_id_ 为 0；不设 0 的响应会被整条丢弃，
        // 断言就退化成恒真。
        f.request_id = 0;
        f.account_id = "acc1";
        return f;
    }
};

// ============================================================================
// AC(#367 §三): 不支持的枚举 -> 本地拒单，且没有「已提交」的委托出去
// ============================================================================

TEST_F(TdPlaceOrderValidationTest, UnsupportedPriceTypeIsRejectedLocally) {
    for (DzPriceType pt :
         {DZ_PRICE_STOP, DZ_PRICE_RFQ, DZ_PRICE_UNKNOWN, static_cast<DzPriceType>(99)}) {
        notifications_.clear();
        session_->place_order(make_req(pt));
        flush_persist();

        const auto frames = read_frames();
        ASSERT_FALSE(frames.empty()) << "rejection must surface as a report frame";
        for (const auto& frame : frames) {
            const shm::FrameView view(frame.get());
            // 只允许出现「拒单回报」；出现 TD_RISK_REJECT 说明校验被排到了风控之后，
            // 出现非 REJECTED 的 ORDER_REPORT 说明有单被当成正常委托写了出去。
            EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
            const auto& rpt = view.payload<DzOrderReport>();
            EXPECT_EQ(rpt.status, DZ_ORDER_REJECTED);
            EXPECT_EQ(rpt.order_id, 42);
        }
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
        EXPECT_EQ(scalar_int("SELECT status FROM orders"), DZ_ORDER_REJECTED)
            << "an order with an unsupported price type must never be persisted as a live order";
        // 本地拒单的 error_id=-1 落在 ext JSON（ADR 0041 §1：柜台差异不进列）。
        EXPECT_EQ(scalar_int("SELECT json_extract(ext, '$.error_id') FROM orders"), -1)
            << "local rejection is distinguishable from counter rejection";
    }
}

TEST_F(TdPlaceOrderValidationTest, UnsupportedPositionEffectIsRejectedLocally) {
    // 显式常量而不是 `100 + pe + 128`: 依赖 UNKNOWN == -128 这个实现细节, 哨兵值一改
    // order_id 就会变负/撞键, 失败现场与真实原因无关 (issue #367 审查 F5).
    const std::vector<std::pair<DzPositionEffect, DzOrderId>> cases = {
        {DZ_POSITION_EFFECT_UNKNOWN, 100},
        {DZ_POSITION_EFFECT_AUTO, 101},
        {static_cast<DzPositionEffect>(99), 102},
    };
    for (const auto& [pe, order_id] : cases) {
        session_->place_order(make_req(DZ_PRICE_LIMIT, DZ_DIRECTION_LONG, pe, order_id));
        flush_persist();
    }

    const auto frames = read_frames();
    ASSERT_FALSE(frames.empty());
    for (const auto& frame : frames) {
        const shm::FrameView view(frame.get());
        EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
        EXPECT_EQ(view.payload<DzOrderReport>().status, DZ_ORDER_REJECTED);
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 3);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE status <> " +
                         std::to_string(DZ_ORDER_REJECTED)),
              0);
}

TEST_F(TdPlaceOrderValidationTest, UnknownDirectionIsRejectedLocally) {
    // UNKNOWN = 柜台数据坏了: 方向读不懂的单绝不能发往交易所.
    // 显式常量理由同 UnsupportedPositionEffectIsRejectedLocally.
    session_->place_order(make_req(DZ_PRICE_LIMIT, DZ_DIRECTION_UNKNOWN, DZ_POSITION_EFFECT_OPEN,
                                   /*order_id=*/201));
    session_->place_order(make_req(DZ_PRICE_LIMIT, static_cast<DzDirection>(99),
                                   DZ_POSITION_EFFECT_OPEN, /*order_id=*/202));
    flush_persist();

    const auto frames = read_frames();
    ASSERT_FALSE(frames.empty());
    for (const auto& frame : frames) {
        const shm::FrameView view(frame.get());
        EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
        EXPECT_EQ(view.payload<DzOrderReport>().status, DZ_ORDER_REJECTED);
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 2);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE status <> " +
                         std::to_string(DZ_ORDER_REJECTED)),
              0);
}

TEST_F(TdPlaceOrderValidationTest, NetDirectionIsRejectedLocally) {
    // NET 不是「数据坏了」, 而是**语义上不是报单方向**: 内部持仓侧标签 (td_position.h 的
    // ActiveOrderUpdate.direction 默认值) 被误当成报单方向传进来时同样必须本地拒单,
    // 但理由与 UNKNOWN 不同, 故与上面的用例分开 (issue #367 审查 F5).
    session_->place_order(make_req(DZ_PRICE_LIMIT, DZ_DIRECTION_NET, DZ_POSITION_EFFECT_OPEN,
                                   /*order_id=*/203));
    flush_persist();

    const auto frames = read_frames();
    ASSERT_FALSE(frames.empty());
    for (const auto& frame : frames) {
        const shm::FrameView view(frame.get());
        EXPECT_EQ(view.type(), DZ_FRAME_ORDER_REPORT);
        EXPECT_EQ(view.payload<DzOrderReport>().status, DZ_ORDER_REJECTED);
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE remark LIKE '不支持的方向%'"), 1)
        << "net direction must be rejected by the direction gate, not by readiness";
}

TEST_F(TdPlaceOrderValidationTest, ValidEnumsPassTheEnumerationGate) {
    // 合法的价格类型/开平/方向必须穿过枚举校验，落到下一条防线（此处 = 未就绪拒单）。
    // 若枚举校验把合法值也拒了，这里拿到的就会是「不支持的价格类型」而不是「未就绪」。
    const std::vector<std::pair<DzPriceType, DzPositionEffect>> combos = {
        {DZ_PRICE_LIMIT, DZ_POSITION_EFFECT_OPEN},
        {DZ_PRICE_MARKET, DZ_POSITION_EFFECT_CLOSE},
        {DZ_PRICE_FAK, DZ_POSITION_EFFECT_CLOSE_TODAY},
        {DZ_PRICE_FOK, DZ_POSITION_EFFECT_CLOSE_YESTDAY},
    };
    for (size_t i = 0; i < combos.size(); ++i) {
        session_->place_order(make_req(combos[i].first, DZ_DIRECTION_SHORT, combos[i].second,
                                       /*order_id=*/300 + static_cast<DzOrderId>(i)));
    }
    flush_persist();

    SQLite::Database db(db_path_, SQLite::OPEN_READONLY);
    db.exec("PRAGMA busy_timeout=5000");
    SQLite::Statement q(db, "SELECT COUNT(*) FROM orders WHERE remark LIKE '交易未就绪%'");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt64(), static_cast<int64_t>(combos.size()))
        << "known-good enum combinations must reach the readiness check untouched";
}

// ============================================================================
// AC(#367 §一/§二): 未知回报不得写成真实值，且不上报成「委托被拒」
// ============================================================================

TEST_F(TdPlaceOrderValidationTest, UnknownPositionDirectionKeepsLastKnownSnapshot) {
    // 先跑一轮**正常**查询：既建立「上次已知快照」（多头 2
    // 手），又把成功标志立起来，后面「未知方向让本轮失败」才可证伪 （会话未登录时
    // position_query_ok_ 构造即 false，直接断言它为 false 恒真）。
    session_->on_rsp_qry_investor_position(
        position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, THOST_FTDC_PD_Long, /*is_last=*/true));
    flush_persist();
    EXPECT_TRUE(session_->position_query_ok()) << "a fully readable snapshot must count as success";

    // 一轮里同时有已知多头和方向未知的行：整轮快照不可信 -> 不应用。
    session_->on_rsp_qry_investor_position(
        position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, THOST_FTDC_PD_Long, /*is_last=*/false));
    session_->on_rsp_qry_investor_position(
        position_row("rb2510", 5, 5, 5.0 * 10.0 * 3500.0, 'Z', /*is_last=*/false));
    session_->on_rsp_qry_investor_position(
        position_row("", 0, 0, 0.0, THOST_FTDC_PD_Long, /*is_last=*/true));
    flush_persist();

    // 关键断言：未知方向既不能当多头写进来，也不能因为「这一行被丢掉」而让整轮查询看起来
    // 像该合约已平仓——那会把已知持仓清零成幽灵空仓（ADR 0045 决策 3）。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions"), 1);
    EXPECT_EQ(scalar_int("SELECT direction FROM positions"), DZ_DIRECTION_LONG);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='IF2506'"), 2)
        << "an unparseable row must not turn a known position into a phantom flat one";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='rb2510'"), 0);
    EXPECT_FALSE(session_->position_query_ok())
        << "an unreadable snapshot counts as a failed query so the resync chain retries";

    // 计数按种类独立累加，且 notify 不弹窗（持续性异常不打断用户操作）。
    EXPECT_EQ(session_->unknown_enum_counts().direction, 1u);
    EXPECT_EQ(session_->unknown_enum_counts().total(), 1u);
    bool saw_notify = false;
    for (const auto& n : notifications_) {
        if (n.level == DZ_NOTIFY_ERROR && !n.popup) {
            saw_notify = true;
        }
    }
    EXPECT_TRUE(saw_notify) << "unknown enum must raise a non-popup error notification";

    // 限频（issue #367 审查 F11）：未知枚举可能每条回报都带，同一间隔内只通知一次、
    // 只触发一次重查；计数仍然逐条累加（否则运维看不到洪水规模）。
    const size_t notify_count = notifications_.size();
    session_->on_rsp_qry_investor_position(
        position_row("rb2510", 5, 5, 5.0 * 10.0 * 3500.0, 'Z', /*is_last=*/true));
    flush_persist();
    EXPECT_EQ(notifications_.size(), notify_count)
        << "a second unknown enum within the throttle window must only bump the counter";
    EXPECT_EQ(session_->unknown_enum_counts().direction, 2u);
}

TEST_F(TdPlaceOrderValidationTest, UnknownOrderStatusKeepsOrderActiveAndFrozen) {
    // CTP 回报里出现未知状态：落成 DZ_ORDER_UNKNOWN 并计数，绝不改成 REJECTED/CANCELLED
    // （那会谎称「这笔单没成立」，而它可能正挂在交易所）；且不得被当作终态而释放冻结。
    // 先建好持仓基准（多头 2 手，今 1）：一张平多 1 手的挂单会冻结 long 1 手。
    session_->on_rsp_qry_investor_position(
        position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, THOST_FTDC_PD_Long, /*is_last=*/true));
    flush_persist();
    ASSERT_EQ(scalar_int("SELECT frozen_volume FROM positions WHERE instrument_id='IF2506'"), 0);

    OnRtnOrderField f{};
    std::strcpy(f.order.BrokerID, "9999");
    std::strcpy(f.order.InvestorID, "acc1");
    std::strcpy(f.order.InstrumentID, "IF2506");
    std::strcpy(f.order.OrderRef, "000000000001");
    std::strcpy(f.order.ExchangeID, "CFFEX");
    f.order.Direction = THOST_FTDC_D_Sell;  // 卖平多
    f.order.CombOffsetFlag[0] = THOST_FTDC_OF_Close;
    f.order.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    f.order.OrderStatus = 'Z';  // 不认识的柜台状态
    f.order.LimitPrice = 3900.0;
    f.order.VolumeTotalOriginal = 1;
    std::strcpy(f.order.InsertTime, "09:30:45");

    session_->on_rtn_order(f);
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_int("SELECT status FROM orders"), DZ_ORDER_UNKNOWN);
    EXPECT_EQ(session_->unknown_enum_counts().order_status, 1u);
    // 未知状态不是终态：该结论是「冻结不释放」的判据，两侧都断言才算钉住。
    // 这句只钉**常量语义**——它读一个纯谓词，不会因为会话行为写错而红（只有人改
    // `is_terminal_order_status` 才会）。真正的可证伪判据是紧随其后的 `frozen_volume == 1`：
    // 谓词若把 UNKNOWN 算成终态，那条断言会回 0。保留本句的理由是补上谓词对 UNKNOWN 的
    // 直接覆盖（`td_position_test.cpp` 的 TerminalStatusTest.Classify 没列 UNKNOWN），
    // 两条一起看才既钉谓词又钉行为。
    EXPECT_FALSE(is_terminal_order_status(DZ_ORDER_UNKNOWN))
        << "unknown status must never be terminal, otherwise the frozen position is released";
    // 活动委托仍在 -> 平多 1 手仍冻结 long（若被当终态处理，这里会回 0 = 凭空释放冻结）。
    EXPECT_EQ(scalar_int("SELECT frozen_volume FROM positions WHERE instrument_id='IF2506'"), 1)
        << "an active close order with unreadable status must keep its frozen volume";
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='IF2506'"), 2);
}

}  // namespace
}  // namespace dztrader::ctp
