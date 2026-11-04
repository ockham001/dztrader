#include "td/td_persist_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <spdlog/spdlog.h>

#include <dztrader/date_time/date_time.h>  // Date (DzDate 距纪元天数 -> YYYYMMDD)
#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include "td/td_persist_rows.h"

namespace dztrader::ctp {

namespace {

constexpr int kBusyTimeoutMs = 10000;  ///< ADR 0042 §3: 写连接 busy_timeout 10s
constexpr int kCacheSizeKb = 8000;
/// 连续损坏重建上限：超过则只退避重试、不再改名重建（防坏盘每轮产生一个 `.old-*` 副本）
constexpr int kMaxCorruptionRebuilds = 3;

/// SQLite 主 result code（低 8 位；扩展码在 extended code 中，主码相同）。
constexpr int kSqliteCorrupt = 11;  ///< SQLITE_CORRUPT（库文件损坏/结构非法）
constexpr int kSqliteNotADb = 26;   ///< SQLITE_NOTADB（文件头非法，不是 SQLite 库）

/// 错误文本归一到单行 prose（日志值 `error="..."` 用）：SQLite/建库门面的错误消息
/// 自带 `| k=v` 片段与双引号，直接嵌入会形成未转义引号与嵌套 k=v（违反日志格式约定）。
/// 替换只作用于本进程输出的错误值，不改变原始异常讲给调用方的语义。
std::string error_value(std::string_view error) {
    std::string text(error);
    std::replace(text.begin(), text.end(), '"', '\'');
    std::replace(text.begin(), text.end(), '|', '/');
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

}  // namespace

// ============================================================================
// 构造 / 析构 / 生命周期
// ============================================================================

PersistWriter::PersistWriter(std::string db_path,
                             std::string gateway,
                             PersistPolicy policy,
                             PersistNotifyFn notify)
    : db_path_(std::move(db_path)),
      gateway_(std::move(gateway)),
      policy_(std::move(policy)),
      notify_(std::move(notify)) {}

PersistWriter::~PersistWriter() {
    // 析构仅作 best-effort 兜底 (短超时, 不 quick_exit); 主流程必须显式调 stop().
    if (explicit_stopped_) {
        return;
    }
    bool started = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        started = writer_started_;
    }
    if (started) {
        SPDLOG_WARN("persist writer destroyed without explicit stop, best-effort cleanup | path={}",
                    db_path_);
        try {
            stop_best_effort();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("persist writer best-effort stop failed | error=\"{}\"", e.what());
        }
    } else {
        close_connection();
        explicit_stopped_ = true;
    }
}

void PersistWriter::open() {
    if (opened_) {
        throw std::runtime_error("persist writer already opened");
    }
    opened_ = true;
    if (try_open()) {
        SPDLOG_INFO("trade db persist opened | path={}", db_path_);
    } else {
        // 首次建连失败不抛：Writer 线程按退避序列重连 (ADR 0042 §3).
        note_failure(last_error_);
    }
}

void PersistWriter::start_writer() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (writer_started_) {
        SPDLOG_WARN("start_writer called twice, ignoring");
        return;
    }
    if (!opened_) {
        throw std::runtime_error("must call open() before start_writer()");
    }
    running_ = true;
    writer_exited_ = false;
    writer_started_ = true;
    writer_thread_ = std::thread([this] { writer_loop(); });
    SPDLOG_INFO("persist writer thread started | path={}", db_path_);
}

void PersistWriter::stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!writer_started_) {
            // Writer 未启动: 直接关闭连接即可 (无队列可排空).
            shutdown_ = true;
            explicit_stopped_ = true;
            close_connection();
            SPDLOG_INFO("persist writer stopped | path={}", db_path_);
            return;
        }
        running_ = false;
        shutdown_ = true;
    }
    cv_.notify_all();

    // 等待 Writer 退出 (30s 超时: 大事务 fsync 兜底)
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_writer_done_.wait_for(lk, kShutdownTimeout, [this] { return writer_exited_; })) {
        SPDLOG_ERROR("persist writer shutdown timeout, calling quick_exit | path={} queued={}",
                     db_path_, queue_.size());
        std::quick_exit(1);  // 跳过析构, OS 回收资源 (Writer 可能仍持 sqlite 句柄)
    }
    lk.unlock();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    close_connection();

    {
        std::lock_guard<std::mutex> lk2(mtx_);
        writer_started_ = false;
    }
    explicit_stopped_ = true;
    SPDLOG_INFO("persist writer stopped | path={}", db_path_);
}

