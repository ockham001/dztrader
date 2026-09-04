#include "td/td_persist_writer.h"

#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Transaction.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <dztrader/db/connection.h>
#include <dztrader/db/migration.h>
#include <dztrader/date_time/date_time.h>  // Date (DzDate 距纪元天数 -> YYYYMMDD)

namespace dztrader::ctp {

// ============================================================================
// INSERT OR REPLACE SQL (RESTART 重传去重, 最新状态覆盖旧记录)
// 字段名与 td_schema.cpp CREATE TABLE 一致:
// - exchange_id (不是 exchange)
// - volume (不是 volume_total, 与 DzOrderReport.volume 一致)
// - date INTEGER (margin_rates/commission_rates, 不是 trading_day TEXT)
// - orders/trades 增加 strategy_id, remark 列 (来自 DzOrderReport/DzTradeReport)
// ============================================================================

namespace {

// 注意: stmt 索引从 1 开始
constexpr const char* kInsertOrderSql =
    "INSERT OR REPLACE INTO orders ("
    "    account_id, trading_day, order_id, order_ref, external_order_id,"
    "    is_external, instrument_id, exchange_id, direction, position_effect,"
    "    price_type, status, price, volume, volume_traded, volume_canceled,"
    "    insert_time, update_time, error_id, error_msg, strategy_id, remark, seq"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,  ?,?,?,?,?,?,  ?,?,?,?, ?,?,?)";

constexpr const char* kInsertTradeSql =
    "INSERT OR REPLACE INTO trades ("
    "    account_id, trading_day, trade_id, order_id, instrument_id, exchange_id,"
    "    direction, position_effect, price, volume, trade_time, trade_date, commission,"
    "    strategy_id, seq"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,  ?,?,?,?, ?)";

constexpr const char* kInsertMarginRateSql =
    "INSERT OR REPLACE INTO margin_rates ("
    "    account_id, instrument_id, product_code, exchange_id,"
    "    hedge_flag, is_relative, long_margin_ratio_by_money, long_margin_ratio_by_volume,"
    "    short_margin_ratio_by_money, short_margin_ratio_by_volume, date"
    ") VALUES (?,?,?,?,  ?,?,?,?,?,?, ?)";

constexpr const char* kInsertCommissionRateSql =
    "INSERT OR REPLACE INTO commission_rates ("
    "    account_id, instrument_id, product_code, exchange_id,"
    "    open_ratio_by_money, open_ratio_by_volume, close_ratio_by_money, close_ratio_by_volume,"
    "    close_today_ratio_by_money, close_today_ratio_by_volume, date"
    ") VALUES (?,?,?,?,  ?,?,?,?,?,?, ?)";

constexpr const char* kInsertInstrumentSql =
    "INSERT OR REPLACE INTO instruments ("
    "    instrument_id, exchange_id, symbol, name, product, settle_cycle,"
    "    settlement_method, is_inverse, currency, base_asset, min_order_volume,"
    "    max_order_volume, volume_multiple, price_tick, volume_step, listed_date,"
    "    expiry_date, option_type, option_exercise_style, underlying_id, option_strike,"
    "    option_series, update_day"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,?,  ?,?,?,?,?,?,  ?,?,?,?,?,?)";

// positions (spec §3.2): key = (account_id, instrument_id, direction), 绝对态 upsert
constexpr const char* kInsertPositionSql =
    "INSERT OR REPLACE INTO positions ("
    "    account_id, trading_day, instrument_id, exchange_id, direction,"
    "    volume, frozen_volume, today_volume, yd_volume, price, seq"
    ") VALUES (?,?,?,?,?,  ?,?,?,?,?, ?)";

// trading_accounts: key = account_id, 绝对态 upsert
constexpr const char* kInsertTradingAccountSql =
    "INSERT OR REPLACE INTO trading_accounts ("
    "    account_id, trading_day, balance, available, frozen, commission,"
    "    margin, withdraw_quota, deposit, withdraw, seq"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?, ?)";

// PositionRebuild 单事务重灌: 清该账户全部持仓行 (spec §3.2 全量语义原子切换)
constexpr const char* kDeletePositionRebuildSql =
    "DELETE FROM positions WHERE account_id=?";

}  // namespace

// ============================================================================
// PersistWriter 实现
// ============================================================================

