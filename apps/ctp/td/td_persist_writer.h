#ifndef DZTRADER_CTP_TD_PERSIST_WRITER_H_
#define DZTRADER_CTP_TD_PERSIST_WRITER_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <variant>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <vector>

#include "td/td_schema.h"

namespace dztrader::ctp {

/// 持久化任务 (队列元素, POD payload 避免 SPI 线程 to_json 抛异常).
struct PersistTask {
    enum class Kind : uint8_t {
        Order,
        Trade,
        MarginRate,
        CommissionRate,
        Instrument,
    } kind;

    std::variant<OrderRecord, TradeRecord, MarginRateRecord,
                 CommissionRateRecord, InstrumentRecord> data;
};

/// 专用 SQLite Writer 线程 + 队列 (设计 §13.8-13.11).
///
/// 线程模型 (纯事件驱动, 无轮询):
/// - SPI 线程: enqueue(task) 入队 (微秒级, mutex+cv)
/// - Writer 线程: 阻塞等待 cv_empty_, drain 队列, 单事务批量提交
///
/// 生命周期:
/// 1. open(): 主线程打开数据库 + migration (不启动 Writer)
/// 2. (可选) db(): 主线程查询 (如 order_id 自检, 无竞争)
/// 3. start_writer(): 启动 Writer 线程
/// 4. (运行期) enqueue(): SPI 线程入队
/// 5. stop(): 主线程设置停止 + drain 残留 + join Writer + 关闭数据库
///
/// 队列硬上限 100000: 超限时 enqueue 阻塞 (cv 等待), 防止 OOM;
/// shutdown 中 enqueue 直接丢弃 (不阻塞).
///
/// shutdown 超时 30s: 超时后 std::quick_exit 跳过析构 (避免 double-free).
class PersistWriter {
public:
    /// 默认队列硬上限.
    static constexpr size_t kDefaultMaxQueueSize = 100000;

    /// shutdown 超时 (fsync 大事务兜底).
    static constexpr auto kShutdownTimeout = std::chrono::seconds(30);

    /// 构造 (不打开数据库). db_path 为数据库文件路径.
    /// max_queue_size 为测试可配 (生产用 kDefaultMaxQueueSize).
    explicit PersistWriter(std::string db_path, size_t max_queue_size = kDefaultMaxQueueSize);
    ~PersistWriter();

    PersistWriter(const PersistWriter&) = delete;
    PersistWriter& operator=(const PersistWriter&) = delete;
    PersistWriter(PersistWriter&&) = delete;
    PersistWriter& operator=(PersistWriter&&) = delete;

    /// 打开数据库 + 应用 migration (不启动 Writer 线程).
    /// 失败抛 std::runtime_error (启动期致命, 不登录 CTP).
    void open();

    /// 启动 Writer 线程 (open() 之后, 登录 CTP 之前调用).
    /// 重复调用是 no-op.
    void start_writer();

    /// 停止 Writer 线程 + 关闭数据库.
    /// 1. 设置 running_=false, notify 所有 cv
    /// 2. Writer 线程 drain 残留队列后退出
    /// 3. 主线程等待 Writer 退出 (30s 超时)
    /// 4. 超时则 std::quick_exit (不阻塞调用方)
    /// 5. 关闭预编译 stmt + 数据库
    /// 显式调用必须: 析构仅作 best-effort 兜底 (短超时, 不 quick_exit).
    void stop();

    /// 入队持久化任务 (线程安全).
    /// 队列满时阻塞 (cv 等待 queue_.size() < max_queue_size).
    /// shutdown 中 (running_=false) 直接丢弃 (不阻塞).
    void enqueue(PersistTask task);

    /// 获取数据库连接 (供主线程在 open() 后 start_writer() 前查询用).
    /// start_writer() 后调用抛 std::runtime_error (Writer 线程独占 db).
    SQLite::Database& db();

    /// 查询库内最大 order_id (orders + trades 两表取大), 供启动自检 (设计 §13 step 8).
    /// 调用时机与 db() 相同: open() 后 start_writer() 前 (主线程独占 db).
    /// 两表均为空时返回 0.
    [[nodiscard]] int64_t max_order_id();

private:
    void writer_loop();
    bool wait_and_pop(PersistTask& out);
    void execute_batch(SQLite::Database& db, std::vector<PersistTask>& batch);
    void prepare_statements(SQLite::Database& db);

    // 绑定单条记录到预编译 stmt 并执行
    void bind_order(SQLite::Statement& stmt, const OrderRecord& r);
    void bind_trade(SQLite::Statement& stmt, const TradeRecord& r);
    void bind_margin_rate(SQLite::Statement& stmt, const MarginRateRecord& r);
    void bind_commission_rate(SQLite::Statement& stmt, const CommissionRateRecord& r);
    void bind_instrument(SQLite::Statement& stmt, const InstrumentRecord& r);

    /// 析构兜底: 短超时 (1s) 等待 Writer 退出, 不 quick_exit.
    /// 主流程应显式调 stop() (30s 超时 + quick_exit 兜底).
    void stop_best_effort();

    std::string db_path_;
    size_t max_queue_size_;

    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<SQLite::Statement> stmt_insert_order_;
    std::unique_ptr<SQLite::Statement> stmt_insert_trade_;
    std::unique_ptr<SQLite::Statement> stmt_insert_margin_;
    std::unique_ptr<SQLite::Statement> stmt_insert_commission_;
    std::unique_ptr<SQLite::Statement> stmt_insert_instrument_;

    std::queue<PersistTask> queue_;
    std::mutex mtx_;
    std::condition_variable cv_empty_;       // 通知 Writer: 队列非空
    std::condition_variable cv_full_;        // 通知 enqueue: 队列非满
    std::condition_variable cv_writer_done_; // 通知 stop: Writer 已退出
    std::atomic<bool> running_{false};       // Writer 线程运行中
    std::atomic<bool> shutdown_{false};      // stop() 已调用 (区别于未启动, 防止 enqueue 误丢弃)
    std::atomic<bool> opened_{false};        // open() 已调用
    bool writer_started_ = false;            // start_writer() 已调用 (受 mtx_ 保护)
    bool writer_exited_ = false;             // Writer 线程已退出 (受 mtx_ 保护)
    bool explicit_stopped_ = false;          // I4: stop() 已显式调用, 析构 no-op
    std::thread writer_thread_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_WRITER_H_
