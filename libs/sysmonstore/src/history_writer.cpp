#include <dztrader/sysmonstore/history_store.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace dztrader::sysmonstore {

namespace {

/// 落盘线程轮询间隔：没有新批次时用它等待维护周期到点（不 busy-wait）。
/// 有批次入队时由条件变量立即唤醒，所以这个值只决定维护检查的粒度。
constexpr std::chrono::milliseconds kIdlePollInterval{500};

}  // namespace

struct HistoryWriter::Impl {
    std::filesystem::path path;
    HistoryWriterOptions options;
    std::function<RetentionPolicy()> retention;
    std::unique_ptr<HistoryStore> store;

    std::mutex mutex;
    std::condition_variable cv;
    /// 有界队列：每个元素是一个批次（批量提交的单元）。
    std::deque<HistoryBatch> queue;
    bool stop = false;         ///< 停止接收新批次
    bool thread_done = false;  ///< 落盘线程已退出

    std::atomic<uint64_t> dropped_records{0};
    std::atomic<uint64_t> written_records{0};
    std::thread worker;

    std::chrono::steady_clock::time_point next_maintenance{};

    /// 落盘线程主体：等批次 → 取走整批 → 提交 → 周期性执行保留维护。
    void run() {
        while (true) {
            std::deque<HistoryBatch> pending;
            bool should_stop = false;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock, kIdlePollInterval, [this]() { return stop || !queue.empty(); });
                pending.swap(queue);
                should_stop = stop;
            }

            // 合并成一个批次提交：单个事务写入全部待落盘记录（批量提交的核心收益）。
            // 合并上限是 `max_batch_records`：超出部分留给下一轮，避免单事务过大
            // （大事务会长时间占写锁，读方 busy_timeout 期间会等不到）。
            HistoryBatch merged;
            size_t merged_records = 0;
            std::deque<HistoryBatch> leftover;
            for (auto& batch : pending) {
                const bool fits =
                    merged_records + batch.record_count() <= options.max_batch_records ||
                    merged_records == 0;
                if (!fits) {
                    leftover.push_back(std::move(batch));
                    continue;
                }
                merged_records += batch.record_count();
                move_into(merged, batch);
            }
            // 没写进本轮的部分放回队首（保持时间顺序：先入队的先落盘）。
            if (!leftover.empty()) {
                std::unique_lock lock(mutex);
                for (auto it = leftover.rbegin(); it != leftover.rend(); ++it) {
                    queue.push_front(std::move(*it));
                }
                cv.notify_one();
            }

            if (!merged.empty()) {
                flush_batch(merged);
            }
            run_maintenance_if_due();

            if (should_stop) {
                // 停止前把队列里剩余的批次全部冲掉：优雅退出不得丢数据。
                drain_remaining();
                break;
            }
        }
        std::unique_lock lock(mutex);
        thread_done = true;
        cv.notify_all();
    }

    void flush_batch(HistoryBatch& batch) noexcept {
        if (store == nullptr) {
            dropped_records += batch.record_count();
            return;
        }
        try {
            store->write_batch(batch);
            written_records += batch.record_count();
        } catch (const std::exception& e) {
            // 落盘失败不杀进程：历史是观测辅助，采集与实时推送必须继续。
            // 记录错误并计为丢弃，让上层能从 dropped_records 看出端倪。
            SPDLOG_ERROR("sysmon history batch write failed | records={} error=\"{}\"",
                         batch.record_count(), e.what());
            dropped_records += batch.record_count();
        }
    }

    void run_maintenance_if_due() noexcept {
        if (store == nullptr || retention == nullptr || options.maintenance_interval_sec <= 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_maintenance) {
            return;
        }
        next_maintenance = now + std::chrono::seconds(options.maintenance_interval_sec);
        try {
            const auto policy = retention();
            const auto result = store->apply_retention(policy);
            SPDLOG_INFO(
                "sysmon history retention applied | compacted_rows={} compacted_buckets={} "
                "deleted_raw={} deleted_minute={} deleted_marks={} deleted_by_size={} "
                "bytes_before={} bytes_after={}",
                result.compacted_rows, result.compacted_buckets, result.deleted_raw_rows,
                result.deleted_minute_rows, result.deleted_marks, result.deleted_by_size_rows,
                result.bytes_before, result.bytes_after);
        } catch (const std::exception& e) {
            // 维护失败不杀进程：下一周期重试（不清 next_maintenance，等一个周期后再试）。
            SPDLOG_ERROR("sysmon history retention failed | error=\"{}\"", e.what());
        }
    }

    void drain_remaining() noexcept {
        while (true) {
            HistoryBatch batch;
            {
                std::unique_lock lock(mutex);
                if (queue.empty()) {
                    break;
                }
                batch = std::move(queue.front());
                queue.pop_front();
                // 队列里剩下的也一并取走，减少锁往返。
                while (!queue.empty()) {
                    move_into(batch, queue.front());
                    queue.pop_front();
                }
            }
            flush_batch(batch);
        }
    }

    static void move_into(HistoryBatch& target, HistoryBatch& source) {
        for (auto& record : source.scalars) {
            target.scalars.push_back(std::move(record));
        }
        for (auto& record : source.dimensions) {
            target.dimensions.push_back(std::move(record));
        }
        for (auto& mark : source.marks) {
            target.marks.push_back(std::move(mark));
        }
        source.clear();
    }
};

