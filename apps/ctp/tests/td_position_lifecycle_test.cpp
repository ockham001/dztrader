/**
 * @file td_position_lifecycle_test.cpp
 * @brief 持仓快照生命周期的**会话级**行为（契约 td-data-sync §3/§7、ADR 0045/ADR 0046）。
 *
 * 为什么需要这个目标（与 `td_persist_writer_test` / `td_data_sync_integration_test` 的分工）：
 * 那两个目标只链 `PersistWriter` / `ReportFilter` / `DayLoad`，**没有** `AccountSession`；
 * 而本票 AC 的核心（查询失败不改表、完整成功查询整组替换、已就绪周期查询无差异不重建、
 * 日切只碰当日行）的决定点在
 * `AccountSession::on_rsp_qry_investor_position → apply_position_query` 里。在只有 Writer
 * 的目标里只能写一个「镜像生产顺序」的替身，断言会恒真——属
 * `docs/unit-test-standard.md` §1.1/§1.2 判定的失效测试形态。
 *
 * 因此本文件直连生产会话：真 SQLite 临时库 + 真 `PersistWriter` 线程 + 真 `AccountSession`，
 * 把 TdApi 从事件队列拿到的 `OnRspQryInvestorPositionField` **直接**喂给会话的公开 SPI 入口。
 *
 * **边界（诚实声明）**：本目标不覆盖「登录链本身」（合约加载 → 收尾 → Ready）——那条链在
 * `req_settlement_confirm` 里无条件解引用 `api_`，而 `api_` 只能由 `open()` 创建真实
 * `CThostFtdcTraderApi`（需连柜台）。因此这里的会话处于**未登录**状态，只驱动持仓查询
 * 响应分支——那正是本票所有持仓写入决策所在。登录链的 flush/Ready 语义由
 * `td_login_finalize_test`（纯逻辑）与 `td_data_sync_integration_test`（Writer 时序）覆盖。
 *
 * 确定性来源：每次断言前用 persist flush 屏障等末批提交（`wait_flush`），不用 sleep。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/timer_queue.h>
#include <dztrader/date_time/date.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/tdstore/trade_db.h>

#include "common/ctp_events.h"
#include "td/td_account_session.h"
#include "td/td_events.h"
#include "td/td_persist_writer.h"
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
    std::unique_ptr<shm::MultiWriter> writer_;
    std::unique_ptr<shm::OrderIdMeta> order_id_meta_;
    std::unique_ptr<PersistWriter> persist_;
    dztrader::core::TimerQueue timer_queue_;
    MpmcQueuePtr queue_;
    std::unique_ptr<AccountSession> session_;

    static constexpr int32_t kDay = 20735;  ///< 20261009（下面 SetUp 用真实日期派生校验）

    void SetUp() override {
        ASSERT_EQ(kDay, day(2026, 10, 9)) << "kDay constant drifted from 2026-10-09";
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

        session_ = std::make_unique<AccountSession>("acc1", *order_id_meta_, *writer_, *persist_,
                                                    queue_, timer_queue_);
        // 交易日固定，避免断言随真实当天漂移（会话用它填 DzPositionInfo.date）。
        session_->set_trading_day(kDay);
    }

    void TearDown() override {
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

    /// 一条持仓查询响应行（`volume` 含昨仓；`cost` 为持仓成本金额）。
    static OnRspQryInvestorPositionField position_row(
        const std::string& instrument, int32_t volume, int32_t yd, double cost, bool is_last) {
        OnRspQryInvestorPositionField f{};
        CThostFtdcInvestorPositionField p{};
        std::strcpy(p.InstrumentID, instrument.c_str());
        std::strcpy(p.ExchangeID, "CFFEX");
        std::strcpy(p.InvestorID, "acc1");
        p.Position = volume;
        p.YdPosition = yd;
        p.PositionCost = cost;
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
                                                           double available = 85000.0) {
        OnRspQryTradingAccountField f{};
        CThostFtdcTradingAccountField a{};
        std::strcpy(a.AccountID, "acc1");
        a.Balance = balance;
        a.Available = available;
        a.CurrMargin = 15000.0;
        f.trading_account = a;
        f.is_last = is_last;
        f.request_id = 0;
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
    raw_exec(
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction, volume,"
        " frozen_volume, today_volume, yd_volume, price, seq, gateway)"
        " VALUES ('acc1','20261008','IF2506','CFFEX',1,5,0,5,0,3900.0,10,'dztd_test')");

    // 生产时序：持仓查询失败 → 收尾链照常发起资金查询 → 资金响应成功到达。
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
    session_ = std::make_unique<AccountSession>("acc1", *order_id_meta_, *writer_, *persist_,
                                                queue_, timer_queue_);
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

    // 同一交易日再次成功查询（重传 / 失败补查链恢复后的首轮）：同键 (account_id,date) upsert，
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

TEST_F(TdPositionLifecycleTest, DaySwitchFundQueriesCoexistAsTwoDayRows) {
    // 交易日 D1：持仓 + 资金写入 D1 行。
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 100000.0, 85000.0));
    flush_persist();
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE date='20261009'"), 1);
    const int64_t day1_seq = scalar_int("SELECT seq FROM trading_accounts WHERE date='20261009'");

    // 日切 → 重连/新登录链：新会话（生产上 = 交易日推进后的重新登录）写 D2 行。
    session_.reset();
    session_ = std::make_unique<AccountSession>("acc1", *order_id_meta_, *writer_, *persist_,
                                                queue_, timer_queue_);
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
    session_ = std::make_unique<AccountSession>("acc1", *order_id_meta_, *writer_, *persist_,
                                                queue_, timer_queue_);
    session_->set_trading_day(kDay);
    feed_position_snapshot({position_row("IF2506", 2, 1, 2.0 * 300.0 * 3900.0, /*is_last=*/true)});
    session_->on_rsp_qry_trading_account(trading_account_row(/*is_last=*/true, 99000.0, 84000.0));
    flush_persist();

    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);
    EXPECT_EQ(
        scalar_int("SELECT CAST(balance AS INTEGER) FROM trading_accounts WHERE date='20261009'"),
        99000);
}

}  // namespace
}  // namespace dztrader::ctp