PersistWriter::PersistWriter(std::string db_path, size_t max_queue_size)
    : db_path_(std::move(db_path)), max_queue_size_(max_queue_size) {}

PersistWriter::~PersistWriter() {
    // I4: 析构仅作 best-effort 兜底 (短超时 1s + 不 quick_exit)
    // 主流程必须显式调 stop() (30s 超时 + quick_exit 兜底)
    if (explicit_stopped_) {
        return;  // 已显式 stop, 资源已释放
    }
    if (writer_thread_.joinable()) {
        SPDLOG_WARN("persist writer destroyed without explicit stop, best-effort cleanup");
        try {
            stop_best_effort();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist writer best-effort stop failed | error=\"{}\"", e.what());
        }
    }
}

void PersistWriter::open() {
    if (opened_.load()) {
        throw std::runtime_error("persist writer already opened");
    }

    SPDLOG_INFO("opening database | path={}", db_path_);

    // 创建 Connection 并应用 migration
    db_ = std::make_unique<SQLite::Database>(db_path_,
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

    // PRAGMA 配置 (与 libs/db/connection.cpp 一致, DELETE + synchronous=FULL)
    db_->exec("PRAGMA synchronous=FULL");
    db_->exec("PRAGMA busy_timeout=5000");
    db_->exec("PRAGMA cache_size=-8000");
    db_->exec("PRAGMA temp_store=MEMORY");

    // 应用 TD migration (v1 创建所有表)
    dztrader::db::MigrationManager mgr;
    apply_td_migrations(mgr);
    auto applied = mgr.apply(*db_);
    for (int v : applied) {
        SPDLOG_INFO("td migration applied | version={}", v);
    }

    // 预编译 INSERT 语句 (复用, 避免每次 prepare)
    prepare_statements(*db_);

    opened_ = true;
    SPDLOG_INFO("database opened | path={} applied_versions={}", db_path_, applied.size());
}

void PersistWriter::prepare_statements(SQLite::Database& db) {
    stmt_insert_order_ = std::make_unique<SQLite::Statement>(db, kInsertOrderSql);
    stmt_insert_trade_ = std::make_unique<SQLite::Statement>(db, kInsertTradeSql);
    stmt_insert_margin_ = std::make_unique<SQLite::Statement>(db, kInsertMarginRateSql);
    stmt_insert_commission_ = std::make_unique<SQLite::Statement>(db, kInsertCommissionRateSql);
    stmt_insert_instrument_ = std::make_unique<SQLite::Statement>(db, kInsertInstrumentSql);
    stmt_insert_position_ = std::make_unique<SQLite::Statement>(db, kInsertPositionSql);
    stmt_insert_taccount_ = std::make_unique<SQLite::Statement>(db, kInsertTradingAccountSql);
    stmt_delete_position_rebuild_ =
        std::make_unique<SQLite::Statement>(db, kDeletePositionRebuildSql);
}

void PersistWriter::start_writer() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (writer_started_) {
        SPDLOG_WARN("start_writer called twice, ignoring");
        return;
    }
    if (!opened_.load()) {
        throw std::runtime_error("must call open() before start_writer()");
    }
    running_ = true;
    writer_exited_ = false;
    writer_started_ = true;
    writer_thread_ = std::thread([this] { writer_loop(); });
    SPDLOG_INFO("persist writer thread started");
}

void PersistWriter::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!writer_started_) {
            // 未启动 Writer, 直接关闭 db
            explicit_stopped_ = true;
            return;
        }
        running_ = false;
        shutdown_ = true;
    }
    cv_empty_.notify_all();  // 唤醒 Writer
    cv_full_.notify_all();   // 唤醒阻塞的 enqueue

    // 等待 Writer 退出, 30s 超时
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_writer_done_.wait_for(lk, kShutdownTimeout, [this] { return writer_exited_; })) {
        SPDLOG_ERROR("persist writer shutdown timeout, calling quick_exit | queue_size={}",
                     queue_.size());
        std::quick_exit(1);  // 跳过析构, OS 回收资源 (设计 §13.11)
    }
    lk.unlock();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    // 关闭预编译 stmt + 数据库 (Writer 已退出, 无竞争)
    stmt_insert_order_.reset();
    stmt_insert_trade_.reset();
    stmt_insert_margin_.reset();
    stmt_insert_commission_.reset();
    stmt_insert_instrument_.reset();
    stmt_insert_position_.reset();
    stmt_insert_taccount_.reset();
    stmt_delete_position_rebuild_.reset();
    db_.reset();

    {
        std::lock_guard<std::mutex> lk2(mtx_);
        writer_started_ = false;
    }
    explicit_stopped_ = true;  // I4: 标记已显式 stop, 析构 no-op
    SPDLOG_INFO("persist writer stopped");
}