void PersistWriter::stop_best_effort() {
    // 析构兜底路径: 短超时, 超时 detach (不 quick_exit; 接受泄漏).
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!writer_started_) {
            explicit_stopped_ = true;
            close_connection();
            return;
        }
        running_ = false;
        shutdown_ = true;
    }
    cv_.notify_all();

    std::unique_lock<std::mutex> lk(mtx_);
    constexpr auto kBestEffortTimeout = std::chrono::seconds(1);
    if (!cv_writer_done_.wait_for(lk, kBestEffortTimeout, [this] { return writer_exited_; })) {
        SPDLOG_ERROR("persist writer best-effort stop timeout (detached) | path={} queued={}",
                     db_path_, queue_.size());
        if (writer_thread_.joinable()) {
            writer_thread_.detach();
        }
        explicit_stopped_ = true;
        return;  // 不释放 db_ (Writer 线程可能仍在用), 接受泄漏
    }
    lk.unlock();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    close_connection();

    {
        std::lock_guard<std::mutex> lk2(mtx_);
        writer_started_ = false;
    }
    explicit_stopped_ = true;
    SPDLOG_INFO("persist writer stopped (best-effort) | path={}", db_path_);
}

// ============================================================================
// 入队 (无上限、不阻塞) / flush 哨兵
// ============================================================================

void PersistWriter::enqueue(PersistTask task) {
    bool backlog_warn = false;
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (shutdown_) {
            // stop() 已调用, 丢弃. FlushSignal 丢弃分支须立即 set_value: 丢弃 = 队列已无
            // 此前任务, wait_flush 恒返回 true (哨兵被"消费"即视为提交完成语义).
            if (task.kind == PersistTask::Kind::FlushSignal) {
                auto token = next_flush_token_++;
                pending_flushes_[token].set_value();
                pending_flushes_.erase(token);
            }
            return;
        }
        queue_.push_back(std::move(task));
        queued = queue_.size();
        const std::size_t size = queued;
        if (!backlog_warned_ && policy_.backlog_warn_threshold > 0 &&
            size >= policy_.backlog_warn_threshold) {
            backlog_warned_ = true;
            backlog_warn = true;
        } else if (backlog_warned_ && policy_.backlog_warn_threshold > 0 &&
                   size < policy_.backlog_warn_threshold / 2) {
            backlog_warned_ = false;  // 回落后重新武装
        }
    }
    cv_.notify_one();
    if (backlog_warn) {
        // 积压只告警一次/积压期 (ADR 0042 §1: 队列不设上限, 内存兜底 + 阈值告警).
        SPDLOG_WARN("trade db persist backlog | path={} queued={} threshold={}", db_path_, queued,
                    policy_.backlog_warn_threshold);
        emit_notify(DZ_NOTIFY_WARN,
                    std::format("trade db persist backlog | path={} queued>={}", db_path_,
                                policy_.backlog_warn_threshold),
                    false);
    }
}

uint64_t PersistWriter::enqueue_flush_signal() {
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        token = next_flush_token_++;
        if (shutdown_) {
            // stop() 后: 丢弃路径语义, 返回"已消费"哨兵 token (wait_flush 恒 true).
            pending_flushes_[token].set_value();
            pending_flushes_.erase(token);
            return token;
        }
        pending_flushes_.emplace(token, std::promise<void>{});
        queue_.push_back(PersistTask{
            .kind = PersistTask::Kind::FlushSignal, .account_id = "", .flush_token = token});
    }
    cv_.notify_one();
    return token;
}

