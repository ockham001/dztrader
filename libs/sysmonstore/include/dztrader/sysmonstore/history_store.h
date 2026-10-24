#ifndef DZTRADER_SYSMONSTORE_HISTORY_STORE_H_
#define DZTRADER_SYSMONSTORE_HISTORY_STORE_H_

/**
 * @file history_store.h
 * @brief 历史库的读写门面：写入方（`dzsysmon`）唯一写者，读取方（`dzweb`）只读。
 *
 * 分工（#41 spec「历史存储」）：
 * - 写入方：`HistoryStore`（建库/迁移/批量提交/保留与压缩），由**落盘线程**独占使用；
 * - 读取方：`open_read_only` 返回的 `HistoryReader`，只读连接，库缺失或 schema 版本不符时
 *   优雅降级为「历史不可用」（不崩溃、不按旧结构解码、不返回伪造数据）。
 *
 * 线程约定：`HistoryStore` 与 `HistoryReader` 均**非线程安全**，由调用方保证单线程访问
 * （写入方是落盘线程，读取方是 dzweb 的查询线程）。这是有意为之：SQLite 让每个连接
 * 绑定一个线程语义最省心，跨线程共享连接需要额外互斥，而历史库的吞吐不需要。
 *
 * 为什么直接用 SQLiteCpp 而不是 `dzdb` 端口：保留与压缩需要 `GROUP BY` 加权均值、
 * `DELETE ... LIMIT` 与 `WITHOUT ROWID` 复合主键，`dzdb` 的 `Session` 抽象不暴露原始 SQL；
 * 而历史库是本地单文件、只此一种后端（不像 td 库需要 sqlite/mysql 切换），
 * 套一层端口只会多一份无法被满足的能力声明。
 */

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "records.h"
#include "schema.h"

namespace dztrader::sysmonstore {

/// 读取方打开结果：`ok` 之外的取值都是「历史不可用」的具体原因，供 REST/UI 区分文案。
enum class HistoryAvailability {
    Ok,              ///< 可读且 schema 版本匹配
    NotFound,        ///< 库文件不存在（dzsysmon 未启用历史，或尚未落过盘）
    SchemaMismatch,  ///< schema 版本不符（新写入方 / 旧读取方等）——必须降级，不得按旧结构解码
    OpenFailed,      ///< 打开失败（权限、损坏、非 SQLite 文件）
};

/// 把可用性转成稳定的 ASCII 原因码（REST/日志用；不是给用户看的文案）。
[[nodiscard]] std::string_view history_availability_reason(
    HistoryAvailability availability) noexcept;

// ===== 写入方 =====

/**
 * @brief 写入方门面：建库、迁移、批量提交、保留与压缩。
 *
 * 由落盘线程独占使用。所有写操作在各自事务内完成：批量提交一个事务、压缩与删除分事务，
 * 因此读取方（WAL 只读连接）不会看到半截数据。
 */
class HistoryStore {
public:
    /**
     * @brief 打开（必要时创建）历史库并应用 schema。
     *
     * 启用 WAL（`journal_mode=WAL`）、`busy_timeout`、`synchronous=FULL`（历史数据必须
     * 抗断电；历史库写入是低频批量，不是交易路径）。schema 版本比当前版本新时拒绝打开
     * （旧写入方不得覆盖新结构），比当前版本旧时按本版本重建（版本 1 尚无历史包袱）。
     *
     * @param path          库文件路径（父目录不存在时自动创建）
     * @param busy_timeout_ms 写锁等待超时（毫秒）
     * @throws std::runtime_error 目录创建/打开/建表/迁移失败（英文消息、小写开头、无句号）
     */
    static HistoryStore open(const std::filesystem::path& path, int busy_timeout_ms = 5000);

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;
    HistoryStore(HistoryStore&&) noexcept;
    HistoryStore& operator=(HistoryStore&&) noexcept;
    ~HistoryStore();

    /// 当前库文件中的 schema 版本（由 `sysmon_meta` 读取）。
    [[nodiscard]] int schema_version() const;

    /// 库文件路径。
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /**
     * @brief 提交一个批次：单个事务内写入宽表/长表/标记。
     *
     * 同桶重复提交按主键覆盖（后写优先），使「先入队、后重试」不会留下重复行。
     * 空批次为无操作。
     *
     * @throws std::runtime_error 写入失败（事务回滚，库内不留半截数据）
     */
    void write_batch(const HistoryBatch& batch);

    /**
     * @brief 执行保留与压缩策略（单个维护周期调用一次）。
     *
     * 顺序（顺序本身是语义的一部分）：
     * 1. **压缩**：`record_interval_sec < 60` 时（记录间隔比一分钟细），把超过
     *    `raw_retention_sec` 的原始桶聚合成一分钟桶写入 `level=1`；记录间隔已 ≥ 60 时
     *    不做压缩（原始记录本就是分钟级或更粗，压缩没有收益）。
     * 2. **删除超期**：原始记录按 `raw_retention_sec`、聚合记录按 `max_retention_sec` 删除。
     * 3. **总量上限**：库文件超过 `max_bytes` 时从**最旧**的数据开始删，直到回落到上限内。
     *
     * 每步按 `maintenance_batch` 分段，避免一次长事务把只读连接挡住整个维护周期。
     *
     * @throws std::runtime_error 事务失败（该步回滚并向上抛；已完成的步保留）
     */
    RetentionResult apply_retention(const RetentionPolicy& policy);

