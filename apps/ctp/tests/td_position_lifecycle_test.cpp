/**
 * @file td_position_lifecycle_test.cpp
 * @brief 持仓快照生命周期的**会话级**行为（契约 td-data-sync §3/§7、ADR 0045/ADR 0046）。
 *
 * 为什么需要这个目标（与 `td_persist_writer_test` / `td_data_sync_integration_test` 的分工）：
 * 那两个目标只链 `PersistWriter` / `ReportFilter` / `DayLoad`，**没有** `AccountSession`；
 * 而本票 AC 的核心（查询失败不改表、完整成功查询整组替换、重复成功查询无差异不重建、
 * 日切只碰当日行）的决定点在
 * `AccountSession::on_rsp_qry_investor_position → apply_position_query` 里。在只有 Writer
 * 的目标里只能写一个「镜像生产顺序」的替身，断言会恒真——属
 * `docs/unit-test-standard.md` §1.1/§1.2 判定的失效测试形态。
 *
 * 因此本文件直连生产会话：真 SQLite 临时库 + 真 `PersistWriter` 线程 + 真 `AccountSession`，
 * 把 TdApi 从事件队列拿到的 `OnRspQryInvestorPositionField` **直接**喂给会话的公开 SPI 入口。
 *
 * **边界（诚实声明）**：本目标不覆盖「登录链本身」（合约加载 → 收尾 → Ready）——那条链在
 * `req_settlement_confirm` 里解引用 `api_`，而 `api_` 只能由 `open()` 创建真实
 * `CThostFtdcTraderApi`（需连柜台）。因此这里的会话处于**未登录**状态，只驱动持仓查询
 * 响应分支——那正是本票所有持仓写入决策所在。登录链的 flush/Ready 语义由
 * `td_login_finalize_test`（纯逻辑）与 `td_data_sync_integration_test`（Writer 时序）覆盖。
 * 基准查询的**有限重试预算/停住/ERROR 通知**是本文件覆盖的例外：重试定时器只排定不触发
 * （无柜台的测试不能真发请求），但预算判定、定时器清理、通知与「失败链不碰表」都走真实代码。
 *
 * **#488 登录链后段失败**：结算确认与合约查询的发送都经注入钩子
 * （`set_settlement_send_hook` / `set_instrument_send_hook`，空 = 走 `api_`）绕开柜台，
 * 因此本目标覆盖两个失败出口的**全部调用点**：结算确认响应被拒 / 发送被拒 / 看门狗超时 /
 * 迟到响应丢弃，以及合约查询发送被拒 / 流控不误报 / `on_rsp_error` 错误响应 / 加载 is_last
 * 失败。状态摆位用 `advance_login_chain_for_test`（重放生产迁移，失败当场抛），超时收口
 * 两路都验：`expire_settlement_confirm_timeout_for_test`（注入收口）与缩时后真
 * `TimerQueue::tick()`（执行排定的回调本身）。不 sleep。
 *
 * **#458 运行期资金刷新**：生产上由登录收尾链进入 Ready，本目标用
 * `AccountSession::enter_ready_for_test` 注入同一状态迁移（纯状态机调用，不触网/不落库），
 * 并注入确定时钟 + 发送钩子（`set_query_clock` / `set_query_send_hook`）驱动按需查询；
 * TimerQueue 的真实 tick 只在「0 延迟即可发送」用例里使用，其余用例直接调
 * `pump_query_lane` / `expire_runtime_query_timeout_for_test` 注入确定时序，不 sleep。
 *
 * 确定性来源：每次断言前用 persist flush 屏障等末批提交（`wait_flush`），不用 sleep。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/timer_queue.h>
#include <dztrader/date_time/date.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/tdstore/trade_db.h>
#include <dztrader/struct.h>

#include "common/ctp_events.h"
#include "td/td_account_session.h"
#include "td/td_events.h"
#include "td/td_persist_writer.h"
#include "shm_frame_reader.h"
#include "test_temp_dir.h"
#include "test_util.h"

namespace dztrader::ctp {
namespace {

class TdPositionLifecycleTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;
    std::string channel_name_;
    std::filesystem::path shm_dir_;

    std::unique_ptr<shm::NamedSemaphore> sem_;
    /// 帧读取器：Reader::create 从**创建时刻**的写位置开始读，必须在首个用例动作之前建好，
    /// 否则「组合持仓行不推帧」的断言会退化成空转（读到 0 帧恒真）。
    std::optional<shm::Reader> reader_;
    std::unique_ptr<shm::MultiWriter> writer_;
    std::unique_ptr<shm::OrderIdMeta> order_id_meta_;
    std::unique_ptr<PersistWriter> persist_;
    dztrader::core::TimerQueue timer_queue_;
    MpmcQueuePtr queue_;
    std::unique_ptr<AccountSession> session_;

    /// notify_ 收到的通知 (基准查询失败弹窗的唯一出口; 校验 level / popup / 文案).
    struct Notification {
        DzNotifyLevel level = DZ_NOTIFY_INFO;
        std::string message;
        bool popup = false;
    };
    std::vector<Notification> notifications_;

    /// 运行期资金刷新夹具（ADR 0055 / #458）：确定性时钟 + 发送钩子，不连柜台不 sleep。
    QueryPacer::TimePoint query_fake_now_{};
    std::vector<int> sent_fund_query_ids_;
    int send_fund_ret_ = 0;  ///< 发送钩子返回码（0=成功；-3 等构造失败路径）

    /// 构造会话 (notify 转发到 notifications_; 各用例的重连/新会话也走这里).
    std::unique_ptr<AccountSession> make_session() {
        return std::make_unique<AccountSession>(
            "acc1", *order_id_meta_, *writer_, *persist_, queue_, timer_queue_,
            /*account_state_cb=*/nullptr, /*seq_allocator=*/nullptr,
            [this](DzNotifyLevel level, const std::string& message, bool popup) {
                notifications_.push_back(Notification{level, message, popup});
            });
    }

    static constexpr int32_t kDay = 20735;  ///< 20261009（下面 SetUp 用真实日期派生校验）
    /// kDay 的 CTP 回报形态 (委托/成交回报的 TradingDay 字段是 8 位 "YYYYMMDD")。
    /// #423 起回报必须能自证等于会话交易日, 否则会被判死并请求登出 —— 正常回报用例
    /// 必须带对这一天, 否则断言不再是"持仓增量"而是"跨日拒绝"。
    static constexpr const char* kTradingDayText = "20261009";

    void SetUp() override {
        ASSERT_EQ(kDay, day(2026, 10, 9)) << "kDay constant drifted from 2026-10-09";
        ASSERT_EQ(dztrader::Date{kDay}.to_string("%Y%m%d"), std::string(kTradingDayText))
            << "kTradingDayText drifted from kDay";
        dir_ = dztrader::test::unique_temp_dir("dz_td_position_lifecycle_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
        (void)dztrader::tdstore::ensure_trade_db(db_path_);

        channel_name_ = dztrader::shm::test::unique_channel_name("dz_td_pos_lifecycle");
        shm_dir_ = dir_ / "shm";
        std::filesystem::create_directories(shm_dir_);
        const shm::ChannelConfig cfg{.channel_name = channel_name_,
                                     .shm_dir = shm_dir_,
                                     .meta_file_size = 4 * 1024 * 1024,
                                     .page_size = 1 * 1024 * 1024,
                                     .lock_memory = false,
                                     .prefetch_memory = false};
        auto meta = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer_ = std::make_unique<shm::MultiWriter>(shm::MultiWriter::create(meta, "td_pos_test"));
        order_id_meta_ = std::make_unique<shm::OrderIdMeta>(
            shm::OrderIdMeta::open_or_create("dz_order_id_pos_test", shm_dir_));
        sem_ = std::make_unique<shm::NamedSemaphore>(channel_name_ + "_sem");
        queue_ = std::make_shared<MpmcQueue>(sem_.get(), 1024);

        persist_ = std::make_unique<PersistWriter>(db_path_, "dztd_test");
        persist_->open();
        persist_->start_writer();

        session_ = make_session();
        // 交易日固定，避免断言随真实当天漂移（会话用它填 DzPositionInfo.date）。
        session_->set_trading_day(kDay);
        // 帧读取器必须在任何写帧动作之前创建（见成员注释）。
        reader_.emplace(shm::Reader::create(channel_name_, shm_dir_, "lifecycle_reader"));
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

    std::string scalar_text(const std::string& sql) {
        SQLite::Database db(db_path_, SQLite::OPEN_READONLY);
        db.exec("PRAGMA busy_timeout=5000");
        SQLite::Statement q(db, sql);
        if (q.executeStep()) {
            return q.getColumn(0).getString();
        }
        return {};
    }

    void raw_exec(const std::string& sql) {
        SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
        db.exec("PRAGMA busy_timeout=5000");
        db.exec(sql);
    }

    /// 等 persist 队列排空且末批已提交（确定性同步点，不用 sleep）。
    void flush_persist() {
        const uint64_t token = persist_->enqueue_flush_signal();
        ASSERT_TRUE(persist_->wait_flush(token, std::chrono::seconds(5)));
    }

    /// 一条持仓查询响应行（`volume` 为总持仓、`yd` 为**当前**昨仓；`cost` 为持仓成本金额）。
    /// CTP 字段按 ADR 0049 口径填：`TodayPosition` = 当前今仓 = volume - yd，
    /// `YdPosition` = 交易日开始时静态昨仓（默认取 `yd`，即当日未平昨；`yd_static` 显式给
    /// 已平昨的场景）。`posi_direction` 缺省 = CTP 多('2')：本 fixture 全仓按多头语义断言，
    /// 必须显式给（未知方向现在落成 UNKNOWN 且整行被丢弃，不再兜成多头）。
    static OnRspQryInvestorPositionField position_row(const std::string& instrument,
                                                      int32_t volume,
                                                      int32_t yd,
                                                      double cost,
                                                      bool is_last,
                                                      char posi_direction = THOST_FTDC_PD_Long,
                                                      const char* exchange_id = "CFFEX",
                                                      int32_t yd_static = -1) {
        OnRspQryInvestorPositionField f{};
        CThostFtdcInvestorPositionField p{};
        std::strcpy(p.InstrumentID, instrument.c_str());
        std::strcpy(p.ExchangeID, exchange_id);
        std::strcpy(p.InvestorID, "acc1");
        p.Position = volume;
        p.TodayPosition = volume - yd;
        p.YdPosition = yd_static >= 0 ? yd_static : yd;
        p.PositionCost = cost;
        p.PosiDirection = posi_direction;
        f.investor_position = p;
        f.is_last = is_last;
        // 会话用 `request_id != position_query_request_id_` 丢弃迟到响应。未登录时
        // `position_query_request_id_` 保持初值 0，故测试响应统一用 0——**不设成 0 的
        // 响应会被整条丢弃，断言就退化成恒真**（本文件第一版踩过）。
        f.request_id = 0;
        f.account_id = "acc1";
        return f;
    }

    /// 一条**失败**的持仓查询 is_last（带 ErrorID）。
    static OnRspQryInvestorPositionField failed_position_query() {
        OnRspQryInvestorPositionField f{};
        CThostFtdcRspInfoField info{};
        info.ErrorID = 31;
        std::strcpy(info.ErrorMsg, "query failed");
        f.rsp_info = info;
        f.is_last = true;
        f.request_id = 0;  // 同 position_row：不匹配就被丢弃（恒真陷阱）
        f.account_id = "acc1";
        return f;
    }

    /// 一条**成功**的资金查询响应（`is_last` 收口；金额可指定，默认值 = 常见形态）。
    static OnRspQryTradingAccountField trading_account_row(bool is_last,
                                                           double balance = 100000.0,
                                                           double available = 85000.0,
                                                           int request_id = 0) {
        OnRspQryTradingAccountField f{};
        CThostFtdcTradingAccountField a{};
        std::strcpy(a.AccountID, "acc1");
        a.Balance = balance;
        a.Available = available;
        a.CurrMargin = 15000.0;
        f.trading_account = a;
        f.is_last = is_last;
        f.request_id = request_id;
        f.account_id = "acc1";
        return f;
    }

    /// 一轮「成功」的持仓查询：先逐行，再 is_last 收口（生产响应的真实形态）。
    void feed_position_snapshot(const std::vector<OnRspQryInvestorPositionField>& rows) {
        for (size_t i = 0; i < rows.size(); ++i) {
            session_->on_rsp_qry_investor_position(rows[i]);
        }
        // 空快照 = 只有 is_last（全平后的查询形态）。
        if (rows.empty()) {
            session_->on_rsp_qry_investor_position(position_row("", 0, 0, 0.0, /*is_last=*/true));
        }
        flush_persist();
    }

    /// 一条合约查询响应行（`product_class` 决定套利/组合判定；缺省 = 普通期货）。
    /// 未登录会话里 `on_rsp_qry_instrument` 的数据行也会写进合约映射表（is_last 才要求
    /// LoadingInstruments 状态），用它给会话注入 product_class。
    static OnRspQryInstrumentField instrument_row(const std::string& instrument,
                                                  const char* exchange_id,
                                                  char product_class = THOST_FTDC_PC_Futures,
                                                  int request_id = 0) {
        OnRspQryInstrumentField f{};
        CThostFtdcInstrumentField inst{};
        std::strcpy(inst.InstrumentID, instrument.c_str());
        std::strcpy(inst.ExchangeID, exchange_id);
        std::strcpy(inst.InstrumentName, instrument.c_str());
        std::strcpy(inst.ProductID, instrument.c_str());
        inst.ProductClass = product_class;
        inst.VolumeMultiple = 300;
        inst.PriceTick = 0.2;
        f.instrument = inst;
        f.is_last = false;
        f.request_id = request_id;
        f.account_id = "acc1";
        return f;
    }

    /// 登记一条合约（会话据 product_class 判定套利/组合合约）。
    void register_instrument(const std::string& instrument,
                             const char* exchange_id,
                             char product_class = THOST_FTDC_PC_Futures) {
        session_->on_rsp_qry_instrument(instrument_row(instrument, exchange_id, product_class));
        flush_persist();
    }

    /// 一条成交回报（缺省 = 开仓买; 套利/单腿用例共用）。
    static OnRtnTradeField trade_row(const std::string& instrument,
                                     const char* trade_id,
                                     int32_t volume = 1,
                                     double price = 3900.0,
                                     const char* exchange_id = "CFFEX",
                                     char direction = THOST_FTDC_D_Buy) {
        OnRtnTradeField f{};
        std::strcpy(f.trade.InstrumentID, instrument.c_str());
        std::strcpy(f.trade.ExchangeID, exchange_id);
        std::strcpy(f.trade.TradeID, trade_id);
        std::strcpy(f.trade.TradeTime, "10:00:00");
        // #423: 回报必须自证属于当前会话交易日, 正常路径用例一律带会话交易日。
        std::strcpy(f.trade.TradingDay, kTradingDayText);
        f.trade.Direction = direction;
        f.trade.OffsetFlag = THOST_FTDC_OF_Open;
        f.trade.Volume = volume;
        f.trade.Price = price;
        f.account_id = "acc1";
        return f;
    }

    /// 一条委托回报（缺省 = 买开限价挂单；TradingDay 可覆盖以构造跨日/空/非法场景）。
    static OnRtnOrderField order_row(const std::string& instrument,
                                     const char* order_ref,
                                     const char* trading_day = kTradingDayText,
                                     char status = THOST_FTDC_OST_NoTradeQueueing) {
        OnRtnOrderField f{};
        std::strcpy(f.order.BrokerID, "9999");
        std::strcpy(f.order.InvestorID, "acc1");
        std::strcpy(f.order.InstrumentID, instrument.c_str());
        std::strcpy(f.order.OrderRef, order_ref);
        std::strcpy(f.order.ExchangeID, "CFFEX");
        std::strcpy(f.order.TradingDay, trading_day);
        std::strcpy(f.order.InsertTime, "09:30:45");
        f.order.Direction = THOST_FTDC_D_Buy;
        f.order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
        f.order.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
        f.order.OrderStatus = status;
        f.order.LimitPrice = 3900.0;
        f.order.VolumeTotalOriginal = 1;
        f.account_id = "acc1";
        return f;
    }

    /// 读出 reader_ 里新增的帧并一次分类：Reader 只前进不回头，分两次读会把后一类的帧漏掉
    /// （帧读取/解码的共享实现见 shm_frame_reader.h）。
    struct FrameInstruments {
        std::vector<std::string> positions;
        std::vector<std::string> trades;
        std::vector<std::string> orders;
        std::vector<std::string> funds;
    };
    FrameInstruments read_frame_instruments() {
        const auto frames = dztrader::test::read_frames(*reader_);
        return FrameInstruments{dztrader::test::position_frame_instruments(frames),
                                dztrader::test::trade_frame_instruments(frames),
                                dztrader::test::order_frame_instruments(frames),
                                dztrader::test::trading_account_frame_accounts(frames)};
    }

    static bool contains(const std::vector<std::string>& values, const std::string& needle) {
        return std::find(values.begin(), values.end(), needle) != values.end();
    }

    // === 运行期资金刷新夹具（ADR 0055 / #458） ===

    /// 安装确定性时钟 + 发送钩子（不改变状态；未 Ready 用例也用它验证「不置脏」）。
    void install_runtime_query_hooks() {
        session_->set_query_clock([this]() { return query_fake_now_; });
        session_->set_query_send_hook(
            [this](CThostFtdcQryTradingAccountField& /*qry*/, int request_id) {
                sent_fund_query_ids_.push_back(request_id);
                return send_fund_ret_;
            });
    }

    /// 注入确定性时钟 + 发送钩子，并把会话推进到 Ready（生产由登录收尾链进入）。
    void enable_runtime_fund_refresh(bool position_ok = true) {
        install_runtime_query_hooks();
        session_->enter_ready_for_test(position_ok);
    }

    void advance_fake_time(int64_t ms) { query_fake_now_ += std::chrono::milliseconds(ms); }

    static std::string padded(int64_t n, size_t width) {
        std::string text = std::to_string(n);
        if (text.size() < width) text.insert(0, width - text.size(), '0');
        return text;
    }

    void trigger_order_event(int64_t n) {
        const std::string ref = padded(n, 12);
        session_->on_rtn_order(order_row("IF2506", ref.c_str()));
    }

    void trigger_trade_event(int64_t n) {
        const std::string trade_id = padded(n, 8);
        session_->on_rtn_trade(trade_row("IF2506", trade_id.c_str()));
    }

    void trigger_transfer_rsp() {
        OnRspFromBankToFutureByFutureField f{};
        f.req_transfer = CThostFtdcReqTransferField{};
        std::strcpy(f.req_transfer->TradeCode, "202002");
        f.req_transfer->TradeAmount = 1000.0;
        f.account_id = "acc1";
        session_->on_rsp_transfer(f);
    }

    void trigger_transfer_rtn() {
        OnRtnFromBankToFutureByFutureField f{};
        std::strcpy(f.rsp_transfer.TradeCode, "202002");
        f.rsp_transfer.TradeAmount = 2000.0;
        f.account_id = "acc1";
        session_->on_rtn_transfer(f);
    }

    /// 按运行期在途 request id 收口一笔成功资金响应。
    void feed_fund_response(int request_id, double balance = 100000.0) {
        session_->on_rsp_qry_trading_account(
            trading_account_row(/*is_last=*/true, balance, 85000.0, request_id));
    }

    /// 当日持仓行（让资金行的「锚点日不前进」断言有对照；生产上由登录链写入）。
    void seed_today_position_row() {
        raw_exec(
            "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, "
            "volume,"
            " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
            " VALUES ('acc1','20261009','IF2506','CFFEX',1,2,0,1,1,3900.0,20,'dztd_test')");
    }
};

// ============================================================================
// AC: 查询失败保持上次已知快照（不改表、不清零、不预置 0）
// ============================================================================

TEST_F(TdPositionLifecycleTest, FailedQueryTouchesNothingAndKeepsPreviousSnapshot) {
    // 上一交易日的"上次已知快照"（跨会话保留的形态）与当日既有行。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','IF2506','CFFEX',1,3,0,3,0,3910.0,20,'dztd_test')");

    session_->on_rsp_qry_investor_position(failed_position_query());
    flush_persist();

    // 查询失败不改表：当日行 seq/内容完全不变，历史行不动，也没有新增的行。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions"), 2);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261009'"), 3);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE date='20261009'"), 20)
        << "a failed query must not rewrite seq or content";
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261008'"), 5);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE date='20261008'"), 10);
}

TEST_F(TdPositionLifecycleTest, FailedQueryOnEmptyDayWritesNoPlaceholderRows) {
    // 当日无行、只有上一交易日快照 → 失败查询不得预置 0 行（ADR 0045 决策 3）。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");

    session_->on_rsp_qry_investor_position(failed_position_query());
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0)
        << "login/query failure must not pre-write volume=0 rows for the current day";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261008'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261008'"), 5);
}