HistoryWriter::HistoryWriter(const std::filesystem::path& path,
                             HistoryWriterOptions options,
                             std::function<RetentionPolicy()> retention)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->options = options;
    impl_->retention = std::move(retention);
    // 打开库在构造线程上同步完成：失败要能被调用方捕获并降级为「不写历史」，
    // 而不是在落盘线程里崩掉（那样主进程只会看到子进程反复重启）。
    impl_->store =
        std::make_unique<HistoryStore>(HistoryStore::open(path, options.busy_timeout_ms));
    impl_->next_maintenance =
        std::chrono::steady_clock::now() + std::chrono::seconds(options.maintenance_interval_sec);
    impl_->worker = std::thread([this]() { impl_->run(); });
}

HistoryWriter::~HistoryWriter() { (void)flush(); }

void HistoryWriter::enqueue(HistoryBatch&& batch) noexcept {
    if (batch.empty()) {
        return;
    }
    const uint64_t incoming = batch.record_count();
    {
        std::unique_lock lock(impl_->mutex);
        if (impl_->stop) {
            // 已请求退出：丢弃而不阻塞（入队方是采集线程，绝不能卡在退出路径上）。
            impl_->dropped_records += incoming;
            return;
        }
        if (impl_->queue.size() >= impl_->options.queue_capacity && !impl_->queue.empty()) {
            // 队列满：丢**最旧**的批次（新数据还能解释刚刚发生了什么；缺口会由
            // 维护周期记成标记）。计数供上层观测。
            const auto dropped = impl_->queue.front().record_count();
            impl_->dropped_records += dropped;
            impl_->queue.pop_front();
        }
        impl_->queue.push_back(std::move(batch));
    }
    impl_->cv.notify_one();
}

size_t HistoryWriter::flush() noexcept {
    {
        std::unique_lock lock(impl_->mutex);
        if (impl_->stop) {
            // 幂等：已在退出流程中，等线程收尾即可（下面 join）。
            lock.unlock();
            if (impl_->worker.joinable()) {
                impl_->worker.join();
            }
            return 0;
        }
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    // 落盘线程已把队列冲空并退出：关库（checkpoint + 释放连接）。
    if (impl_->store) {
        impl_->store->close();
    }
    return 0;
}

uint64_t HistoryWriter::dropped_records() const noexcept { return impl_->dropped_records.load(); }

uint64_t HistoryWriter::written_records() const noexcept { return impl_->written_records.load(); }

bool HistoryWriter::running() const noexcept {
    std::unique_lock lock(impl_->mutex);
    return !impl_->thread_done;
}

}  // namespace dztrader::sysmonstore