bool PersistWriter::wait_flush(uint64_t token, std::chrono::milliseconds timeout) {
    std::future<void> fut;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = pending_flushes_.find(token);
        if (it == pending_flushes_.end()) {
            // 未知 token: 视为已消费 (提交成功已 erase / stop 丢弃路径 / 非法值).
            return true;
        }
        fut = it->second.get_future();
    }
    // promise 由 Writer 线程 set_value (提交成功) / set_exception (退出排空失败),
    // 或 enqueue_flush_signal 的 stop 丢弃路径 set_value.
    if (fut.wait_for(timeout) != std::future_status::ready) {
        return false;
    }
    try {
        fut.get();  // 传播 set_exception (退出排空失败) — 捕获转 false, 不逃逸给调用方
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
    return true;
}

// ============================================================================
// Writer 线程主循环 (纯事件驱动, 无轮询)
// ============================================================================

void PersistWriter::writer_loop() {
    SPDLOG_INFO("persist writer loop started | path={}", db_path_);
    std::vector<PersistTask> batch;  // 失败时保留, 退避后重试同一批 (FIFO)
    std::size_t retry_attempt = 0;

    while (true) {
        if (!connected_) {
            if (!try_open()) {
                note_failure(last_error_);
                if (!wait_retry_or_stop(retry_attempt++)) {
                    break;  // 停止请求: 无连接可排空, 退出
                }
                continue;
            }
            retry_attempt = 0;
            // 恢复 INFO 留到首个批提交成功后发 (数据真正落盘), 见 note_recovered().
        }

        if (batch.empty()) {
            if (!take_batch(batch)) {
                break;  // 已停止且队列空
            }
        }

        if (commit_batch(batch)) {
            signal_flush_tokens(batch);
            note_recovered();
            corruption_rebuilds_ = 0;
            retry_attempt = 0;
            batch.clear();
            continue;
        }

        // 批失败: 整批已回滚并保留在 batch, 通知 + 退避后重试同一批 (ADR 0042 §2).
        note_failure(last_error_);
        if (!wait_retry_or_stop(retry_attempt++)) {
            // 停止请求: 对失败批做最后一次立即重试 (退出前尽力排空).
            if (commit_batch(batch)) {
                signal_flush_tokens(batch);
                note_recovered();
                corruption_rebuilds_ = 0;
                batch.clear();
                continue;  // 继续排空队列, 之后 take_batch 返回 false 退出
            }
            break;
        }
    }

    // 退出: 未提交的失败批 + 队列 (含其中 FlushSignal token) 一律不落盘,
    // 记 ERROR (ADR 0042 §6: 未提交内存积压丢弃为已知限制).
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queued = queue_.size();
    }
    if (!batch.empty() || queued > 0) {
        SPDLOG_ERROR(
            "persist shutdown with uncommitted tasks, dropping | path={} batch={} queued={}",
            db_path_, batch.size(), queued);
    }
    fail_pending_flush_waiters();
    close_connection();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        writer_exited_ = true;
    }
    cv_writer_done_.notify_one();
    SPDLOG_INFO("persist writer loop exited | path={}", db_path_);
}

bool PersistWriter::take_batch(std::vector<PersistTask>& batch) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
    if (queue_.empty()) {
        // 谓词保证此时 running_ == false (已停止且队列空).
        return false;
    }
    while (!queue_.empty()) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return true;
}