TEST_F(TdPositionLifecycleTest, FailedPositionQuerySuppressesFundRowSoAnchorDayStays) {
    // 契约 ADR 0045 决策 4 / ADR 0046 的跨表不变量：**资金行只在持仓查询成功之后写入**。
    // 若持仓失败仍写资金当日行，锚点日（两快照表 MAX(date)）会被单独推进到当日，
    // 而 positions 当日无行 → 持仓 REST 返回空数组 = 假空仓（ADR 0045 决策 3 的最危险形态）。
    // ADR 0049 后生产时序不再「失败照常发资金查询」（改为有限重试），但唯一写入点的守卫
    // 仍须成立：资金响应在失败窗口到达时同样不得落库（防御性断言）。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");

    session_->on_rsp_qry_investor_position(failed_position_query());
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true));
    flush_persist();

    // 资金当日行**不得写入**——否则锚点日被推到当日。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 0)
        << "fund row must not be written when the position query failed (ADR 0045 decision 4)";
    // 锚点日仍是上一成功日 = 两快照表 MAX(date) 都停在 10-08。
    EXPECT_EQ(scalar_int("SELECT MAX(date) FROM positions"), 20261008);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0);
}

TEST_F(TdPositionLifecycleTest, SuccessfulPositionQueryAllowsFundRowSameDay) {
    // 反面对照：持仓成功后资金行与持仓行同日（锚点推进时持仓行集已一致）。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true));
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1)
        << "after a successful position query the fund row joins the same anchor day";
    EXPECT_EQ(scalar_int("SELECT MAX(date) FROM positions"),
              scalar_int("SELECT MAX(date) FROM trading_accounts"));
}

