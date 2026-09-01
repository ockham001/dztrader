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
    "    insert_time, update_time, error_id, error_msg, strategy_id, remark"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,  ?,?,?,?,?,?,  ?,?,?,?, ?,?)";

constexpr const char* kInsertTradeSql =
    "INSERT OR REPLACE INTO trades ("
    "    account_id, trading_day, trade_id, order_id, instrument_id, exchange_id,"
    "    direction, position_effect, price, volume, trade_time, trade_date, commission,"
    "    strategy_id"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,  ?,?,?,?)";

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
    "    instrument_id, exchange_id, name, product, volume_multiple, price_tick,"
    "    min_order_volume, max_order_volume, option_type, option_strike,"
    "    option_underlying, option_listed, option_expiry, update_day"
    ") VALUES (?,?,?,?,?,?,  ?,?,?,?,  ?,?,?,?)";

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
            // stop() 已调用, 丢弃 (区别于 running_=false 的未启动状态)
            return;
        }
        queue_.push(std::move(task));
    }
    cv_empty_.notify_one();
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
        try {
            SQLite::Transaction txn(*db_);
            execute_batch(*db_, batch);
            txn.commit();
            SPDLOG_DEBUG("persist batch committed | count={}", batch.size());
        } catch (const SQLite::Exception& e) {
            SPDLOG_ERROR("persist batch failed, dropping | count={} error=\"{}\"",
                         batch.size(), e.what());
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist batch failed (std), dropping | count={} error=\"{}\"",
                         batch.size(), e.what());
        } catch (...) {
            // 非 std 异常 (如 SQLite C 接口错误回调 throw int) 也必须吞下,
            // 防止 Writer 线程意外退出导致主线程 enqueue 永久阻塞 (设计 §13.11)
            SPDLOG_ERROR("persist batch failed (unknown), dropping | count={}", batch.size());
        }
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        writer_exited_ = true;
    }
    cv_writer_done_.notify_one();
    SPDLOG_INFO("writer loop exited");
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

void PersistWriter::execute_batch(SQLite::Database& db, std::vector<PersistTask>& batch) {
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
    stmt.bind(3, r.base.name);
    stmt.bind(4, static_cast<int>(r.base.product));
    stmt.bind(5, r.base.volume_multiple);
    stmt.bind(6, r.base.price_tick);
    stmt.bind(7, r.base.min_order_volume);
    stmt.bind(8, r.base.max_order_volume);
    stmt.bind(9, static_cast<int>(r.base.option_type));
    stmt.bind(10, r.base.option_strike);
    stmt.bind(11, r.base.option_underlying);
    stmt.bind(12, r.base.option_listed);
    stmt.bind(13, r.base.option_expiry);
    stmt.bind(14, r.update_day);
}

}  // namespace dztrader::ctp