void PersistWriter::stop_best_effort() {
    // I4: 析构兜底路径, 短超时 (1s) + 不 quick_exit
    // 与 stop() 区别: 超时仅记 ERROR, 不调 quick_exit (避免析构栈中跳过其他析构)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!writer_started_) {
            explicit_stopped_ = true;
            return;
        }
        running_ = false;
        shutdown_ = true;
    }
    cv_empty_.notify_all();
    cv_full_.notify_all();

    std::unique_lock<std::mutex> lk(mtx_);
    constexpr auto kBestEffortTimeout = std::chrono::seconds(1);
    if (!cv_writer_done_.wait_for(lk, kBestEffortTimeout, [this] { return writer_exited_; })) {
        SPDLOG_ERROR("persist writer best-effort stop timeout (detached) | queue_size={}",
                     queue_.size());
        // 不 quick_exit: detach 线程, 让 OS 在进程退出时回收
        // 注意: 此处 db_/stmt 不 reset (线程可能仍在用), 接受泄漏
        if (writer_thread_.joinable()) {
            writer_thread_.detach();
        }
        explicit_stopped_ = true;
        return;
    }
    lk.unlock();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    stmt_insert_order_.reset();
    stmt_insert_trade_.reset();
    stmt_insert_margin_.reset();
    stmt_insert_commission_.reset();
    stmt_insert_instrument_.reset();
    stmt_insert_position_.reset();
    stmt_insert_taccount_.reset();
    stmt_delete_position_rebuild_.reset();
    db_.reset();

    {
        std::lock_guard<std::mutex> lk2(mtx_);
        writer_started_ = false;
    }
    explicit_stopped_ = true;
    SPDLOG_INFO("persist writer stopped (best-effort)");
}

void PersistWriter::enqueue(PersistTask task) {
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_full_.wait(lk, [this] { return queue_.size() < max_queue_size_ || shutdown_.load(); });
        if (shutdown_.load()) {
            // stop() 已调用, 丢弃 (区别于 running_=false 的未启动状态).
            // FlushSignal 丢弃分支须立即 set_value: 丢弃 = 队列已无此前任务,
            // wait_flush 恒返回 true (哨兵被"消费"即视为提交完成语义), 避免死 token.
            if (task.kind == PersistTask::Kind::FlushSignal) {
                auto token = next_flush_token_++;
                pending_flushes_[token].set_value();
                pending_flushes_.erase(token);
            }
            return;
        }
        queue_.push(std::move(task));
    }
    cv_empty_.notify_one();
}

uint64_t PersistWriter::enqueue_flush_signal() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (shutdown_.load()) {
        // stop() 后: 丢弃路径语义, 返回一个"已消费"哨兵 token (wait_flush 恒 true).
        auto token = next_flush_token_++;
        pending_flushes_[token].set_value();
        pending_flushes_.erase(token);
        return token;
    }
    auto token = next_flush_token_++;
    pending_flushes_.emplace(token, std::promise<void>{});
    queue_.push(PersistTask{.kind = PersistTask::Kind::FlushSignal,
                            .account_id = "",
                            .flush_token = token});
    lk.unlock();
    cv_empty_.notify_one();
    return token;
}

bool PersistWriter::wait_flush(uint64_t token, std::chrono::milliseconds timeout) {
    std::future<void> fut;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = pending_flushes_.find(token);
        if (it == pending_flushes_.end()) {
            // 未知 token: 视为已消费 (stop 丢弃路径返回的 token 或非法值)
            return true;
        }
        fut = it->second.get_future();
    }
    // promise 在 Writer 线程 set_value / set_exception (批失败, 终检发现 F), 或
    // stop 丢弃路径 set_value. set_exception 时 wait_for 仍返回 ready, get() 抛异常 —
    // 捕获并返回 false (失败场景"此前任务未完成提交", 与超时同语义).
    // 超时返回 false.
    if (fut.wait_for(timeout) != std::future_status::ready) {
        return false;
    }
    try {
        fut.get();  // 传播批失败异常 (set_exception) — 捕获转 false, 不逃逸给调用方
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
    return true;
}