// ============================================================================
// AC: 完整成功查询 → 整组原子替换（历史行不变、消失的合约被删）
// ============================================================================

TEST_F(TdPositionLifecycleTest, SuccessfulQueryReplacesWholeDayGroupAndKeepsHistory) {
    // 当日旧行：IF2506（仍在新快照里）+ rb2510（新快照里已消失 → 必须被删）。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','IF2506','CFFEX',1,9,0,9,0,3800.0,20,'dztd_test')");
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','rb2510','SHFE',1,4,0,4,0,3500.0,21,'dztd_test')");
    // 历史交易日行不参与重建。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','rb2510','SHFE',1,9,0,9,0,3400.0,5,'dztd_test')");

    // 成功快照 = IF2506 多 2（今 1 + 昨 1）。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});

    // 当日整组只剩 IF2506；消失的 rb2510 被删除（不产生幽灵持仓）。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM positions WHERE date='20261009'"), "IF2506");
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261009'"), 2);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE date='20261009'"), 1);
    // 历史交易日行不可变。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261008'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261008'"), 9);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE date='20261008'"), 5);
}

// ============================================================================
// AC(#411): 今昨按 TodayPosition 计算（YdPosition 是静态昨仓，不得用于当前昨仓）
// ============================================================================

TEST_F(TdPositionLifecycleTest, QueryAggregationUsesTodayPositionNotStaticYdPosition) {
    // CTP 单行（非 SHFE/INE）：开盘昨仓 6，盘中开 6 手今仓、平昨 2 手 →
    // Position=10 / TodayPosition=6 / YdPosition=6（静态）。当前应为 今 6 / 昨 4。
    feed_position_snapshot({position_row("IF2506", 10, 4, 10.0 * 300.0 * 3900.0, /*is_last=*/true,
                                         THOST_FTDC_PD_Long, /*exchange_id=*/"CFFEX",
                                         /*yd_static=*/6)});
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261009'"), 10);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE date='20261009'"), 6);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE date='20261009'"), 4)
        << "current yd = Position - TodayPosition, not the static YdPosition (6)";
}

TEST_F(TdPositionLifecycleTest, ShfeTwoRowQueryAggregatesTodayRowAndYesterdayRow) {
    // SHFE/INE 同一（合约, 方向）分两行返回：今仓行（Position=TodayPosition=5）+ 昨仓行
    // （Position=剩余昨 3、TodayPosition=0、静态 YdPosition=5，即今日已平昨 2 手）。
    // 聚合后 = 总 8 / 今 5 / 昨 3；旧聚合口径（累加 YdPosition）会得今 3 / 昨 5。
    feed_position_snapshot({
        position_row("rb2510", 5, 0, 5.0 * 10.0 * 3500.0, /*is_last=*/false, THOST_FTDC_PD_Long,
                     /*exchange_id=*/"SHFE"),
        position_row("rb2510", 3, 3, 3.0 * 10.0 * 3500.0, /*is_last=*/true, THOST_FTDC_PD_Long,
                     /*exchange_id=*/"SHFE", /*yd_static=*/5),
    });
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261009'"), 8);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE date='20261009'"), 5);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE date='20261009'"), 3);
}

TEST_F(TdPositionLifecycleTest, ZeroPositionRowIsNotWrittenAsZeroVolumeRow) {
    // CTP 会返回 Position=0 的「已全平」行（其他字段可能非 0，静态 YdPosition 仍 >0 表示
    // 开盘曾有昨仓）。它不代表当前持仓：不落 0 量行，已平合约由整组原子替换移除
    // （契约 td-data-sync §3「消失的合约由原子替换移除」）。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','IF2506','CFFEX',1,4,0,4,0,3900.0,20,'dztd_test')");

    // 旧实现用静态 YdPosition>0 判「有持仓行」，会把这一行聚合出一条 0 量行并重灌落库。
    feed_position_snapshot({position_row("IF2506", 0, 0, 0.0, /*is_last=*/true, THOST_FTDC_PD_Long,
                                         /*exchange_id=*/"CFFEX",
                                         /*yd_static=*/5)});

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0);
}

TEST_F(TdPositionLifecycleTest, EmptySuccessfulQueryClearsCurrentDayRowsOnly) {
    // 当日有仓，历史日也有仓；一次「成功但空」的查询 = 当日全平。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','IF2506','CFFEX',1,3,0,3,0,3910.0,20,'dztd_test')");
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,7,0,7,0,3900.0,5,'dztd_test')");

    feed_position_snapshot({});

    // 当日行被清空（真空仓，不是「从未查询过」）；历史行保留。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261008'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261008'"), 7);
}

// ============================================================================
// AC: 删户重加 / 盘中换柜台——新会话重建是整组替换（不是追加）
// ============================================================================

TEST_F(TdPositionLifecycleTest, NewSessionSameDayRebuildReplacesGroupInsteadOfAppending) {
    // 会话 A（旧柜台 / 删除前的账户实例）：快照 = IF2506 多 2 + rb2510 空 3。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    // 第二个合约用另一轮查询写入（同一轮多行也行；这里用两轮表达「两行都在库里」）。
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);

    // 删除账户 = 销毁会话（配置层 RemoveAccount 不删库：td_api_pure 的 RemoveAccount
    // 只改配置，全仓无 DELETE FROM positions）。这里销毁 A、重建同 account_id 的会话 B，
    // 等价于删户重加 / 盘中换柜台后的新实例。
    session_.reset();
    session_ = make_session();
    session_->set_trading_day(kDay);

    // 会话 B 的首个完整成功查询：只剩 rb2510 空 4（上一轮的 IF2506 已平、rb2510 数量变了）。
    feed_position_snapshot({position_row("rb2510", 4, 4, 4.0 * 300.0 * 3500.0, /*is_last=*/true)});

    // 新会话的整组重建**替换**当日行：不是追加，不残留上一会话独有的 IF2506，
    // 也不产生重复行（唯一键 (account_id,date,instrument_id,direction)）。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM positions WHERE date='20261009'"), "rb2510");
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261009'"), 4);
}

// ============================================================================
// AC: 日切 / 多日共存只碰当日行
// ============================================================================

TEST_F(TdPositionLifecycleTest, DaySwitchWritesNewDayRowAndKeepsHistoricalSplit) {
    // 上一交易日：今仓 5（当日盘中开仓的形态）。
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");

    // 新交易日的首个成功查询：全部成为昨仓（today 0 / yd 5）。
    feed_position_snapshot({position_row("IF2506", 5, 5, 5.0 * 300.0 * 3900.0, /*is_last=*/true)});

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions"), 2) << "two trading days coexist";
    // 当日行：今 0 / 昨 5。
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE date='20261009'"), 0);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE date='20261009'"), 5);
    // 历史行保持它自己当时的今昨拆分（重建只碰当日）。
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE date='20261008'"), 5);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE date='20261008'"), 0);
}

// ============================================================================
// AC(#250): 资金多日快照 / 同账户同日幂等 / 日切与重连正确
// ============================================================================

