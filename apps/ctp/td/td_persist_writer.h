#ifndef DZTRADER_CTP_TD_PERSIST_WRITER_H_
#define DZTRADER_CTP_TD_PERSIST_WRITER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <dztrader/data_type.h>
#include <dztrader/tdstore/records.h>

#include "td/td_notify_types.h"
#include "td/td_persist_records.h"

namespace SQLite {
class Database;
class Statement;
}  // namespace SQLite

namespace dztrader::ctp {

/// 持久化任务 (队列元素, POD payload 避免 SPI 线程 to_json 抛异常).
/// 注意: data 必须是第二个成员 — td_account_session.cpp 用位置聚合初始化
/// PersistTask{Kind::X, rec}, 新增成员只能追加在尾部.
struct PersistTask {
    enum class Kind : uint8_t {
        Order,
        Trade,
        Instrument,
        Position,         // 单行绝对态 upsert (盘中有变化时走它)
        TradingAccount,   // 单行绝对态 upsert
        PositionRebuild,  // 单事务重灌: 清该账户当日持仓行 + upsert 本组 (ADR 0045)
        FlushSignal,      // FIFO 哨兵: 此前任务必已提交 (flush 屏障)
    } kind;

    /// Position / PositionRebuild / TradingAccount 用 (绝对态). TradingAccount 用单记录.
    /// NSDMI: FlushSignal 等不带 data 的 Kind 默认构造 (variant 第一个 alternative).
    std::variant<OrderRecord,
                 TradeRecord,
                 tdstore::InstrumentRecord,
                 std::vector<DzPositionInfo>,
                 DzTradingAccount>
        data = {};

    /// PositionRebuild / FlushSignal 用: 目标账户.
    /// NSDMI 使其在位置初始化 {Kind, rec} 时默认空串; 仅需 account_id 的
    /// Kind (TradingAccount/PositionRebuild) 走 designated 初始化显式填值 (见
    /// td_account_session.cpp).
    std::string account_id = {};
    /// Position / PositionRebuild / TradingAccount 用: 交易日 (DzDate 距纪元天数).
    int64_t trading_day = 0;
    /// FlushSignal 用: enqueue_flush_signal 返回的哨兵 token.
    uint64_t flush_token = 0;
};

/// 持久化失败通知回调 (ADR 0042 §4 分级语义): level/popup 由 Writer 决定, 调用方只转发.
/// 别名到进程内唯一类型 TdNotifyFn (td_notify_types.h); 线程约定见该类型注释.
/// 空 = 仅 spdlog.
using PersistNotifyFn = TdNotifyFn;

/// 持久化失败策略 (ADR 0042). 生产用默认值; 测试注入短延迟/低阈值实现确定性.
struct PersistPolicy {
    /// 失败批退避重试间隔 (逐次递增, 末值封顶): 1s -> 2s -> 5s -> 10s -> 30s.
    std::vector<std::chrono::milliseconds> retry_delays{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30)};
    /// 持续失败 WARN 通知限频 (ADR 0042 §4: 每 5 分钟最多 1 次).
    std::chrono::milliseconds sustained_notify_interval{std::chrono::minutes(5)};
    /// 积压阈值告警 (队列不设上限, 仅达阈值时通知一次/积压期; ADR 0042 §1).
    std::size_t backlog_warn_threshold = 100000;
};