    /// 显式关库（落盘线程退出前调用；析构也会关，但显式关便于报告错误）。
    void close();

private:
    struct Impl;
    explicit HistoryStore(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
// ===== 读取方 =====

/**
 * @brief 只读历史库连接（供 `dzweb` 查历史，`#79`）。
 *
 * 只读连接**不试图**转换 journal_mode（WAL 转换需要写库头，只读连接必然失败），
 * 只设置 `busy_timeout` 与查询相关 PRAGMA。库缺失或版本不符时打开失败并给出
 * `HistoryAvailability`，调用方据此降级为「历史不可用」。
 */
class HistoryReader {
public:
    HistoryReader(const HistoryReader&) = delete;
    HistoryReader& operator=(const HistoryReader&) = delete;
    HistoryReader(HistoryReader&&) noexcept;
    HistoryReader& operator=(HistoryReader&&) noexcept;
    ~HistoryReader();

    /**
     * @brief 以只读方式打开历史库。
     *
     * 库文件不存在时返回 `{NotFound, nullptr}`（**不是**错误：dzsysmon 可能未启用历史）；
     * 文件存在但 schema 版本不符时返回 `{SchemaMismatch, nullptr}`；两者都由调用方
     * 落成「历史不可用」而不是异常。
     *
     * @param path            库文件路径
     * @param busy_timeout_ms 读锁等待超时（毫秒）；写入方正在提交时读取方等待而非失败
     * @return 可用性 + （仅 `Ok` 时）reader
     */
    static std::pair<HistoryAvailability, std::unique_ptr<HistoryReader>> open(
        const std::filesystem::path& path, int busy_timeout_ms = 2000);

    [[nodiscard]] int schema_version() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    /**
     * @brief 查询宽表时间序列。
     *
     * @param level 桶层级（`kLevelRaw` 原始 / `kLevelMinute` 分钟聚合）
     * @param since_ts 起始时刻（含，Unix 秒）
     * @param until_ts 结束时刻（不含，Unix 秒；`<= since_ts` 返回空）
     * @param max_rows 行数上限（> 0；超出时按时间升序截断）
     * @return 按 `bucket_ts` 升序的行
     */
    [[nodiscard]] std::vector<ScalarRecord> query_scalars(int32_t level,
                                                          int64_t since_ts,
                                                          int64_t until_ts,
                                                          int64_t max_rows) const;

    /**
     * @brief 按**时间降序**查询宽表时间序列（配合点数上限的精确截断判定）。
     *
     * 为什么需要降序：点数上限要求「保留最新 N 个桶」，而「是否还有更旧的桶」只能靠多取
     * 一行来确定；升序 + `LIMIT N+1` 拿到的是最旧的 N+1 行，判不出尾部是否还有数据。
     * 降序取的 `max_rows` 行就是最新的 `max_rows` 行，超出即说明丢了更旧的数据。
     *
     * @param level 桶层级
     * @param since_ts 起始时刻（含）
     * @param until_ts 结束时刻（不含；`<= since_ts` 返回空）
     * @param max_rows 行数上限（> 0）
     * @return 按 `bucket_ts` **降序**的行
     */
    [[nodiscard]] std::vector<ScalarRecord> query_scalars_desc(int32_t level,
                                                               int64_t since_ts,
                                                               int64_t until_ts,
                                                               int64_t max_rows) const;

    /**
     * @brief 查询长表中某个维度序列（如某网卡的收速率）。
     *
     * @param domain     域标识（见 `schema.h` 的 `k*Domain`）
     * @param dimension  维度标识（空串 = 不限维度，返回该域全部维度）
     * @param metric     指标名（空串 = 该域全部指标）
     * @param level      桶层级
     * @param since_ts   起始时刻（含）
     * @param until_ts   结束时刻（不含）
     * @param max_rows   行数上限（> 0）
     */
    [[nodiscard]] std::vector<DimensionRecord> query_dimensions(std::string_view domain,
                                                                std::string_view dimension,
                                                                std::string_view metric,
                                                                int32_t level,
                                                                int64_t since_ts,
                                                                int64_t until_ts,
                                                                int64_t max_rows) const;

    /**
     * @brief 按**时间降序**查询长表中某个维度序列（配合点数上限的精确截断判定）。
     *
     * @param domain     域标识
     * @param dimension  维度标识（**必须**非空：本版本每次只查一条序列）
     * @param metric     指标名（必须非空）
     * @param level      桶层级
     * @param since_ts   起始时刻（含）
     * @param until_ts   结束时刻（不含）
     * @param max_rows   行数上限（> 0）
     * @return 按 `bucket_ts` **降序**的行
     */
    [[nodiscard]] std::vector<DimensionRecord> query_dimensions_desc(std::string_view domain,
                                                                     std::string_view dimension,
                                                                     std::string_view metric,
                                                                     int32_t level,
                                                                     int64_t since_ts,
                                                                     int64_t until_ts,
                                                                     int64_t max_rows) const;

    /**
     * @brief 取已落库的最新一个桶的时刻（任意层级）。
     *
     * 用于「重启缺口秒数」这类只关心**最后一条记录**的场景：等价于
     * `query_scalars(...).back()` 但走 `MAX(bucket_ts)` 索引，不把整段历史读进内存。
     *
     * @param level 桶层级（`kLevelRaw` / `kLevelMinute`）
     * @return 最新桶的 `bucket_ts`；表内无该层级记录时为 `std::nullopt`
     */
    [[nodiscard]] std::optional<int64_t> latest_bucket_ts(int32_t level) const;

    /// 查询标记（采集中断/重启/时钟跳变），按 ts 升序。
    [[nodiscard]] std::vector<HistoryMark> query_marks(int64_t since_ts,
                                                       int64_t until_ts,
                                                       int64_t max_rows) const;

private:
    struct Impl;
    explicit HistoryReader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ===== 落盘线程 =====

/// 落盘线程配置。
struct HistoryWriterOptions {
    int busy_timeout_ms = 5000;              ///< 写锁等待超时（毫秒）
    size_t queue_capacity = 256;             ///< 有界队列容量（批次）；满时丢最旧并计数
    size_t max_batch_records = 4096;         ///< 单次事务合并的最大记录数（批量提交上限）
    int64_t maintenance_interval_sec = 300;  ///< 保留/压缩维护周期（秒）；0 = 不自动维护
};

/**
 * @brief 历史落盘线程：有界队列 + 批量提交。
 *
 * **采集线程只入队、永不阻塞**（#41「线程模型」）：数据库提交与 fsync 在磁盘繁忙时可阻塞
 * 数十至数百毫秒，放在 io_context 线程上会同时拖住采集（历史出现缺口）与帧处理
 * （优雅退出请求无法及时响应 ⇒ 被主进程强杀 ⇒ 重启循环）。
 *
 * 队列满时丢**最旧**的批次并计数（`dropped_records()`）——丢旧数据优于丢新数据：
 * 新数据还能解释「刚刚发生了什么」，且缺口会由维护周期记录成标记。
 *
 * 线程模型：`enqueue` 从采集线程调用（只锁队列、不碰数据库）；落盘线程独占
 * `HistoryStore`。`flush` 与析构会 join 落盘线程。
 */
class HistoryWriter {
public:
    /**
     * @param path        历史库路径
     * @param options     落盘参数
     * @param retention   保留策略提供者：维护周期到点时调用一次（在落盘线程上执行）。
     *                    传空表示不做保留维护（测试/关闭历史用）。
     * @throws std::runtime_error 库打开失败（历史不可用时由调用方降级为「不写历史」）
     */
    HistoryWriter(const std::filesystem::path& path,
                  HistoryWriterOptions options,
                  std::function<RetentionPolicy()> retention);

    HistoryWriter(const HistoryWriter&) = delete;
    HistoryWriter& operator=(const HistoryWriter&) = delete;
    HistoryWriter(HistoryWriter&&) = delete;
    HistoryWriter& operator=(HistoryWriter&&) = delete;

    /// 停止落盘线程并冲掉队列中剩余批次，随后关库。
    ~HistoryWriter();

    /**
     * @brief 入队一个批次（**不阻塞、不抛异常**）。
     *
     * 队列满时丢最旧批次并累加丢弃计数；落盘线程已停止时为无操作（退出路径）。
     */
    void enqueue(HistoryBatch&& batch) noexcept;

    /**
     * @brief 优雅退出：停止接收、冲掉队列中剩余批次、关库并 join。
     *
     * 幂等：重复调用与析构均安全。
     *
     * @return 冲盘失败的记录数（0 = 全部落盘成功，取自 `failed_records()`）；
     *         失败不抛（退出路径不能抛）
     */
    size_t flush() noexcept;

    /// 因队列满被丢弃的记录数（单调累加）。与 `failed_records()` 分开：
    /// 前者说明消费跟不上，后者说明写库失败（磁盘/库问题），处置方式不同。
    [[nodiscard]] uint64_t dropped_records() const noexcept;

    /// 落盘失败的记录数（单调累加）。
    [[nodiscard]] uint64_t failed_records() const noexcept;

    /// 已成功落盘的记录数（单调累加）。
    [[nodiscard]] uint64_t written_records() const noexcept;

    /// 落盘线程是否仍在运行。
    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dztrader::sysmonstore

#endif  // DZTRADER_SYSMONSTORE_HISTORY_STORE_H_