TEST_F(TdPositionLifecycleTest, SameDayFundQueryUpsertsInsteadOfAppending) {
    // 登录收尾链：持仓查询成功后才写资金行（ADR 0045 决策 4）。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 100000.0, 85000.0));
    flush_persist();
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);
    const int64_t first_seq = scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'");

    // 同一交易日再次成功查询（重传 / 重新登录后的首轮）：同键 (account_id,date) upsert，
    // 不追加行、历史行不受影响；余额类列与 seq 更新为最新查询结果。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 120000.0, 95000.0));
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1)
        << "same account+date is one row (business key), not an append";
    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261009'"),
              120000);
    EXPECT_EQ(scalar_int("SELECT CAST(available AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261009'"),
              95000);
    EXPECT_GT(scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'"), first_seq)
        << "each successful query takes a new account-level seq";
}

// AC(#457): 查询响应按 nRequestID 关联本链——迟到 / 串号响应必须丢弃（不写表、不推进收尾）。
TEST_F(TdPositionLifecycleTest, QueryResponsesWithWrongRequestIdAreDropped) {
    // 未发起查询的会话 request id = 0（同生产：每次查询前 ++request_id_）。
    // 先用错误 id 注入合约行 / 资金行：都应被丢弃。
    session_->on_rsp_qry_instrument(
        instrument_row("IF2506", "CFFEX", THOST_FTDC_PC_Futures, /*request_id=*/99));
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(
        trading_account_row(/*is_last=*/true, 100000.0, 85000.0, /*request_id=*/99));
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM instruments"), 0)
        << "stale instrument response must not be persisted";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0)
        << "stale trading account response must not be persisted";

    // 正确 id（未发起时 = 0）的响应才被接受。
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 100000.0, 85000.0));
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);
}

TEST_F(TdPositionLifecycleTest, DaySwitchFundQueriesCoexistAsTwoDayRows) {
    // 交易日 D1：持仓 + 资金写入 D1 行。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 100000.0, 85000.0));
    flush_persist();
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);
    const int64_t day1_seq = scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'");

    // 日切 → 重连/新登录链：新会话（生产上 = 交易日推进后的重新登录）写 D2 行。
    session_.reset();
    session_ = make_session();
    const int32_t next_day = day(2026, 10, 10);
    session_->set_trading_day(next_day);
    feed_position_snapshot({position_row("IF2506", 2, 0, 2.0 * 300.0 * 3910.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 130000.0, 70000.0));
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 2) << "two trading days coexist";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261010'"), 1);

    // 历史日行不可变：余额与 seq 都保持 D1 自己的值。
    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261009'"),
              100000);
    EXPECT_EQ(scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'"), day1_seq);
    // 新日行是新查询结果。
    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261010'"),
              130000);
    // 锚点日一致性：资金行与持仓行同属 D2（快照表同进同退）。
    EXPECT_EQ(scalar_int("SELECT MAX(date) FROM positions"),
              scalar_int("SELECT MAX(date) FROM trading_accounts"));
}

TEST_F(TdPositionLifecycleTest, NewSessionSameDayFundQueryOverwritesRowInPlace) {
    // 同一交易日内的重连/删户重加：新会话的首轮成功查询把同一行覆盖（不追加、不丢行）。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 100000.0, 85000.0));
    flush_persist();
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);

    session_.reset();
    session_ = make_session();
    session_->set_trading_day(kDay);
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 99000.0, 84000.0));
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);
    EXPECT_EQ(
        scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts WHERE date='20261009'"),
        99000);
}

// ============================================================================
// 基准查询失败策略 (#413 / ADR 0049 决策 3):
// 有限重试 → 重试耗尽停住 + ERROR 弹窗通知; 零行不是失败.
// ============================================================================

TEST_F(TdPositionLifecycleTest, BaselineQueryFailureRetriesThenStopsWithErrorNotification) {
    // 上次已知快照: 证明整条失败链(含每次重试调度)不碰表.
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");

    // 生产上每次重试由 3s 定时器发起; 本夹具不连柜台(api_ 为空), 无法真发请求,
    // 故直接按「尝试序列」喂 is_last 失败响应 —— 会话侧的重试预算判定/定时器排定/
    // 耗尽后的停住与通知仍走真实代码路径 (定时器只排定不触发, 预算按响应推进).
    const int kTotalAttempts = 1 + kPositionQueryMaxRetries;  // 首次 + 3 次重试
    for (int attempt = 1; attempt <= kTotalAttempts; ++attempt) {
        session_->on_rsp_qry_investor_position(failed_position_query());
        EXPECT_EQ(notifications_.size(), attempt < kTotalAttempts ? 0u : 1u)
            << "notification must be raised exactly once, after the retry budget is exhausted";
        if (attempt < kTotalAttempts) {
            // 预算未耗尽: 排了重试定时器 (待命), 尚未放弃登录.
            ASSERT_FALSE(timer_queue_.empty())
                << "a retry must be scheduled while the budget remains";
            // 待触发的最近定时器必须是 3 秒重试, 而不是 5 分钟单次尝试超时 ——
            // 只看队列非空无法区分两者, 排错定时器也会绿.
            EXPECT_LE(timer_queue_.next_timeout(), std::chrono::seconds(3));
        }
    }
    flush_persist();

    // 耗尽后: 一次 ERROR + popup 通知, 文案只陈述事实 (不承诺 UI 不存在的操作, #409).
    // 文案的逐字锁定在 td_account_session_pure_test（唯一真相源 = position_query_failed_message）,
    // 这里只钉「会话确实把该文案推给了 UI」.
    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_EQ(notifications_.front().message, position_query_failed_message());
    // 不再排任何定时器: 不自动恢复, 等用户处理 (ADR 0049 决策 3).
    EXPECT_TRUE(timer_queue_.empty()) << "no timer may remain after the retry budget is exhausted";
    // 没有转 Ready (夹具未登录, 这里钉「未就绪」而不是具体状态; LoadingInstruments 的停驻
    // 由 LoginFinalizer 纯逻辑用例钉住).
    EXPECT_FALSE(session_->is_ready());
    EXPECT_FALSE(session_->position_query_ok());
    // 失败链不碰表: 当日无行, 上一交易日快照原样.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE date='20261008'"), 5);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0);

    // 预算耗尽后迟到的响应不得复活本次登录 (on_rsp_qry_investor_position 的失败终态早退):
    // 喂一条「成功」is_last（含一行持仓）与一条失败 is_last，都必须被拦住 ——
    // 不写表、不二次弹窗、不排定时器、position_query_ok 不得回 true.
    session_->on_rsp_qry_investor_position(
        position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true));
    session_->on_rsp_qry_investor_position(failed_position_query());
    flush_persist();
    EXPECT_EQ(notifications_.size(), 1u) << "late responses must not raise a second notification";
    EXPECT_TRUE(timer_queue_.empty()) << "late responses must not schedule retries";
    EXPECT_FALSE(session_->is_ready());
    EXPECT_FALSE(session_->position_query_ok())
        << "a late success must not revive the failed login";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0)
        << "a late response after the login is failed must not write the snapshot";
}

TEST_F(TdPositionLifecycleTest, ZeroRowSuccessfulQueryIsNotFailureAndSchedulesNoRetry) {
    // 「查不到」不是失败 (ADR 0049 决策 3): is_last 正常到达但零行 = 成功, 空账户合法.
    // 失败路径才会排重试定时器 + 发弹窗; 零行必须两者都不发生, 且当日空集被整组替换.
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261009','IF2506','CFFEX',1,3,0,3,0,3910.0,20,'dztd_test')");

    feed_position_snapshot({});  // 只有 is_last, 无行, 无 ErrorID

    EXPECT_TRUE(session_->position_query_ok())
        << "zero rows with a clean is_last is a successful query, not a failure";
    EXPECT_TRUE(timer_queue_.empty()) << "a successful zero-row query must not schedule retries";
    EXPECT_TRUE(notifications_.empty()) << "success must not raise any notification";
    // 成功查询整组替换为当日空集 (当日旧行被删, 历史不受影响).
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 0);
}

// ============================================================================
// AC(#414): 套利/组合合约不参与持仓统计 (ADR 0049 决策 6)
// ============================================================================

TEST_F(TdPositionLifecycleTest, SpreadPositionRowIsSkippedWhileLegRowIsKept) {
    // 组合合约（CTP ProductClass='3' -> DZ_PRODUCT_SPREAD）的持仓行不入库、不推帧；
    // 单腿合约是普通期货合约，照常入库/推帧。这正是「盘后自动组合」在库里表现为
    // 「持仓比柜台少一行」的原因（DEBUG 日志留证）。
    register_instrument("SP m2501&m2505", "DCE", THOST_FTDC_PC_Combination);
    register_instrument("m2501", "DCE", THOST_FTDC_PC_Futures);

    feed_position_snapshot({
        position_row("SP m2501&m2505", 2, 0, 0.0, /*is_last=*/false, THOST_FTDC_PD_Long, "DCE"),
        position_row("m2501", 3, 1, 3.0 * 300.0 * 3100.0, /*is_last=*/true, THOST_FTDC_PD_Long,
                     "DCE"),
    });

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='SP m2501&m2505'"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='m2501'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='m2501'"), 3);
    const auto frames = read_frame_instruments();
    EXPECT_FALSE(contains(frames.positions, "SP m2501&m2505"))
        << "spread rows must not be pushed as positions";
    EXPECT_TRUE(contains(frames.positions, "m2501"))
        << "leg rows are ordinary futures and must be pushed";
    // 跳过组合行不影响整轮查询的成功判定（否则账户会登录失败）。
    EXPECT_TRUE(session_->position_query_ok());
}