/// 专用 SQLite Writer 线程 + 队列 (ADR 0042; 直连 SQLiteCpp).
///
/// 线程模型 (事件驱动, 无轮询):
/// - 调用线程: enqueue(task) 只做内存入队 (不设上限、不阻塞)
/// - Writer 线程: 等待队列非空, drain 为一批, 单事务提交; 提交成功才出队
///
/// 失败策略 (ADR 0042):
/// - 批事务失败: 整批回滚并保留, Writer 线程主动退避重试 (不依赖下一次业务事件);
/// - 建连失败: 同一退避序列重连 (td 不因交易库不可用而停止交易);
/// - 坏库 (CORRUPT/NOTADB/不可读): 关闭连接, 下次建连走 tdstore 建库门面
///   (旧库改名保留 + 新建), 记 ERROR; 不在同一坏文件上无限重试;
/// - 通知分级: 首次失败 ERROR 弹窗 1 次 / 持续每 5 分钟最多 1 次 WARN 弹窗 / 恢复 INFO 1 次;
/// - 退出排空失败: 只记 ERROR 并退出 (未提交内存积压丢弃, 已知限制).
///
/// 生命周期:
/// 1. open(): 首次建连尝试 (缺失库补建 / 坏库改名重建; 失败不抛, Writer 线程退避重连)
/// 2. start_writer(): 启动 Writer 线程
/// 3. (运行期) enqueue(): 调用线程入队
/// 4. stop(): 停止 + 排空残留 + join Writer + 关闭数据库
class PersistWriter {
public:
    /// shutdown 等待上限 (fsync 大事务兜底).
    static constexpr auto kShutdownTimeout = std::chrono::seconds(30);

    /// 构造 (不连接数据库). db_path 为交易库文件路径; gateway 为写入来源 td 实例名
    /// (schema v1 审计列, 不进唯一键). policy/notify 为空用生产默认/仅日志.
    explicit PersistWriter(std::string db_path,
                           std::string gateway,
                           PersistPolicy policy = {},
                           PersistNotifyFn notify = {});
    ~PersistWriter();

    PersistWriter(const PersistWriter&) = delete;
    PersistWriter& operator=(const PersistWriter&) = delete;
    PersistWriter(PersistWriter&&) = delete;
    PersistWriter& operator=(PersistWriter&&) = delete;

    /// 首次建连尝试 (不启动 Writer 线程). 缺失库/旧版本/损坏库经 tdstore 建库门面
    /// 处理 (master 负责建库; td 侧兜底为幂等); 失败只记 ERROR + 通知, 不抛异常 —
    /// 交易库不是交易真相源, 持久化降级不得阻塞交易 (ADR 0042 §5).
    void open();

    /// 启动 Writer 线程 (open() 之后). 重复调用是 no-op.
    void start_writer();

    /// 停止 Writer 线程 + 关闭数据库: 唤醒 Writer, 对失败批做最后一次立即重试,
    /// 成功则继续排空队列后退出; 仍失败只记 ERROR 并退出 (未提交积压丢弃).
    /// 显式调用必须: 析构仅作 best-effort 兜底.
    void stop();

    /// 入队持久化任务 (线程安全, 不设上限, 不阻塞). stop 后丢弃 (FlushSignal 丢弃
    /// 路径立即 set_value, 保持既有"哨兵被消费即视为提交完成"语义).
    void enqueue(PersistTask task);

    /// 入队 flush 哨兵 (线程安全): FIFO 保证此前任务必已提交.
    /// 返回自增 token, 供 wait_flush 使用. stop() 后入队丢弃路径会立即 set_value,
    /// wait_flush 恒返回 true (哨兵被"消费"即视为提交完成语义).
    uint64_t enqueue_flush_signal();

    /// 等待哨兵被处理 (内部 promise/future). 哨兵被消费 (提交完成 或 stop 丢弃) 前阻塞.
    /// 提交成功返回 true; 超时 / 批最终失败 / 退出排空失败返回 false.
    bool wait_flush(uint64_t token, std::chrono::milliseconds timeout);

private:
    /// 建连尝试 (open()/Writer 线程): ensure_trade_db -> 打开连接 -> 连接级 PRAGMA ->
    /// 语句失效重建. 失败返回 false 并记录 last_error_.
    bool try_open();
    void close_connection();
    void reset_statements();
    [[nodiscard]] SQLite::Statement& statement_for(PersistTask::Kind kind);
    [[nodiscard]] SQLite::Statement& position_delete_statement();