bool PersistWriter::commit_batch(std::vector<PersistTask>& batch) {
    if (!db_) {
        last_error_ = "database not open";
        return false;
    }
    last_error_.clear();
    try {
        SQLite::Transaction transaction(*db_);
        for (auto& task : batch) {
            switch (task.kind) {
                case PersistTask::Kind::Order: {
                    SQLite::Statement& stmt = statement_for(task.kind);
                    stmt.reset();
                    stmt.clearBindings();
                    bind_order(stmt, std::get<OrderRecord>(task.data), gateway_);
                    stmt.exec();
                    break;
                }
                case PersistTask::Kind::Trade: {
                    SQLite::Statement& stmt = statement_for(task.kind);
                    stmt.reset();
                    stmt.clearBindings();
                    bind_trade(stmt, std::get<TradeRecord>(task.data), gateway_);
                    stmt.exec();
                    break;
                }
                case PersistTask::Kind::Instrument: {
                    SQLite::Statement& stmt = statement_for(task.kind);
                    stmt.reset();
                    stmt.clearBindings();
                    bind_instrument(stmt, std::get<tdstore::InstrumentRecord>(task.data));
                    stmt.exec();
                    break;
                }
                case PersistTask::Kind::Position: {
                    // 单行绝对态 upsert (盘中有变化时走它). task.trading_day 是当前交易日.
                    const auto& row = std::get<std::vector<DzPositionInfo>>(task.data).front();
                    SQLite::Statement& stmt = statement_for(task.kind);
                    stmt.reset();
                    stmt.clearBindings();
                    bind_position(stmt, row, format_date(task.trading_day), gateway_);
                    stmt.exec();
                    break;
                }
                case PersistTask::Kind::TradingAccount: {
                    SQLite::Statement& stmt = statement_for(task.kind);
                    stmt.reset();
                    stmt.clearBindings();
                    bind_trading_account(stmt, std::get<DzTradingAccount>(task.data),
                                         format_date(task.trading_day), gateway_);
                    stmt.exec();
                    break;
                }
                case PersistTask::Kind::PositionRebuild: {
                    // 单事务重灌 (ADR 0045): 先删该账户当日行再 upsert 本组. 按批内原始序
                    // 原位执行 — 早于它的 Position 行被清除, 晚于它的 Position 行覆盖
                    // rebuild 结果, 不得把所有 DELETE 提前到所有 upsert 之前.
                    // 只删当日行: 历史交易日行不可变.
                    const std::string day = format_date(task.trading_day);
                    SQLite::Statement& del = position_delete_statement();
                    del.reset();
                    del.clearBindings();
                    del.bind(":account_id", task.account_id);
                    del.bind(":date", day);
                    del.exec();
                    SQLite::Statement& stmt = statement_for(PersistTask::Kind::Position);
                    for (const auto& row : std::get<std::vector<DzPositionInfo>>(task.data)) {
                        stmt.reset();
                        stmt.clearBindings();
                        bind_position(stmt, row, day, gateway_);
                        stmt.exec();
                    }
                    break;
                }
                case PersistTask::Kind::FlushSignal:
                    // FIFO 哨兵: 收集 token 由 writer_loop 在批事务 commit 成功后统一
                    // set_value. 保证 "wait_flush 返回" ⇒ "此前任务必已提交".
                    break;
            }
        }
        transaction.commit();
        return true;
    } catch (const SQLite::Exception& e) {
        last_error_ = error_value(e.what());
        const int primary_code = e.getErrorCode() & 0xFF;
        if (is_corruption(primary_code)) {
            handle_corruption(primary_code);
        } else {
            reset_statements();
        }
        return false;
    } catch (const std::exception& e) {
        last_error_ = error_value(e.what());
        reset_statements();
        return false;
    } catch (...) {
        // 非 std 异常 (如 SQLite C 接口错误回调 throw) 也必须吞下, 防止 Writer 线程
        // 意外退出导致队列无消费者.
        last_error_ = "unknown persist error";
        reset_statements();
        return false;
    }
}

void PersistWriter::signal_flush_tokens(const std::vector<PersistTask>& batch) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& task : batch) {
        if (task.kind != PersistTask::Kind::FlushSignal) {
            continue;
        }
        auto it = pending_flushes_.find(task.flush_token);
        if (it != pending_flushes_.end()) {
            it->second.set_value();
            pending_flushes_.erase(it);
        }
    }
}

bool PersistWriter::wait_retry_or_stop(std::size_t attempt) {
    std::chrono::milliseconds delay{30000};
    if (!policy_.retry_delays.empty()) {
        const std::size_t index = std::min(attempt, policy_.retry_delays.size() - 1);
        delay = policy_.retry_delays[index];
    }
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait_for(lk, delay, [this] { return !running_.load(); });
    return running_.load();
}

void PersistWriter::fail_pending_flush_waiters() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [token, promise] : pending_flushes_) {
        (void)token;
        try {
            promise.set_exception(std::make_exception_ptr(
                std::runtime_error("persist writer stopped before commit")));
        } catch (...) {
            // 防御: promise 已 set (理论不可达, 单 Writer 线程串行)
        }
    }
    pending_flushes_.clear();
}