TEST_F(TdPositionLifecycleTest, SpreadTradeIsRecordedButNotAppliedToPosition) {
    // 组合合约成交照常落库/推帧，但不参与持仓统计；两条腿（普通期货合约）的持仓按各自
    // 成交正常增量维护——这是 AC「套利成交后两腿持仓正确」的回归。
    register_instrument("SP m2501&m2505", "DCE", THOST_FTDC_PC_Combination);
    register_instrument("m2501", "DCE", THOST_FTDC_PC_Futures);
    register_instrument("m2505", "DCE", THOST_FTDC_PC_Futures);
    // 基准：价差两腿 = m2501 多 2 手 + m2505 空 2 手（全今仓）。
    feed_position_snapshot({position_row("m2501", 2, 0, 2.0 * 300.0 * 3100.0, /*is_last=*/false,
                                         THOST_FTDC_PD_Long, "DCE"),
                            position_row("m2505", 2, 0, 2.0 * 300.0 * 3050.0, /*is_last=*/true,
                                         THOST_FTDC_PD_Short, "DCE")});
    (void)read_frame_instruments();  // 丢弃基准帧，只看后面的增量

    session_->on_rtn_trade(trade_row("SP m2501&m2505", "T001", 2, 3000.0, /*exchange_id=*/"DCE"));
    session_->on_rtn_trade(trade_row("m2501", "T002", 1, 3100.0, /*exchange_id=*/"DCE"));
    session_->on_rtn_trade(
        trade_row("m2505", "T003", 1, 3050.0, /*exchange_id=*/"DCE", THOST_FTDC_D_Sell));
    flush_persist();

    // 三笔成交都照常落库（套利成交不丢）。
    for (const char* trade_id : {"T001", "T002", "T003"}) {
        EXPECT_EQ(
            scalar_int(std::format("SELECT COUNT(*) FROM trades WHERE trade_id='{}'", trade_id)), 1)
            << trade_id;
    }
    // 组合合约不产生持仓行/持仓帧。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='SP m2501&m2505'"), 0);
    // 两腿各自按成交增量维护（多头买开 2 -> 3；空头卖开 2 -> 3），今昨拆分同为全今仓。
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='m2501' AND"
                         " direction=1"),
              3);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE instrument_id='m2501'"), 3);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='m2505' AND"
                         " direction=-1"),
              3);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE instrument_id='m2505'"), 3);
    const auto frames = read_frame_instruments();
    EXPECT_FALSE(contains(frames.positions, "SP m2501&m2505"));
    EXPECT_TRUE(contains(frames.positions, "m2501"));
    EXPECT_TRUE(contains(frames.positions, "m2505"));
    // 成交帧照常推送（含组合合约）。
    EXPECT_TRUE(contains(frames.trades, "SP m2501&m2505"));
    EXPECT_TRUE(contains(frames.trades, "m2501"));
    EXPECT_TRUE(contains(frames.trades, "m2505"));
}

// ============================================================================
// AC(#414): 净持仓('1') 跳过该行 + WARN，不触发整轮作废 (ADR 0049 决策 9)
// ============================================================================

TEST_F(TdPositionLifecycleTest, NetDirectionRowIsSkippedWithoutFailingTheQuery) {
    // 喂一行净持仓('1') + 一行正常多头：正常行照常入库、净持仓行不出现，该轮查询**仍判成功**
    // —— 净持仓是合法 CTP 枚举，不能落进「未知方向 → 整轮快照不可信」分支，否则账户会因
    // 一个实际不存在的输入登录不上（ADR 0049 决策 9）。
    feed_position_snapshot({
        position_row("IF2506", 4, 0, 4.0 * 300.0 * 3900.0, /*is_last=*/false, THOST_FTDC_PD_Net),
        position_row("rb2510", 2, 1, 2.0 * 10.0 * 3500.0, /*is_last=*/true, THOST_FTDC_PD_Long,
                     "SHFE"),
    });

    EXPECT_TRUE(session_->position_query_ok())
        << "net direction is a legal enum and must not invalidate the snapshot";
    EXPECT_TRUE(timer_queue_.empty()) << "a legal enum must not schedule a query retry";
    EXPECT_TRUE(notifications_.empty()) << "a legal enum must not raise an ERROR notification";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM positions WHERE date='20261009'"), "rb2510");
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='IF2506'"), 0)
        << "net direction must be skipped, not mapped to long";
}

TEST_F(TdPositionLifecycleTest, NetAndSpreadRowsAreBothSkippedInTheSameQuery) {
    // 组合行与净持仓行同时出现：两条跳过路径互不干扰，都不登记未知枚举，整轮查询仍成功。
    register_instrument("SP m2501&m2505", "DCE", THOST_FTDC_PC_Combination);
    feed_position_snapshot({
        position_row("SP m2501&m2505", 2, 0, 0.0, /*is_last=*/false, THOST_FTDC_PD_Long, "DCE"),
        position_row("IF2506", 4, 0, 4.0 * 300.0 * 3900.0, /*is_last=*/false, THOST_FTDC_PD_Net),
        position_row("rb2510", 2, 1, 2.0 * 10.0 * 3500.0, /*is_last=*/true, THOST_FTDC_PD_Long,
                     "SHFE"),
    });

    EXPECT_TRUE(session_->position_query_ok());
    EXPECT_TRUE(timer_queue_.empty()) << "neither skip path may schedule a query retry";
    EXPECT_TRUE(notifications_.empty()) << "neither skip path is an unknown-enum error";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20261009'"), 1);
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM positions WHERE date='20261009'"), "rb2510");
}

// ============================================================================
// AC(#423): 会话不得跨交易日 —— 回报 TradingDay 不能自证即判死 + 请求主动登出
// (ADR 0049 决策 5 / 契约 td-data-sync §3 §7)
// ============================================================================

TEST_F(TdPositionLifecycleTest, SameDayTradeIsStillProcessed) {
    // 回归基线: 回报带对会话交易日时行为与 #423 之前完全一致 (不误杀正常回报).
    session_->on_rtn_trade(trade_row("m2501", "T300"));
    flush_persist();

    EXPECT_TRUE(notifications_.empty()) << "normal report must not raise a notification";
    EXPECT_FALSE(session_->take_cross_day_logout_request());
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 1);
    EXPECT_TRUE(contains(read_frame_instruments().trades, "m2501"));
}

TEST_F(TdPositionLifecycleTest, CrossDayTradeIsDiscardedAndRequestsLogout) {
    // 回报交易日与会话交易日不一致 (期货公司没在日切时断连): 记 ERROR + 弹窗通知 + 请求登出;
    // 该条成交不落库不推帧 —— 它的 date 取自会话交易日, 落库即永久污染追加流 (次日登录的
    // 整组替换只覆盖持仓/资金, 修不回委托/成交).
    auto trade = trade_row("m2501", "T301");
    std::strcpy(trade.trade.TradingDay, "20261012");
    session_->on_rtn_trade(trade);
    flush_persist();

    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_EQ(notifications_[0].level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_[0].popup) << "forced logout is destructive: it must pop up";
    EXPECT_NE(notifications_[0].message.find("不一致"), std::string::npos)
        << notifications_[0].message;
    EXPECT_TRUE(session_->take_cross_day_logout_request());
    EXPECT_FALSE(session_->take_cross_day_logout_request()) << "one-shot per session";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 0);
    EXPECT_TRUE(read_frame_instruments().trades.empty()) << "rejected report must not be pushed";
}

TEST_F(TdPositionLifecycleTest, CrossDayOrderIsDiscardedAndRequestsLogout) {
    // 委托回报走同一条判据 (两个 handler 都要覆盖, 漏一个等于漏检); 与成交用例对称:
    // 不落库 + 不推帧 (帧断言依赖 reader_ 在 SetUp 里先建、早于本用例的写动作).
    auto order = order_row("m2501", "000000000001", /*trading_day=*/"20261012");
    session_->on_rtn_order(order);
    flush_persist();

    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_EQ(notifications_[0].level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(session_->take_cross_day_logout_request());
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);
    EXPECT_TRUE(read_frame_instruments().orders.empty()) << "rejected report must not be pushed";
}

TEST_F(TdPositionLifecycleTest, EmptyOrMalformedReportDayIsAlsoRejected) {
    // 拍板 ③c: 空串 / 解析失败与「不相等」同罪 (都属无法自证) —— 不退回会话交易日继续跑.
    auto empty_day = trade_row("m2501", "T302");
    empty_day.trade.TradingDay[0] = '\0';
    session_->on_rtn_trade(empty_day);
    flush_persist();
    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_NE(notifications_[0].message.find("未携带交易日"), std::string::npos)
        << notifications_[0].message;
    EXPECT_TRUE(session_->take_cross_day_logout_request());

    // 换新会话覆盖解析失败分支: 8 位但含非数字 (定长字段里可能有空位/脏数据).
    session_ = make_session();
    session_->set_trading_day(kDay);
    auto malformed = trade_row("m2501", "T303");
    std::strcpy(malformed.trade.TradingDay, "2026100X");
    session_->on_rtn_trade(malformed);
    flush_persist();
    ASSERT_EQ(notifications_.size(), 2u);
    EXPECT_NE(notifications_[1].message.find("无法解析"), std::string::npos)
        << notifications_[1].message;
    EXPECT_TRUE(session_->take_cross_day_logout_request());
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 0);
}

TEST_F(TdPositionLifecycleTest, RepeatedCrossDayReportsNotifyOnce) {
    // 同一会话只通知/请求一次登出: 后续回报静默丢弃 (逐条弹窗会刷爆前端通知环).
    for (const char* trade_id : {"T304", "T305", "T306"}) {
        auto trade = trade_row("m2501", trade_id);
        std::strcpy(trade.trade.TradingDay, "20261012");
        session_->on_rtn_trade(trade);
    }
    flush_persist();
    EXPECT_EQ(notifications_.size(), 1u);
    EXPECT_TRUE(session_->take_cross_day_logout_request());
}