SQLite::Database& PersistWriter::db() {
    if (!opened_.load()) {
        throw std::runtime_error("must call open() before db()");
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (writer_started_) {
        throw std::runtime_error("db() not available after start_writer() (writer thread owns db)");
    }
    return *db_;
}

int64_t PersistWriter::max_order_id() {
    // 调用时机与 db() 相同: open() 后 start_writer() 前 (主线程独占 db, 无竞争)
    auto& db = this->db();
    int64_t result = 0;
    SQLite::Statement q_orders(db, "SELECT COALESCE(MAX(order_id), 0) FROM orders");
    if (q_orders.executeStep()) {
        result = q_orders.getColumn(0).getInt64();
    }
    SQLite::Statement q_trades(db, "SELECT COALESCE(MAX(order_id), 0) FROM trades");
    if (q_trades.executeStep()) {
        result = std::max(result, q_trades.getColumn(0).getInt64());
    }
    return result;
}

// ============================================================================
// Writer 线程主循环 (纯事件驱动, 无轮询)
// ============================================================================

void PersistWriter::writer_loop() {
    SPDLOG_INFO("writer loop started");

    while (true) {
        PersistTask task;
        if (!wait_and_pop(task)) {
            break;  // running_=false 且队列空
        }

        // drain 批量: 取出当前队列全部, 减少锁竞争
        std::vector<PersistTask> batch;
        batch.push_back(std::move(task));
        {
            std::lock_guard<std::mutex> lk(mtx_);
            while (!queue_.empty()) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }
        cv_full_.notify_all();  // 队列已清空, 唤醒阻塞的 enqueue

        // 单事务批量提交 (失败不退出, 丢弃本批, 记 ERROR)
        // C7: 必须捕获所有异常 (含非 std 异常), 否则 Writer 线程退出会导致 enqueue 死锁
        // 终检发现 F【Important】: 批失败时已入批的 flush 哨兵 token 既不 set_value
        // 也不 erase → wait_flush 永久超时 + map 慢性泄漏 (7×24 周期重登累积)。
        // 关键: 批执行中途抛异常时, execute_batch 在抛点之前收集的 flushed_tokens 可能
        // 不含抛点之后/未执行的哨兵 token — 故 catch 分支以"本批全部 FlushSignal token"
        // 为集合 (预扫描 batch), 对每个 set_exception + erase: wait_flush 捕获异常返回
        // false, 语义正确 — 失败场景下批事务已回滚, "此前任务未完成提交"须以 false 显式表达。
        // (注释: set_value 仅在批 commit 成功后执行, 保证 wait_flush 返回 true ⇒ 已提交。
        //  既有 FlushReturnsFalseWhenBatchCommitFails 测试的"超时不 set"语义升级为
        //  "显式 set_exception" — 同一不变量: 失败必 false, 且不再泄漏/饿死等待者。)
        std::vector<uint64_t> flushed_tokens;
        try {
            SQLite::Transaction txn(*db_);
            execute_batch(*db_, batch, flushed_tokens);
            txn.commit();
            // 评审 C1: 批事务提交成功后才 set_value, 保证 "wait_flush 返回" ⇒
            // "此前任务必已提交/fsync". 提交/批执行失败时 (走 catch) 不 set,
            // waiter 超时获知失败, 不会在数据实际被回滚时误报成功.
            if (!flushed_tokens.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                for (uint64_t token : flushed_tokens) {
                    auto it = pending_flushes_.find(token);
                    if (it != pending_flushes_.end()) {
                        it->second.set_value();
                        pending_flushes_.erase(it);
                    }
                }
            }
            SPDLOG_DEBUG("persist batch committed | count={}", batch.size());
        } catch (const SQLite::Exception& e) {
            SPDLOG_ERROR("persist batch failed, dropping | count={} error=\"{}\"",
                         batch.size(), e.what());
            fail_flushed_tokens(batch);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist batch failed (std), dropping | count={} error=\"{}\"",
                         batch.size(), e.what());
            fail_flushed_tokens(batch);
        } catch (...) {
            // 非 std 异常 (如 SQLite C 接口错误回调 throw int) 也必须吞下,
            // 防止 Writer 线程意外退出导致主线程 enqueue 永久阻塞 (设计 §13.11)
            SPDLOG_ERROR("persist batch failed (unknown), dropping | count={}", batch.size());
            fail_flushed_tokens(batch);
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        writer_exited_ = true;
    }
    cv_writer_done_.notify_one();
    SPDLOG_INFO("writer loop exited");
}

/// 批事务失败收尾: 本批全部 FlushSignal token 的 promise 必须终结 (set_exception), 否则
/// wait_flush 永久阻塞/超时 + pending_flushes_ 条目永不回收 (终检发现 F 慢性泄漏)。
/// set_exception 使 wait_flush 的 future 立即 ready (异常), 调用方捕获后返回 false —
/// 失败场景"任务未完成提交"语义, 与 stop 丢弃路径的"已消费"语义区分。
/// 注意: 批执行中途抛异常时 execute_batch 收集的 flushed_tokens 可能不含抛点之后的哨兵,
/// 故以 batch 全量预扫描为 token 集合 (批内每个 FlushSignal 都是本批的等待者)。
void PersistWriter::fail_flushed_tokens(const std::vector<PersistTask>& batch) {
    std::vector<uint64_t> tokens;
    for (const auto& task : batch) {
        if (task.kind == PersistTask::Kind::FlushSignal) {
            tokens.push_back(task.flush_token);
        }
    }
    if (tokens.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    for (uint64_t token : tokens) {
        auto it = pending_flushes_.find(token);
        if (it != pending_flushes_.end()) {
            try {
                it->second.set_exception(
                    std::make_exception_ptr(std::runtime_error("persist batch commit failed")));
            } catch (...) {
                // 防御: promise 已 set (理论不可达, 单 Writer 线程串行)
            }
            pending_flushes_.erase(it);
        }
    }
}

bool PersistWriter::wait_and_pop(PersistTask& out) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_empty_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
    // 仅在 队列空 且 running_=false 时退出 (drain 残留优先)
    if (queue_.empty() && !running_.load()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop();
    cv_full_.notify_one();
    return true;
}

void PersistWriter::execute_batch(SQLite::Database& db, std::vector<PersistTask>& batch,
                                  std::vector<uint64_t>& flushed_tokens) {
    (void)db;  // 预留: 事务/批处理优化走同一个连接
    for (auto& task : batch) {
        switch (task.kind) {
            case PersistTask::Kind::Order:
                stmt_insert_order_->reset();
                bind_order(*stmt_insert_order_, std::get<OrderRecord>(task.data));
                stmt_insert_order_->exec();
                break;
            case PersistTask::Kind::Trade:
                stmt_insert_trade_->reset();
                bind_trade(*stmt_insert_trade_, std::get<TradeRecord>(task.data));
                stmt_insert_trade_->exec();
                break;
            case PersistTask::Kind::MarginRate:
                stmt_insert_margin_->reset();
                bind_margin_rate(*stmt_insert_margin_, std::get<MarginRateRecord>(task.data));
                stmt_insert_margin_->exec();
                break;
            case PersistTask::Kind::CommissionRate:
                stmt_insert_commission_->reset();
                bind_commission_rate(*stmt_insert_commission_,
                                     std::get<CommissionRateRecord>(task.data));
                stmt_insert_commission_->exec();
                break;
            case PersistTask::Kind::Instrument:
                stmt_insert_instrument_->reset();
                bind_instrument(*stmt_insert_instrument_, std::get<InstrumentRecord>(task.data));
                stmt_insert_instrument_->exec();
                break;
            case PersistTask::Kind::Position: {
                // 单行绝对态 upsert (盘中有变化时走它). task.trading_day 是当前交易日.
                const auto& row = std::get<std::vector<DzPositionInfo>>(task.data).front();
                stmt_insert_position_->reset();
                bind_position(*stmt_insert_position_, row, format_trading_day(task.trading_day));
                stmt_insert_position_->exec();
                break;
            }
            case PersistTask::Kind::TradingAccount: {
                const auto& row = std::get<DzTradingAccount>(task.data);
                stmt_insert_taccount_->reset();
                bind_trading_account(*stmt_insert_taccount_, row,
                                     format_trading_day(task.trading_day));
                stmt_insert_taccount_->exec();
                break;
            }
            case PersistTask::Kind::PositionRebuild: {
                // 单事务重灌 (spec §3.2 全量语义): 清该账户全部持仓行 + upsert 本组行,
                // 外部读者只见原子切换. 外层 writer_loop 已有事务, 此处复用.
                // 删全部行 (而非按 trading_day 排除): 查询响应为全量, 响应不含的
                // 合约 = 已全平/已过期, 必须删除 (否则盘中平仓的幽灵持仓永驻到次日).
                auto day = format_trading_day(task.trading_day);
                stmt_delete_position_rebuild_->reset();
                stmt_delete_position_rebuild_->bind(1, task.account_id);
                stmt_delete_position_rebuild_->exec();
                for (const auto& row : std::get<std::vector<DzPositionInfo>>(task.data)) {
                    stmt_insert_position_->reset();
                    bind_position(*stmt_insert_position_, row, day);
                    stmt_insert_position_->exec();
                }
                break;
            }
            case PersistTask::Kind::FlushSignal: {
                // FIFO 哨兵: 收集 token, 由 writer_loop 在批事务 commit 成功后统一
                // set_value. 保证 "wait_flush 返回" ⇒ "此前任务必已提交"
                // (评审 C1: set 必须在 commit 之后, 不得在此处提前 set).
                flushed_tokens.push_back(task.flush_token);
                break;
            }
        }
    }
}

// ============================================================================
// bind 函数: POD 字段 -> SQLite 参数 (索引从 1 开始)
// 组合方式: r.base.xxx 访问 strategy_api 结构体字段, r.xxx 访问 SQL 扩展字段
// ============================================================================

void PersistWriter::bind_order(SQLite::Statement& stmt, const OrderRecord& r) {
    stmt.bind(1, r.base.account_id);
    stmt.bind(2, r.trading_day);
    stmt.bind(3, r.base.order_id);
    stmt.bind(4, r.order_ref);
    stmt.bind(5, r.external_order_id);
    stmt.bind(6, static_cast<int>(r.is_external));
    stmt.bind(7, r.base.instrument_id);
    stmt.bind(8, r.base.exchange_id);
    stmt.bind(9, static_cast<int>(r.base.direction));
    stmt.bind(10, static_cast<int>(r.base.position_effect));
    stmt.bind(11, static_cast<int>(r.base.price_type));
    stmt.bind(12, static_cast<int>(r.base.status));
    stmt.bind(13, r.base.price);
    stmt.bind(14, r.base.volume);
    stmt.bind(15, r.base.volume_traded);
    stmt.bind(16, r.volume_canceled);
    stmt.bind(17, r.insert_time);
    stmt.bind(18, r.update_time);
    stmt.bind(19, r.error_id);
    stmt.bind(20, r.error_msg);
    stmt.bind(21, r.base.strategy_id);
    stmt.bind(22, r.base.remark);
    stmt.bind(23, static_cast<int64_t>(r.base.seq));
}

void PersistWriter::bind_trade(SQLite::Statement& stmt, const TradeRecord& r) {
    stmt.bind(1, r.base.account_id);
    stmt.bind(2, r.trading_day);
    stmt.bind(3, r.base.trade_id);
    stmt.bind(4, r.base.order_id);
    stmt.bind(5, r.base.instrument_id);
    stmt.bind(6, r.base.exchange_id);
    stmt.bind(7, static_cast<int>(r.base.direction));
    stmt.bind(8, static_cast<int>(r.base.position_effect));
    stmt.bind(9, r.base.price);
    stmt.bind(10, r.base.volume);
    stmt.bind(11, r.trade_time);
    stmt.bind(12, r.trade_date);
    stmt.bind(13, r.commission);
    stmt.bind(14, r.base.strategy_id);
    stmt.bind(15, static_cast<int64_t>(r.base.seq));
}

void PersistWriter::bind_margin_rate(SQLite::Statement& stmt, const MarginRateRecord& r) {
    stmt.bind(1, r.account_id);
    stmt.bind(2, r.instrument_id);
    stmt.bind(3, r.product_code);
    stmt.bind(4, r.exchange_id);
    stmt.bind(5, static_cast<int>(r.hedge_flag));
    stmt.bind(6, static_cast<int>(r.is_relative));
    stmt.bind(7, r.long_margin_ratio_by_money);
    stmt.bind(8, r.long_margin_ratio_by_volume);
    stmt.bind(9, r.short_margin_ratio_by_money);
    stmt.bind(10, r.short_margin_ratio_by_volume);
    stmt.bind(11, r.date);
}

void PersistWriter::bind_commission_rate(SQLite::Statement& stmt, const CommissionRateRecord& r) {
    stmt.bind(1, r.account_id);
    stmt.bind(2, r.instrument_id);
    stmt.bind(3, r.product_code);
    stmt.bind(4, r.exchange_id);
    stmt.bind(5, r.open_ratio_by_money);
    stmt.bind(6, r.open_ratio_by_volume);
    stmt.bind(7, r.close_ratio_by_money);
    stmt.bind(8, r.close_ratio_by_volume);
    stmt.bind(9, r.close_today_ratio_by_money);
    stmt.bind(10, r.close_today_ratio_by_volume);
    stmt.bind(11, r.date);
}

void PersistWriter::bind_instrument(SQLite::Statement& stmt, const InstrumentRecord& r) {
    stmt.bind(1, r.base.instrument_id);
    stmt.bind(2, r.base.exchange_id);
    stmt.bind(3, r.base.symbol);
    stmt.bind(4, r.base.name);
    stmt.bind(5, static_cast<int>(r.base.product));
    stmt.bind(6, static_cast<int>(r.base.settle_cycle));
    stmt.bind(7, static_cast<int>(r.base.settlement_method));
    stmt.bind(8, static_cast<int>(r.base.is_inverse));
    stmt.bind(9, r.base.currency);
    stmt.bind(10, r.base.base_asset);
    stmt.bind(11, static_cast<int64_t>(r.base.min_order_volume));
    stmt.bind(12, static_cast<int64_t>(r.base.max_order_volume));
    stmt.bind(13, r.base.volume_multiple);
    stmt.bind(14, r.base.price_tick);
    stmt.bind(15, r.base.volume_step);
    stmt.bind(16, r.base.listed_date);
    stmt.bind(17, r.base.expiry_date);
    stmt.bind(18, static_cast<int>(r.base.option_type));
    stmt.bind(19, static_cast<int>(r.base.option_exercise_style));
    stmt.bind(20, r.base.underlying_id);
    stmt.bind(21, r.base.option_strike);
    stmt.bind(22, r.base.option_series);
    stmt.bind(23, r.update_day);
}

std::string PersistWriter::format_trading_day(int64_t days) {
    // DzDate (距纪元天数) -> "YYYYMMDD" 文本 (positions/trading_accounts.trading_day 列)
    dztrader::Date d{static_cast<int32_t>(days)};
    return std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
}

void PersistWriter::bind_position(SQLite::Statement& stmt, const DzPositionInfo& r,
                                  const std::string& trading_day) {
    stmt.bind(1, r.account_id);
    stmt.bind(2, trading_day);
    stmt.bind(3, r.instrument_id);
    stmt.bind(4, r.exchange_id);
    stmt.bind(5, static_cast<int>(r.direction));
    stmt.bind(6, r.volume);
    stmt.bind(7, r.frozen_volume);
    stmt.bind(8, r.today_volume);
    stmt.bind(9, r.yd_volume);
    stmt.bind(10, r.price);
    stmt.bind(11, static_cast<int64_t>(r.seq));
}

void PersistWriter::bind_trading_account(SQLite::Statement& stmt, const DzTradingAccount& r,
                                         const std::string& trading_day) {
    stmt.bind(1, r.account_id);
    stmt.bind(2, trading_day);
    stmt.bind(3, r.balance);
    stmt.bind(4, r.available);
    stmt.bind(5, r.frozen);
    stmt.bind(6, r.commission);
    stmt.bind(7, r.margin);
    stmt.bind(8, r.withdraw_quota);
    stmt.bind(9, r.deposit);
    stmt.bind(10, r.withdraw);
    stmt.bind(11, static_cast<int64_t>(r.seq));
}

}  // namespace dztrader::ctp