    void writer_loop();
    /// 等待并取走整批 (drain). 返回 false = 已停止且队列空.
    bool take_batch(std::vector<PersistTask>& batch);
    /// 单事务执行一批 (按原始序, FIFO 载重). 成功 commit 返回 true; 失败整批回滚返回 false,
    /// last_error_ 记录原因; 损坏库时关闭连接交给下一轮建连重建.
    bool commit_batch(std::vector<PersistTask>& batch);
    /// 批执行命中损坏库 (CORRUPT/NOTADB): 关闭连接交给下一轮建连改名重建;
    /// 连续重建超上限后只退避重试 (不再产生 .old-* 副本).
    void handle_corruption(int primary_code);
    /// 提交成功后 set_value 本批全部 FlushSignal token.
    void signal_flush_tokens(const std::vector<PersistTask>& batch);

    /// 退避等待. 返回 false = 收到停止请求 (应转入最后一次立即重试).
    bool wait_retry_or_stop(size_t attempt);

    /// 通知分级 (ADR 0042 §4). Writer 启动后仅 Writer 线程访问; open() 首次建连
    /// (Writer 线程启动前) 在主线程调用本函数, 两者不并发。
    void note_failure(const std::string& message);
    void note_recovered();
    void emit_notify(DzNotifyLevel level, const std::string& message, bool popup) const;

    /// stop/析构兜底: 等待 Writer 退出 (best-effort 短超时, 不 quick_exit).
    void stop_best_effort();
    /// 退出前终结全部未完成的 flush 等待者 (set_exception + erase, 不泄漏不永久阻塞).
    void fail_pending_flush_waiters();

    /// DzDate (距纪元天数) -> "YYYYMMDD" 文本 (DB date 列).
    static std::string format_date(int64_t days);
    /// SQLite 主 result code 是否属于"库文件损坏" (CORRUPT/NOTADB).
    static bool is_corruption(int sqlite_result_code);

    std::string db_path_;
    std::string gateway_;
    PersistPolicy policy_;
    PersistNotifyFn notify_;

    /// Writer 线程独占连接 + 预编译语句 (失败/重连后语句失效重建).
    std::unique_ptr<SQLite::Database> db_;
    std::unique_ptr<SQLite::Statement> stmt_order_;
    std::unique_ptr<SQLite::Statement> stmt_trade_;
    std::unique_ptr<SQLite::Statement> stmt_instrument_;
    std::unique_ptr<SQLite::Statement> stmt_position_;
    std::unique_ptr<SQLite::Statement> stmt_trading_account_;
    std::unique_ptr<SQLite::Statement> stmt_position_delete_;
    bool connected_ = false;
    /// 连续损坏重建计数 (成功提交后清零; 超过上限不再改名重建, 只退避重试).
    int corruption_rebuilds_ = 0;
    /// 最近一次建连/批执行失败原因 (通知/日志用; Writer 线程独占).
    std::string last_error_;

    std::deque<PersistTask> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;              // 通知 Writer: 队列非空 / 停止
    std::condition_variable cv_writer_done_;  // 通知 stop: Writer 已退出
    std::atomic<bool> running_{false};        // Writer 线程运行中
    bool shutdown_ = false;                   // stop() 已调用 (受 mtx_ 保护)
    bool writer_started_ = false;             // start_writer() 已调用 (受 mtx_ 保护)
    bool writer_exited_ = false;              // Writer 线程已退出 (受 mtx_ 保护)
    bool explicit_stopped_ = false;           // stop() 已显式调用, 析构 no-op
    bool opened_ = false;                     // open() 已调用
    bool backlog_warned_ = false;             // 积压告警已发 (回落后重新武装)
    std::thread writer_thread_;

    /// flush 哨兵: token -> promise (Writer 线程 set_value/set_exception). 受 mtx_ 保护.
    uint64_t next_flush_token_ = 1;
    std::unordered_map<uint64_t, std::promise<void>> pending_flushes_;

    /// 通知分级状态 (Writer 启动后仅 Writer 线程访问; open() 首次建连在主线程访问,
    /// 两者不并发).
    bool failing_ = false;
    std::chrono::steady_clock::time_point last_notify_{};
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_PERSIST_WRITER_H_