// ============================================================================
// 运行期资金刷新（ADR 0055 / #458）：四类触发源 + 合并 + 写路径 + 失败退避
// ============================================================================

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshTriggersOnOrderAndWritesRowFrame) {
    seed_today_position_row();
    // 历史资金行（上一交易日）：运行期刷新只 upsert 当日行，历史行不可变。
    raw_exec(
        "INSERT INTO trading_accounts (account_id, date, balance, available, seq, gateway)"
        " VALUES ('acc1','20261008',999.0,888.0,5,'dztd_test')");
    enable_runtime_fund_refresh();

    trigger_order_event(1);
    session_->pump_query_lane();  // 确定时钟下测试直接驱动（生产由 lane 定时器回调）
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);

    feed_fund_response(sent_fund_query_ids_.back(), /*balance=*/123456.0);
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261009'"),
              123456);
    EXPECT_GT(scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'"), 0);
    const auto frames = read_frame_instruments();
    ASSERT_EQ(frames.funds.size(), 1u);
    EXPECT_EQ(frames.funds[0], "acc1");
    // 运行期写当日行不推进锚点日（两快照表 MAX(date) 同日）。
    EXPECT_EQ(scalar_int("SELECT MAX(date) FROM positions"), 20261009);
    EXPECT_EQ(scalar_int("SELECT MAX(date) FROM trading_accounts"), 20261009);
    // 历史日资金行保持原值（不改写、不删除）。
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261008'"), 1);
    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261008'"),
              999);
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshCoalescesWindowAndReplenishesAfterSend) {
    seed_today_position_row();
    enable_runtime_fund_refresh();

    // 间隔窗口内多类事件（委托 x2 + 成交）只产生 1 笔查询。
    trigger_order_event(1);
    trigger_trade_event(1);
    trigger_order_event(2);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u) << "window events must coalesce";
    const int first_id = sent_fund_query_ids_.back();

    // 查询在途期间到达的事件：收口后补查一次（不丢）。
    trigger_order_event(3);
    feed_fund_response(first_id);
    session_->pump_query_lane();  // 最小间隔未到 → 不发
    EXPECT_EQ(sent_fund_query_ids_.size(), 1u);

    advance_fake_time(1100);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 2u) << "event during in-flight must be re-queried";
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshRespectsPositionBaselineGuard) {
    // 持仓基准未就绪（ADR 0045 决策 4）：不写资金行、不推帧，避免锚点日被单独推进。
    seed_today_position_row();
    enable_runtime_fund_refresh(/*position_ok=*/false);

    trigger_trade_event(1);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    feed_fund_response(sent_fund_query_ids_.back());
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0)
        << "fund row must not be written when the position baseline is not ready";
    EXPECT_TRUE(read_frame_instruments().funds.empty());
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshSuppressedBeforeReady) {
    // 登录缓冲 / 重放 / 未就绪阶段：事件不置脏，也不产生查询。
    install_runtime_query_hooks();
    trigger_order_event(1);
    trigger_trade_event(1);
    // 事件发生在非 Ready 阶段：不置脏。随后即使进入 Ready，也不能凭空补一笔。
    session_->enter_ready_for_test();
    session_->pump_query_lane();
    EXPECT_TRUE(sent_fund_query_ids_.empty());
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshTriggeredByTransferEvents) {
    seed_today_position_row();
    enable_runtime_fund_refresh();

    trigger_transfer_rsp();
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    feed_fund_response(sent_fund_query_ids_.back());
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);
    {
        const auto first_frames = read_frame_instruments();
        ASSERT_EQ(first_frames.funds.size(), 1u);
        EXPECT_EQ(first_frames.funds[0], "acc1");
    }
    const int64_t first_seq = scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'");

    // 出入金实时通知（银行权威）同样触发；等最小间隔后补一笔。
    trigger_transfer_rtn();
    advance_fake_time(1100);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 2u);
    feed_fund_response(sent_fund_query_ids_.back(), /*balance=*/200000.0);
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1)
        << "same-day refresh upserts the single business-key row";
    EXPECT_EQ(scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts"
                         " WHERE date='20261009'"),
              200000);
    // AC6：每次成功刷新都推新帧并取新 seq（不做「值未变不写」优化）。
    const auto second_frames = read_frame_instruments();
    ASSERT_EQ(second_frames.funds.size(), 1u);
    EXPECT_EQ(second_frames.funds[0], "acc1");
    EXPECT_GT(scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'"), first_seq);
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshFailureBacksOffAndNotifiesThrottled) {
    seed_today_position_row();
    enable_runtime_fund_refresh();
    send_fund_ret_ = -3;  // 柜台流控拒绝 → 发送失败路径

    // 每轮失败 4 次 = 1 次退避耗尽（≤3 次重试）；事件重新武装后下一轮再试。
    auto exhaust_one_round = [&](int64_t ref_base) {
        for (int i = 0; i < 4; ++i) {
            trigger_order_event(ref_base + i);
            session_->pump_query_lane();
            advance_fake_time(10000);  // 跨过退避窗口（测试时钟，不 sleep）
        }
    };
    exhaust_one_round(100);
    EXPECT_EQ(sent_fund_query_ids_.size(), 4u);
    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_EQ(notifications_[0].level, DZ_NOTIFY_ERROR);
    EXPECT_FALSE(notifications_[0].popup) << "运行期刷新失败不得弹窗";

    // 5 分钟限频窗口内再次耗尽：不再通知（日志留证）。
    exhaust_one_round(200);
    EXPECT_EQ(notifications_.size(), 1u);

    // 超出窗口后可再次通知。
    advance_fake_time(301000);
    exhaust_one_round(300);
    EXPECT_EQ(notifications_.size(), 2u);
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshBusinessErrorAndOnRspErrorBackoff) {
    seed_today_position_row();
    enable_runtime_fund_refresh();

    // 业务错误（is_last 带 ErrorID）：不写表不推帧，进入退避（不悬挂）。
    trigger_order_event(1);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    const int first_id = sent_fund_query_ids_.back();
    OnRspQryTradingAccountField failed{};
    failed.rsp_info = CThostFtdcRspInfoField{};
    failed.rsp_info->ErrorID = 31;
    std::strcpy(failed.rsp_info->ErrorMsg, "insufficient");
    failed.request_id = first_id;
    failed.is_last = true;
    failed.account_id = "acc1";
    session_->on_rsp_qry_trading_account(failed);
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0);

    // OnRspError（通用错误响应）按在途 request id 路由到同一失败路径。
    advance_fake_time(1100);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 2u);
    OnRspErrorTdField rsp_error{};
    rsp_error.rsp_info = CThostFtdcRspInfoField{};
    rsp_error.rsp_info->ErrorID = 3;
    rsp_error.request_id = sent_fund_query_ids_.back();
    session_->on_rsp_error(rsp_error);

    // 退避后第三笔成功：正常写行，证明失败不会永久停住通道。
    // 第 2 次失败退避翻倍（1100 * 2^(2-1) = 2200ms，ADR 0055 决策 6）。
    advance_fake_time(2200);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 3u);
    feed_fund_response(sent_fund_query_ids_.back());
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshLateResponseAfterDisconnectIsDropped) {
    seed_today_position_row();
    enable_runtime_fund_refresh();
    trigger_order_event(1);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    const int request_id = sent_fund_query_ids_.back();

    session_->on_front_disconnected(0);  // 断线：generation 前进，旧响应作废
    feed_fund_response(request_id);
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 0)
        << "late runtime response after leaving Ready must not be written";
}

TEST_F(TdPositionLifecycleTest, ManualRefreshRequestMarksDirtyOnlyWhenReady) {
    seed_today_position_row();
    install_runtime_query_hooks();

    // 非 Ready：手动刷新请求被拒绝，不置脏不查询（契约 td-refresh：只对 Ready 账户受理）。
    EXPECT_FALSE(session_->request_fund_refresh());
    session_->pump_query_lane();
    EXPECT_TRUE(sent_fund_query_ids_.empty());

    // Ready：受理并进入同一置脏路径（受查询通道合并/限速）。
    session_->enter_ready_for_test();
    EXPECT_TRUE(session_->request_fund_refresh());
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    feed_fund_response(sent_fund_query_ids_.back());
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);

    // 断线后再次拒绝（会话已不可查询）。
    session_->on_front_disconnected(0);
    EXPECT_FALSE(session_->request_fund_refresh());
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshTimerWiringDrivesPump) {
    seed_today_position_row();
    enable_runtime_fund_refresh();
    trigger_order_event(1);

    // 事件置脏后应排好 lane 定时器（0 延迟 = 立即可发）；真正触发来自 TimerQueue::tick。
    ASSERT_FALSE(timer_queue_.empty());
    timer_queue_.tick();
    EXPECT_EQ(sent_fund_query_ids_.size(), 1u)
        << "lane timer must drive pump_query_lane (not test-only manual call)";
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshTimeoutEntersBackoff) {
    seed_today_position_row();
    enable_runtime_fund_refresh();
    trigger_order_event(1);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);
    const int first_id = sent_fund_query_ids_.back();

    // 响应丢失 → 10s 超时收口（测试注入同一收口逻辑，不 sleep）：不写表、进入退避。
    session_->expire_runtime_query_timeout_for_test();
    flush_persist();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 0);

    advance_fake_time(1100);  // 第 1 次失败退避 = min_gap
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 2u) << "timeout must not stop the channel";
    EXPECT_NE(sent_fund_query_ids_.back(), first_id);
}

TEST_F(TdPositionLifecycleTest, RuntimeFundRefreshResetsOnDisconnect) {
    seed_today_position_row();
    enable_runtime_fund_refresh();
    trigger_order_event(1);
    session_->pump_query_lane();
    ASSERT_EQ(sent_fund_query_ids_.size(), 1u);

    session_->on_front_disconnected(0);  // 断线：通道复位、状态离开 Ready
    // 断线期间的事件不置脏（不排队补查）。
    trigger_order_event(2);
    session_->pump_query_lane();
    EXPECT_EQ(sent_fund_query_ids_.size(), 1u) << "disconnected session must not schedule queries";

    // 重连后重新 Ready：断线前的在途/待查必须已清空——新事件能立即再发一笔，
    // 而不是被旧的 in-flight 永久挡住（falsifiable：reset 缺失时这里不会发）。
    session_->enter_ready_for_test();
    trigger_order_event(3);
    session_->pump_query_lane();
    EXPECT_EQ(sent_fund_query_ids_.size(), 2u) << "lane must be reset across disconnect";
}

