#include "td/td_persist_writer.h"

#include <SQLiteCpp/Exception.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

#include <dztrader/db/database.h>
#include <dztrader/date_time/date_time.h>  // Date (DzDate 距纪元天数 -> YYYYMMDD)
#include <dztrader/tdstore/records_store.h>
#include <dztrader/tdstore/schema_catalog.h>

#include "td/td_persist_rows.h"

namespace dztrader::ctp {

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

    // 统一 DB 接口: schema 由 tdstore 声明 (字段序 = 建表列序), migrate 幂等
    dztrader::db::Config config{
        .backend = "sqlite",
        .options = {{"path", db_path_}, {"journal_mode", "wal"}, {"synchronous", "full"},
                    {"busy_timeout_ms", "5000"}, {"cache_size_kb", "8000"},
                    {"temp_store", "memory"}}};
    database_ = dztrader::db::Database::open(config, dztrader::tdstore::schemas());
    database_->migrate();
    // Writer 线程独占会话 (open 后 start_writer 前主线程不得复用)
    writer_session_ = database_->session();

    opened_ = true;
    SPDLOG_INFO("database opened | path={}", db_path_);
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

    // 释放 Session + Database (Writer 已退出, 无竞争; Session 须先于 Database)
    writer_session_.reset();
    database_.reset();

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

    writer_session_.reset();
    database_.reset();

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

int64_t PersistWriter::max_order_id() {
    // 调用时机: open() 后 start_writer() 前 (主线程独立只读会话, 与 Writer 无竞争)
    if (!opened_.load()) {
        throw std::runtime_error("must call open() before max_order_id()");
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (writer_started_) {
            throw std::runtime_error(
                "max_order_id() not available after start_writer() (writer thread owns db)");
        }
    }
    if (database_ == nullptr) {
        throw std::runtime_error("max_order_id() not available after stop()");
    }
    // 临时只读会话: orders/trades 两表 MAX(order_id) 取大, 空表 (NULL) 计 0
    std::unique_ptr<dztrader::db::Session> session = database_->session(/*read_only=*/true);
    int64_t result = 0;
    for (std::string_view collection : {"orders", "trades"}) {
        dztrader::db::Aggregation aggregation;
        aggregation.op = dztrader::db::AggregateOp::Max;
        aggregation.field = "order_id";
        dztrader::db::ResultSet rows = session->aggregate(collection, aggregation);
        if (rows.empty() || rows.rows().front().values().empty()) {
            continue;
        }
        if (const auto* value = std::get_if<int64_t>(&rows.rows().front().values().front())) {
            result = std::max(result, *value);
        }
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
            std::unique_ptr<dztrader::db::Transaction> transaction =
                writer_session_->begin_transaction();
            execute_batch(batch, flushed_tokens);
            transaction->commit();
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

void PersistWriter::execute_batch(std::vector<PersistTask>& batch,
                                  std::vector<uint64_t>& flushed_tokens) {
    // FIFO 语义 (load-bearing): 按批内原始序处理任务; 仅连续同类任务聚合为一次 upsert;
    // PositionRebuild 在原位先 remove 再 upsert — 早于它的 Position 行被清除,
    // 晚于它的 Position 行覆盖 rebuild 结果, 不得把所有 delete 提前到所有 upsert 之前.
    PersistTask::Kind pending_kind = PersistTask::Kind::Order;
    bool has_pending = false;
    std::vector<dztrader::db::Row> pending_rows;

    const auto flush_pending = [&]() {
        if (has_pending && !pending_rows.empty()) {
            switch (pending_kind) {
                case PersistTask::Kind::Order: writer_session_->upsert("orders", pending_rows); break;
                case PersistTask::Kind::Trade: writer_session_->upsert("trades", pending_rows); break;
                case PersistTask::Kind::Position:
                    writer_session_->upsert("positions", pending_rows);
                    break;
                case PersistTask::Kind::TradingAccount:
                    writer_session_->upsert("trading_accounts", pending_rows);
                    break;
                case PersistTask::Kind::Instrument:
                    writer_session_->upsert("instruments", pending_rows);
                    break;
                default: break;
            }
        }
        has_pending = false;
        pending_rows.clear();
    };

    const auto accumulate = [&](PersistTask::Kind kind, dztrader::db::Row row) {
        if (has_pending && pending_kind != kind) {
            flush_pending();
        }
        pending_kind = kind;
        has_pending = true;
        pending_rows.push_back(std::move(row));
    };

    for (auto& task : batch) {
        switch (task.kind) {
            case PersistTask::Kind::Order:
                accumulate(task.kind, make_order_row(std::get<OrderRecord>(task.data)));
                break;
            case PersistTask::Kind::Trade:
                accumulate(task.kind, make_trade_row(std::get<TradeRecord>(task.data)));
                break;
            case PersistTask::Kind::Instrument:
                accumulate(task.kind, tdstore::make_instrument_row(
                                          std::get<tdstore::InstrumentRecord>(task.data)));
                break;
            case PersistTask::Kind::Position: {
                // 单行绝对态 upsert (盘中有变化时走它). task.trading_day 是当前交易日.
                const auto& row = std::get<std::vector<DzPositionInfo>>(task.data).front();
                accumulate(task.kind,
                           tdstore::make_position_row(row, format_trading_day(task.trading_day)));
                break;
            }
            case PersistTask::Kind::TradingAccount:
                accumulate(task.kind, tdstore::make_trading_account_row(
                                          std::get<DzTradingAccount>(task.data),
                                          format_trading_day(task.trading_day)));
                break;
            case PersistTask::Kind::PositionRebuild: {
                // 单事务重灌 (spec §3.2 全量语义): 先落此前任务保持 FIFO, 再原位
                // remove + upsert; 外层 writer_loop 事务保证外部读者只见原子切换.
                // 删该账户全部行 (而非按 trading_day 排除): 查询响应为全量, 响应不含的
                // 合约 = 已全平/已过期, 必须删除 (否则盘中平仓的幽灵持仓永驻到次日).
                flush_pending();
                writer_session_->remove("positions",
                                        dztrader::db::filters::eq("account_id", task.account_id));
                std::vector<dztrader::db::Row> rows;
                const std::string day = format_trading_day(task.trading_day);
                for (const auto& row : std::get<std::vector<DzPositionInfo>>(task.data)) {
                    rows.push_back(tdstore::make_position_row(row, day));
                }
                writer_session_->upsert("positions", rows);
                break;
            }
            case PersistTask::Kind::FlushSignal:
                // FIFO 哨兵: 收集 token, 由 writer_loop 在批事务 commit 成功后统一
                // set_value. 保证 "wait_flush 返回" ⇒ "此前任务必已提交"
                // (评审 C1: set 必须在 commit 之后, 不得在此处提前 set).
                flushed_tokens.push_back(task.flush_token);
                break;
        }
    }
    flush_pending();
}

std::string PersistWriter::format_trading_day(int64_t days) {
    // DzDate (距纪元天数) -> "YYYYMMDD" 文本 (positions/trading_accounts.trading_day 列)
    dztrader::Date d{static_cast<int32_t>(days)};
    return std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
}

}  // namespace dztrader::ctp