// ============================================================================
// 建连 / 语句缓存
// ============================================================================

bool PersistWriter::try_open() {
    close_connection();
    try {
        // 建库门面 (master 负责建库, td 侧幂等兜底): 缺失库补建、旧版本/损坏库改名重建.
        const tdstore::TradeDbInitReport report = tdstore::ensure_trade_db(db_path_);
        if (report.status == tdstore::TradeDbInitReport::Status::Rebuilt) {
            // ADR 0042 §3: 坏库/旧库改名保留 + 新建 + ERROR, 交易继续.
            SPDLOG_ERROR("trade db rebuilt | path={} reason={} backup={}", db_path_,
                         report.rebuild_reason, report.backup_path);
            // 重建即本轮失败事件: 首次失败发 ERROR 弹窗一次, 后续失败按"持续"分级限频
            // (同一事件不得两次 ERROR 弹窗); 首个批提交成功时 note_recovered() 发恢复 INFO.
            if (!failing_) {
                failing_ = true;
                last_notify_ = std::chrono::steady_clock::now();
                emit_notify(DZ_NOTIFY_ERROR,
                            std::format("trade db rebuilt | path={} reason={} backup={}", db_path_,
                                        report.rebuild_reason, report.backup_path),
                            true);
            }
        }

        auto db = std::make_unique<SQLite::Database>(db_path_, SQLite::OPEN_READWRITE);
        db->exec(std::format("PRAGMA busy_timeout={}", kBusyTimeoutMs));
        db->exec("PRAGMA synchronous=FULL");
        db->exec(std::format("PRAGMA cache_size=-{}", kCacheSizeKb));
        db->exec("PRAGMA temp_store=MEMORY");
        const std::string journal_mode = db->execAndGet("PRAGMA journal_mode=WAL").getString();
        if (journal_mode != "wal") {
            // WAL 转换需独占锁且 busy_timeout 不生效: 视为可重试的建连失败.
            throw std::runtime_error("trade db wal conversion failed | mode=" + journal_mode);
        }
        db_ = std::move(db);
        connected_ = true;
        SPDLOG_INFO("trade db opened | path={}", db_path_);
        return true;
    } catch (const std::exception& e) {
        close_connection();
        last_error_ = error_value(e.what());
        SPDLOG_DEBUG("trade db open failed | path={} error=\"{}\"", db_path_, last_error_);
        return false;
    } catch (...) {
        close_connection();
        last_error_ = "unknown open error";
        SPDLOG_DEBUG("trade db open failed | path={} error=\"unknown\"", db_path_);
        return false;
    }
}

void PersistWriter::close_connection() {
    reset_statements();
    db_.reset();
    connected_ = false;
}

void PersistWriter::reset_statements() {
    stmt_order_.reset();
    stmt_trade_.reset();
    stmt_instrument_.reset();
    stmt_position_.reset();
    stmt_trading_account_.reset();
    stmt_position_delete_.reset();
}

SQLite::Statement& PersistWriter::statement_for(PersistTask::Kind kind) {
    std::unique_ptr<SQLite::Statement>* slot = nullptr;
    std::string sql;
    switch (kind) {
        case PersistTask::Kind::Order:
            slot = &stmt_order_;
            sql = tdstore::build_upsert_sql(tdstore::orders_schema());
            break;
        case PersistTask::Kind::Trade:
            slot = &stmt_trade_;
            sql = tdstore::build_upsert_sql(tdstore::trades_schema());
            break;
        case PersistTask::Kind::Instrument:
            slot = &stmt_instrument_;
            sql = tdstore::build_upsert_sql(tdstore::instruments_schema());
            break;
        case PersistTask::Kind::Position:
            slot = &stmt_position_;
            sql = tdstore::build_upsert_sql(tdstore::positions_schema());
            break;
        case PersistTask::Kind::TradingAccount:
            slot = &stmt_trading_account_;
            sql = tdstore::build_upsert_sql(tdstore::trading_accounts_schema());
            break;
        case PersistTask::Kind::PositionRebuild:
        case PersistTask::Kind::FlushSignal:
            break;
    }
    if (slot == nullptr) {
        throw std::runtime_error("persist task kind has no upsert statement");
    }
    if (!*slot) {
        *slot = std::make_unique<SQLite::Statement>(*db_, sql);
    }
    return **slot;
}