// ============================================================================
// 登录链后段失败的可观测性 + 结算确认看门狗 (#488)
//
// 为什么放在本目标: 这里是全仓唯一「真 AccountSession + 已注入 notify 回调 + 真 TimerQueue」
// 的夹具。失败出口的承诺是「状态回滚到 LoggedIn 且用户看得见」——两半都必须在真会话上验,
// 只测纯函数 (文案) 证明不了 notify_ 真被调用, 只测状态机 (td_state_test) 证明不了通知
// 被接住 (那正是本票修的缺陷: 5 个调用点丢弃返回值).
//
// 确定性来源: 结算发送经注入钩子 (不碰 api_), 看门狗经 expire_*_for_test 注入同一收口逻辑,
// 以及 0 延迟定时器 + timer_queue_.tick()。不用 sleep。
// ============================================================================

TEST_F(TdPositionLifecycleTest, SettlementConfirmTimeoutRollsBackAndNotifies) {
    // 会话进 Confirming 并发出一笔结算确认（发送经注入钩子，不连柜台）。
    int send_calls = 0;
    session_->set_settlement_send_hook([&](CThostFtdcSettlementInfoConfirmField&, int) {
        ++send_calls;
        return 0;
    });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    ASSERT_EQ(session_->state(), TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    ASSERT_EQ(send_calls, 1) << "the request must really be sent";
    ASSERT_EQ(session_->state(), TdState::Confirming);
    // 看门狗必须已排定, 且是 60s 那一档 (排错定时器/根本没排都会在这里露出来).
    ASSERT_FALSE(timer_queue_.empty()) << "a watchdog must be scheduled right after sending";
    EXPECT_LE(timer_queue_.next_timeout(), kSettlementConfirmTimeout);

    // 柜台不回响应 → 看门狗收口 (生产由 TimerQueue 回调; 这里注入同一收口逻辑, 不 sleep).
    session_->expire_settlement_confirm_timeout_for_test();

    // 状态回滚到 LoggedIn (6/10) —— 那是正常中间态, 所以必须同时有通知, 否则就是静默.
    EXPECT_EQ(session_->state(), TdState::LoggedIn);
    ASSERT_EQ(notifications_.size(), 1u)
        << "a settlement confirm timeout must reach the user (the whole point of the watchdog)";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup) << "a stuck login chain must pop up";
    // 文案逐字锁定在 td_account_session_pure_test (唯一真相源); 这里钉会话确实推了这一条.
    EXPECT_EQ(notifications_.front().message,
              settlement_confirm_failed_message(std::format("{} 秒内未收到柜台的结算单确认响应",
                                                            kSettlementConfirmTimeout.count())));
    // 收口后不留残留定时器: 不自动重试 (#406).
    EXPECT_TRUE(timer_queue_.empty()) << "no timer may outlive the watchdog";
}

TEST_F(TdPositionLifecycleTest, LateSettlementConfirmResponseAfterTimeoutIsIgnored) {
    // 看门狗超时回滚并通知后, 真响应可能才到 (实测该响应有 4–14s 延迟, 60s 看门狗只在
    // 极端丢包时触发)。迟到的成功不能被当成本次登录继续推进 —— 否则会发起一次没有收尾
    // 归属的合约查询, 账户停在一个既非「已通知失败」也非「正常初始化」的中间态。
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    session_->expire_settlement_confirm_timeout_for_test();
    ASSERT_EQ(notifications_.size(), 1u);
    ASSERT_EQ(session_->state(), TdState::LoggedIn);

    // 迟到响应: 用**错误 request_id** 也一并钉住 (只判状态会被旧链接的迟到响应蒙混过关)。
    OnRspSettlementInfoConfirmField late_wrong_id{};
    CThostFtdcRspInfoField ok2{};
    ok2.ErrorID = 0;
    late_wrong_id.rsp_info = ok2;
    late_wrong_id.request_id = 999;
    session_->on_rsp_settlement_confirm(late_wrong_id);
    EXPECT_EQ(session_->state(), TdState::LoggedIn) << "a foreign request id must be dropped too";
    EXPECT_EQ(notifications_.size(), 1u);

    // 迟到的成功响应。
    OnRspSettlementInfoConfirmField late{};
    CThostFtdcRspInfoField ok{};
    ok.ErrorID = 0;
    late.rsp_info = ok;
    late.request_id = 1;
    session_->on_rsp_settlement_confirm(late);

    EXPECT_EQ(session_->state(), TdState::LoggedIn)
        << "a late success must not push the chain into instruments loading";
    EXPECT_EQ(notifications_.size(), 1u) << "a late response must not raise another notification";
    // 没有进入合约查询 (LoadingInstruments) -> 也没有 5 分钟加载定时器.
    EXPECT_TRUE(timer_queue_.empty()) << "a late response must not schedule the instrument query";
}

TEST_F(TdPositionLifecycleTest, SettlementConfirmSendRejectionRollsBackAndNotifies) {
    // 发送被拒 (ret != 0): 此前只记 ERROR、状态停在 LoggedIn、回调永不到来 ——
    // 账户静默停在 6/10。现在必须回滚 (状态已先转 Confirming, 同一个出口) + 弹窗.
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return -1; });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();

    EXPECT_EQ(session_->state(), TdState::LoggedIn) << "a rejected send must roll back to LoggedIn";
    ASSERT_EQ(notifications_.size(), 1u) << "a rejected settlement confirm send must be visible";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_NE(notifications_.front().message.find("发送结算单确认请求失败"), std::string::npos)
        << notifications_.front().message;
    EXPECT_TRUE(timer_queue_.empty()) << "a rejected send must not leave a watchdog armed";
}

TEST_F(TdPositionLifecycleTest, SettlementConfirmFailureResponseRollsBackAndNotifies) {
    // 柜台明确拒绝 (ErrorID != 0): 状态机回滚 + 通知 (此前返回值被丢弃, 失败无痕).
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    ASSERT_EQ(session_->state(), TdState::Confirming);

    OnRspSettlementInfoConfirmField failed{};
    CThostFtdcRspInfoField info{};
    info.ErrorID = 3;
    std::strcpy(info.ErrorMsg, "CTP:结算单确认失败");
    failed.rsp_info = info;
    failed.request_id = 1;
    session_->on_rsp_settlement_confirm(failed);

    EXPECT_EQ(session_->state(), TdState::LoggedIn);
    ASSERT_EQ(notifications_.size(), 1u);
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_NE(notifications_.front().message.find("结算单确认失败（柜台拒绝，错误码 3："),
              std::string::npos)
        << notifications_.front().message;
    EXPECT_TRUE(timer_queue_.empty()) << "the response must cancel the watchdog";
}

TEST_F(TdPositionLifecycleTest, InstrumentsLoadFailureRollsBackAndNotifies) {
    // 合约查询 is_last 带错误: 状态机回滚到 LoggedIn (8/10 退回 6/10「已登录」这个正常
    // 中间态), 返回值此前被丢弃 —— 用户看不到任何失败痕迹.
    session_->advance_login_chain_for_test(TdState::LoadingInstruments);
    ASSERT_EQ(session_->state(), TdState::LoadingInstruments);

    OnRspQryInstrumentField failed{};
    CThostFtdcRspInfoField info{};
    info.ErrorID = 7;
    std::strcpy(info.ErrorMsg, "CTP:未到合约加载时段");
    failed.rsp_info = info;
    failed.is_last = true;
    failed.request_id = 0;  // 夹具未发起查询, 在途 id = 0
    session_->on_rsp_qry_instrument(failed);

    EXPECT_EQ(session_->state(), TdState::LoggedIn);
    ASSERT_EQ(notifications_.size(), 1u) << "an instruments load failure must reach the user";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_NE(notifications_.front().message.find("合约加载失败（柜台拒绝，错误码 7："),
              std::string::npos)
        << notifications_.front().message;
    EXPECT_NE(notifications_.front().message.find("请先登出再重新登录"), std::string::npos);
}

TEST_F(TdPositionLifecycleTest, InstrumentsLoadFailureOutsideLoadingInstrumentsIsSilent) {
    // 状态机拒绝转移 (此刻不在 LoadingInstruments) 时不推送: 弹窗会对不上界面状态
    // (与 #407 / notify_login_failed 同一取舍, 也防止将来把 push 提到 guard 之前).
    ASSERT_EQ(session_->state(), TdState::Idle);
    OnRspQryInstrumentField failed{};
    CThostFtdcRspInfoField info{};
    info.ErrorID = 7;
    std::strcpy(info.ErrorMsg, "CTP:未到合约加载时段");
    failed.rsp_info = info;
    failed.is_last = true;
    failed.request_id = 0;
    session_->on_rsp_qry_instrument(failed);

    EXPECT_TRUE(notifications_.empty());
    EXPECT_EQ(session_->state(), TdState::Idle) << "state machine must not move on a rejected step";
}

TEST_F(TdPositionLifecycleTest, LoginChainFailureWithoutNotifyCallbackDoesNotCrash) {
    // notify_ 为空 (无 UI / 部分测试) 时: 状态照常回滚、不崩、不新增日志噪音。
    auto bare_session = std::make_unique<AccountSession>(
        "acc_bare", *order_id_meta_, *writer_, *persist_, queue_, timer_queue_,
        /*account_state_cb=*/nullptr, /*seq_allocator=*/nullptr,
        /*notify=*/nullptr);
    bare_session->set_trading_day(kDay);
    bare_session->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    bare_session->advance_login_chain_for_test(TdState::LoggedIn);
    bare_session->request_settlement_confirm_for_test();
    ASSERT_EQ(bare_session->state(), TdState::Confirming);

    EXPECT_NO_THROW(bare_session->expire_settlement_confirm_timeout_for_test());
    EXPECT_EQ(bare_session->state(), TdState::LoggedIn) << "state must still roll back";
    EXPECT_TRUE(notifications_.empty()) << "no callback means no notification, not a crash";
}

TEST_F(TdPositionLifecycleTest, WatchdogFiresThroughTimerQueueOnRealDeadline) {
    // 上一条用例走 expire_*_for_test (注入收口)。这一条钉**真回调**：把看门狗时长缩到 0,
    // 让 TimerQueue::tick() 真正执行 req_settlement_confirm 排进去的那个 lambda。
    // 为什么必须有: 若把排定的 lambda 换成空壳 `[]{}`, 只验「定时器排上了」的用例照样绿
    // —— 断言必须能失败 (docs/unit-test-standard.md §1.1)。
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    session_->set_settlement_confirm_timeout_for_test(std::chrono::seconds(0));
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    ASSERT_EQ(session_->state(), TdState::Confirming);

    timer_queue_.tick();  // 到点: 排定的回调此刻执行

    EXPECT_EQ(session_->state(), TdState::LoggedIn)
        << "the queued callback itself must roll the state back";
    ASSERT_EQ(notifications_.size(), 1u) << "the queued callback itself must notify";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_TRUE(timer_queue_.empty());
}

TEST_F(TdPositionLifecycleTest, WatchdogNotFiredEarlyAndCancelledByDisconnect) {
    // 60s 未到不得触发 (证明排的是真延迟而不是 0, 也证明不会误伤正常慢响应)。
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    ASSERT_FALSE(timer_queue_.empty());

    timer_queue_.tick();
    EXPECT_EQ(session_->state(), TdState::Confirming);
    EXPECT_TRUE(notifications_.empty()) << "the watchdog must not fire early";

    // 断线会取消看门狗: 否则 60s 后回调会对着已回滚的会话弹一条对不上状态的告警。
    session_->on_front_disconnected(0);
    EXPECT_TRUE(timer_queue_.empty()) << "disconnect must cancel the armed watchdog";
}

TEST_F(TdPositionLifecycleTest, InstrumentQuerySendRejectionRollsBackAndNotifies) {
    // req_qry_instrument 的 ret != 0 分支 (#488): 此前回退到 LoggedIn 但通知被丢弃.
    session_->set_instrument_send_hook([](CThostFtdcQryInstrumentField&, int) { return -2; });
    session_->advance_login_chain_for_test(TdState::LoadingInstruments);
    session_->request_instrument_query_for_test();

    EXPECT_EQ(session_->state(), TdState::LoggedIn);
    ASSERT_EQ(notifications_.size(), 1u) << "a rejected instrument query must be visible";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_NE(notifications_.front().message.find("发送合约查询请求失败（ret=-2）"),
              std::string::npos)
        << notifications_.front().message;
    // ret = -3 (流控) 不算失败: 排 1.5s 重试, 不通知. 见下一例.
}

TEST_F(TdPositionLifecycleTest, InstrumentQueryFlowControlRetriesWithoutNotification) {
    // -3 是流控不是失败: 必须只排重试、不弹窗、状态留在 LoadingInstruments.
    // 否则一次正常的流控退避会变成用户可见的假失败.
    session_->set_instrument_send_hook([](CThostFtdcQryInstrumentField&, int) { return -3; });
    session_->advance_login_chain_for_test(TdState::LoadingInstruments);
    session_->request_instrument_query_for_test();

    EXPECT_EQ(session_->state(), TdState::LoadingInstruments)
        << "flow control must not roll back the login chain";
    EXPECT_TRUE(notifications_.empty()) << "flow control is not a user-visible failure";
    EXPECT_FALSE(timer_queue_.empty()) << "a retry must be scheduled";
    EXPECT_LE(timer_queue_.next_timeout(), std::chrono::milliseconds(1500));
}

TEST_F(TdPositionLifecycleTest, InstrumentQueryErrorResponseRollsBackAndNotifies) {
    // on_rsp_error 的 Instrument 分支 (#488): 柜台错误响应 -> 回滚 + 弹窗.
    int instrument_request_id = 0;
    session_->set_instrument_send_hook([&](CThostFtdcQryInstrumentField&, int request_id) {
        instrument_request_id = request_id;
        return 0;
    });
    session_->advance_login_chain_for_test(TdState::LoadingInstruments);
    session_->request_instrument_query_for_test();
    ASSERT_EQ(session_->state(), TdState::LoadingInstruments);
    ASSERT_GT(instrument_request_id, 0) << "the hook must observe the real in-flight id";

    OnRspErrorTdField rsp_error{};
    CThostFtdcRspInfoField info{};
    info.ErrorID = 7;
    std::strcpy(info.ErrorMsg, "CTP:查询失败");
    rsp_error.rsp_info = info;
    // request_id 必须等于在途合约查询 id, 否则会被当作陈旧错误丢弃 (那是另一条分支)。
    // 从发送钩子捕获实际 id, 不写死 (夹具的 request_id_ 起点变了就会静默走错分支)。
    rsp_error.request_id = instrument_request_id;
    session_->on_rsp_error(rsp_error);

    EXPECT_EQ(session_->state(), TdState::LoggedIn);
    ASSERT_EQ(notifications_.size(), 1u) << "an instrument error response must be visible";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    EXPECT_NE(notifications_.front().message.find("柜台返回错误，错误码 7："), std::string::npos)
        << notifications_.front().message;
}

TEST_F(TdPositionLifecycleTest, InstrumentsLoadTimeoutFiresThroughTimerQueueAndNotifies) {
    // 合约加载 5 分钟超时出口 (#488 的第 7 个调用点): 此前只排了定时器并回滚, 通知被丢弃。
    // 缩时到 0 后用真 TimerQueue::tick() 执行排定的回调本身 —— 只验「排上了定时器」的
    // 用例在 lambda 被换成空壳时照样绿 (docs/unit-test-standard.md §1.1)。
    session_->set_instrument_send_hook([](CThostFtdcQryInstrumentField&, int) { return 0; });
    session_->set_instruments_load_timeout_for_test(std::chrono::seconds(0));
    session_->advance_login_chain_for_test(TdState::LoadingInstruments);
    session_->request_instrument_query_for_test();
    // 发送被接受 -> 查询在途, 等待 is_last; 超时定时器已武装。
    ASSERT_EQ(session_->state(), TdState::LoadingInstruments);
    ASSERT_FALSE(timer_queue_.empty());

    timer_queue_.tick();  // 到点: 排定的超时回调此刻执行

    EXPECT_EQ(session_->state(), TdState::LoggedIn)
        << "the queued timeout callback itself must roll the state back";
    ASSERT_EQ(notifications_.size(), 1u) << "the instruments load timeout must reach the user";
    EXPECT_EQ(notifications_.front().level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(notifications_.front().popup);
    // 正文里的秒数随实际配置 (测试缩成 0 秒), 证明文案拿的是同一个时长源.
    EXPECT_NE(notifications_.front().message.find("查询 0 秒未收到最后一批响应"), std::string::npos)
        << notifications_.front().message;
    EXPECT_TRUE(timer_queue_.empty());
}

TEST_F(TdPositionLifecycleTest, WrongRequestIdResponseWhileConfirmingIsDropped) {
    // #488: 迟到响应的判据是「状态 + request_id」双校验。只判状态的实现在这里会绿——
    // 本条用例在**仍处于 Confirming** 时喂一个陌生 request_id, 必须被丢弃:
    // 状态不动、不弹窗、看门狗仍武装 (后续真响应才收口)。
    session_->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    session_->advance_login_chain_for_test(TdState::LoggedIn);
    session_->request_settlement_confirm_for_test();
    ASSERT_EQ(session_->state(), TdState::Confirming);

    OnRspSettlementInfoConfirmField foreign{};
    CThostFtdcRspInfoField ok{};
    ok.ErrorID = 0;
    foreign.rsp_info = ok;
    foreign.request_id = 999;  // 不是本链的在途 id
    session_->on_rsp_settlement_confirm(foreign);

    EXPECT_EQ(session_->state(), TdState::Confirming)
        << "a foreign request id must not advance the chain";
    EXPECT_TRUE(notifications_.empty()) << "a foreign request id must not notify";
    // 关键的一条: 被丢弃的响应绝不能顺手把本链的看门狗撤掉 —— 否则会话留在 Confirming
    // 却再无任何回调, 正是本票要消灭的静默卡死形态 (cancel 若排在守卫之前就会发生)。
    EXPECT_FALSE(timer_queue_.empty())
        << "a dropped response must leave the watchdog armed for the real one";
    EXPECT_LE(timer_queue_.next_timeout(), kSettlementConfirmTimeout);
}

TEST_F(TdPositionLifecycleTest, NotifyThrowIsCaughtAndStateStillRollsBack) {
    // AC7 后半: notify_ 抛异常必须被 catch (不逃逸到 TdApi 主循环), 且状态照常回滚.
    // 用独立的会抛 notify 的会话 —— 本 fixture 的 notifications_ 不可抛.
    auto throwing = std::make_unique<AccountSession>(
        "acc_throw", *order_id_meta_, *writer_, *persist_, queue_, timer_queue_,
        /*account_state_cb=*/nullptr, /*seq_allocator=*/nullptr,
        [](DzNotifyLevel, const std::string&, bool) { throw std::runtime_error("notify boom"); });
    throwing->set_trading_day(kDay);
    throwing->set_settlement_send_hook(
        [](CThostFtdcSettlementInfoConfirmField&, int) { return 0; });
    throwing->advance_login_chain_for_test(TdState::LoggedIn);
    throwing->request_settlement_confirm_for_test();
    ASSERT_EQ(throwing->state(), TdState::Confirming);

    EXPECT_NO_THROW(throwing->expire_settlement_confirm_timeout_for_test())
        << "a throwing notify callback must not escape into the main loop";
    EXPECT_EQ(throwing->state(), TdState::LoggedIn)
        << "the state must still roll back even when the notification path throws";
}

}  // namespace
}  // namespace dztrader::ctp