SQLite::Statement& PersistWriter::position_delete_statement() {
    if (!stmt_position_delete_) {
        stmt_position_delete_ = std::make_unique<SQLite::Statement>(
            *db_,
            "DELETE FROM \"positions\" WHERE \"account_id\" = :account_id AND \"date\" = :date");
    }
    return *stmt_position_delete_;
}

void PersistWriter::handle_corruption(int primary_code) {
    // Release 下 SPDLOG_DEBUG 被编译掉 (SPDLOG_ACTIVE_LEVEL), 显式消费入参防 unused-parameter.
    (void)primary_code;
    if (corruption_rebuilds_ >= kMaxCorruptionRebuilds) {
        // 连续多轮重建仍损坏: 视为持续失败, 只退避重试 (不再改名, 防 .old-* 副本风暴).
        SPDLOG_DEBUG(
            "trade db corrupt, rebuild limit reached, retrying | path={} code={} rebuilds={}",
            db_path_, primary_code, corruption_rebuilds_);
        reset_statements();
        return;
    }
    ++corruption_rebuilds_;
    // 失败通知/日志由调用方的 note_failure() 分级发出; 此处只留诊断轨迹.
    SPDLOG_DEBUG("trade db corrupt, will rebuild on next connect | path={} code={} rebuilds={}",
                 db_path_, primary_code, corruption_rebuilds_);
    // 关闭连接: 下一轮建连走 ensure_trade_db, 探测到坏文件后改名保留 + 新建 (ADR 0042 §3).
    close_connection();
}

// ============================================================================
// 通知分级 (ADR 0042 §4)
// ============================================================================

void PersistWriter::note_failure(const std::string& message) {
    const auto now = std::chrono::steady_clock::now();
    if (!failing_) {
        failing_ = true;
        last_notify_ = now;
        SPDLOG_ERROR("trade db persist failed | path={} error=\"{}\"", db_path_, message);
        emit_notify(
            DZ_NOTIFY_ERROR,
            std::format("trade db persist failed | path={} error=\"{}\"", db_path_, message), true);
        return;
    }
    if (now - last_notify_ >= policy_.sustained_notify_interval) {
        last_notify_ = now;
        SPDLOG_WARN("trade db persist still failing | path={} error=\"{}\"", db_path_, message);
        emit_notify(
            DZ_NOTIFY_WARN,
            std::format("trade db persist still failing | path={} error=\"{}\"", db_path_, message),
            true);
        return;
    }
    // 首次 ERROR / 每 5 分钟 WARN 之外不刷屏: 只留 DEBUG 轨迹.
    SPDLOG_DEBUG("trade db persist still failing (throttled) | path={} error=\"{}\"", db_path_,
                 message);
}

void PersistWriter::note_recovered() {
    if (!failing_) {
        return;
    }
    failing_ = false;
    SPDLOG_INFO("trade db persist recovered | path={}", db_path_);
    emit_notify(DZ_NOTIFY_INFO, std::format("trade db persist recovered | path={}", db_path_),
                false);
}

void PersistWriter::emit_notify(DzNotifyLevel level, const std::string& message, bool popup) const {
    if (!notify_) {
        return;
    }
    try {
        notify_(level, message, popup);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("persist notify callback failed | error=\"{}\"", error_value(e.what()));
    } catch (...) {
        SPDLOG_ERROR("persist notify callback failed | error=\"unknown\"");
    }
}

// ============================================================================
// 工具
// ============================================================================

std::string PersistWriter::format_date(int64_t days) {
    // DzDate (距纪元天数) -> "YYYYMMDD" 文本 (schema v1 date 列)
    dztrader::Date d{static_cast<int32_t>(days)};
    return std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
}

bool PersistWriter::is_corruption(int sqlite_result_code) {
    return sqlite_result_code == kSqliteCorrupt || sqlite_result_code == kSqliteNotADb;
}

}  // namespace dztrader::ctp
