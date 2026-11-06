/**
 * @file bench_td_read_path.cpp
 * @brief 交易库（trade.db）读路径 report-only 基准（issue #253 AC4）。
 *
 * 量的是 dzweb `GET /api/td/orders` 背后的直查路径，在**生产规模**上的耗时：
 * 20 个账户 / 20 万行 orders（每账户 1 万行）+ 2 万行 trades。建库走
 * `tdstore::ensure_trade_db`（schema 单一真相源：列/键/索引含 18:00 表达式索引
 * `idx_orders_sort`），查询走**生产实现** `TdOrdersQuery::execute`（不是基准里重写的 SQL）。
 *
 * 四项测量（口径见各项打印头）：
 * 1. **批量插入**：20 万行 orders / 2 万行 trades 各一个事务的总耗时与行/秒；
 * 2. **游标分页**：首页、连续 N 页、深页（~90% 深度）的 `execute()` 耗时；
 * 3. **18:00 排序表达式** `((time + 21600) % 86400)`：生产形状（20 账户 IN 列表）
 *    与单账户（索引可服务 ORDER BY）对比，并打印 `EXPLAIN QUERY PLAN` 作为证据；
 * 4. **`tdstore::read_max_order_id()`** 在 20 万行库上的耗时（master 启动期播种用）。
 *
 * ## 报告口径（与 docs/agents/build-appendix.md「性能基准」一致）
 *
 * - **只报告数值、不做阈值断言**（perf 标签、RUN_SERIAL、不进 fast 门禁）。
 * - 计时用 `steady_clock`：本基准的单项是毫秒级（SQL 扫描/排序），成对读表开销约几十 ns
 *   可忽略，不需要 RDTSC（那是 ns 级基准的口径）。每样本 = 一次完整 `execute()`。
 * - 分页项按「每页一次」采样，打印样本数 / 均值 / p50 / p99 / 最大值；插入项是整段事务耗时
 *   （batch_mean 口径），打印总耗时与行/秒。
 * - 分页耗时**含** `TdOrdersQuery` 每次调用独立打开只读连接 + `PRAGMA user_version` 校验
 *   （契约 rest §2.9.8 的真实 REST 路径），不是纯 SQL 时间。
 * - 临时库建在**项目内** `<repo>/.cache/bench_td_read_path_<pid>/`，结束即删；不碰
 *   `$DZTRADER_HOME`（AGENTS §一.2）。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh --target bench_td_read_path
 * ./build/linux/x86_64/Release/tests/bench_td_read_path --quick   # 秒级自测
 * ./build/linux/x86_64/Release/tests/bench_td_read_path           # 全规模（20 万行）
 * ./scripts/test.sh --label perf                                   # 经 ctest（含本基准）
 * ```
 */

#include "td_list_query.h"
#include "td_orders_query.h"
#include "td_read_db.h"

#include "bench_support.h"

#include <dztrader/core/this_process.h>
#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using dztrader::strategy_api::bench_support::build_type_name;
using dztrader::strategy_api::bench_support::compiler_name;
using dztrader::strategy_api::bench_support::cpu_brand;
using dztrader::strategy_api::bench_support::platform_name;
using dztrader::strategy_api::bench_support::quick_mode;
using dztrader::strategy_api::bench_support::Statistics;
using dztrader::strategy_api::bench_support::summarize_values;
using dztrader::webui::TdListParams;
using dztrader::webui::TdOrdersQuery;
using steady_clock = std::chrono::steady_clock;

// ============================================================
// 规模与采样参数
// ============================================================

/// 账户数（契约 rest §2.9.2 的 accounts 过滤与在线集合规模）
constexpr int kAccounts = 20;
/// 全规模：每账户 orders 行数（20 × 1 万 = 20 万）
constexpr int kRowsPerAccount = 10000;
/// `--quick`：每账户行数（20 × 200 = 4000，秒级自测）
constexpr int kQuickRowsPerAccount = 200;
/// 每账户 trades 行数（订单的 10%）
constexpr int kTradesPerAccount = 1000;
constexpr int kQuickTradesPerAccount = 20;
/// 分页页大小（契约 rest §2.9.2 的默认 200 行/页）
constexpr int kPageSize = 200;
/// 连续翻页的采样页数（首页之后）
constexpr int kContinuousPages = 10;
/// 走向深页时用的页大小（只用于取游标，不计入深页样本；游标作用域与 limit 无关）
constexpr int kDeepWalkLimit = 1000;
/// 各分页/单点项的采样次数（full / quick）
constexpr int kPageReps = 9;
constexpr int kQuickPageReps = 3;
/// `read_max_order_id` 采样次数
constexpr int kMaxOrderIdReps = 9;
constexpr int kQuickMaxOrderIdReps = 3;
/// 种子数据的交易日（TEXT "YYYYMMDD"；与运行日期无关，保证可比）
constexpr const char* kSeedTradingDay = "20261106";

// ============================================================
// 计时与输出
// ============================================================

double ms_between(steady_clock::time_point begin, steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::string today_text() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday);
}

/// 自检累计结果（不是性能阈值）：任何一条 `expect` 失败都会让 `main` 返回 1。
/// 用文件级累加而非要求每个调用点写 `checks_ok &=`：调用点有 13 处，漏写一处
/// 就等于该自检静默失效（本文件曾出现 10 处 `(void)expect(...)` 丢弃返回值的形态）。
bool g_checks_ok = true;

/// 自检断言（不是性能阈值）：失败打印统一诊断并累计到 `g_checks_ok`。
bool expect(bool condition, const char* check) {
    if (!condition) {
        std::fprintf(stderr, "self-check failed | check=%s\n", check);
        g_checks_ok = false;
    }
    return condition;
}

void print_stats(const char* label, const Statistics& stats, const char* unit) {
    std::printf("  %-38s samples=%lld mean=%.3f %s p50=%.3f p99=%.3f max=%.3f\n", label,
                static_cast<long long>(stats.count), stats.mean, unit, stats.p50, stats.p99,
                stats.max);
}

// ============================================================
// 临时库位置（项目内 .cache/，结束清理）
// ============================================================

/// 从可执行文件目录向上找仓库根（锚点 `AGENTS.md`，与 DSH 工作区根一致）。
std::filesystem::path project_root() {
    std::filesystem::path dir = dztrader::this_process::exe_dir();
    for (int depth = 0; depth < 8; ++depth) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "AGENTS.md", ec)) {
            return dir;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

// ============================================================
// 种子数据
// ============================================================

/// 一条委托种子行（与 `apps/webui/tests/td_order_row_writer.h` 的字段面一致；
/// 基准必须用**一条预编译语句**写 20 万行，不能复用那个逐行开连接的测试夹具）。
struct OrderSeedRow {
    std::string account_id;
    std::string date = kSeedTradingDay;
    int64_t order_id = 0;
    std::string strategy_id = "stg1";
    std::string instrument_id = "rb2601";
    std::string exchange_id = "SHFE";
    int64_t direction = 1;
    int64_t position_effect = 0;
    int64_t price_type = 0;
    int64_t status = 2;
    double price = 3500.0;
    int64_t volume = 1;
    int64_t volume_traded = 0;
    std::string remark;
    int64_t time = 32400;
    double avg_price = 0.0;
    int64_t seq = 1;
    std::string gateway = "dztd_ctp";
};

struct TradeSeedRow {
    std::string account_id;
    std::string date = kSeedTradingDay;
    std::string trade_id;
    int64_t order_id = 0;
    std::string strategy_id = "stg1";
    std::string instrument_id = "rb2601";
    std::string exchange_id = "SHFE";
    int64_t direction = 1;
    int64_t position_effect = 0;
    double price = 3500.0;
    int64_t volume = 1;
    int64_t time = 32400;
    int64_t seq = 1;
    std::string gateway = "dztd_ctp";
};

[[noreturn]] void fail_bind(const char* table, std::string_view column) {
    // 声明加列而基准没跟上时必须硬失败：否则会静默写入 NULL 让基准量到别的数据形状。
    throw std::runtime_error(
        std::format("bench seed cannot bind column | table={} column={}", table, column));
}

/// 由 schema 声明生成 INSERT（列清单不手抄：`tdstore` 是唯一真相源，加列时列清单自动跟上）。
std::string build_insert_sql(const dztrader::tdstore::TableSchema& table) {
    std::string sql = std::format("INSERT INTO \"{}\" (", table.name);
    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += std::format("\"{}\"", table.columns[i].name);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < table.columns.size(); ++i) {
        sql += i > 0 ? ", ?" : "?";
    }
    sql += ")";
    return sql;
}

/// 按 schema 声明序绑定一条 orders 行；未知列 = 硬失败。
void bind_order_row(SQLite::Statement& stmt, const OrderSeedRow& row) {
    const auto columns = dztrader::tdstore::orders_schema().columns;
    for (size_t i = 0; i < columns.size(); ++i) {
        const int index = static_cast<int>(i) + 1;
        const std::string_view name = columns[i].name;
        if (name == "account_id") {
            stmt.bind(index, row.account_id);
        } else if (name == "date") {
            stmt.bind(index, row.date);
        } else if (name == "order_id") {
            stmt.bind(index, row.order_id);
        } else if (name == "strategy_id") {
            stmt.bind(index, row.strategy_id);
        } else if (name == "instrument_id") {
            stmt.bind(index, row.instrument_id);
        } else if (name == "exchange_id") {
            stmt.bind(index, row.exchange_id);
        } else if (name == "direction") {
            stmt.bind(index, row.direction);
        } else if (name == "position_effect") {
            stmt.bind(index, row.position_effect);
        } else if (name == "price_type") {
            stmt.bind(index, row.price_type);
        } else if (name == "status") {
            stmt.bind(index, row.status);
        } else if (name == "price") {
            stmt.bind(index, row.price);
        } else if (name == "volume") {
            stmt.bind(index, row.volume);
        } else if (name == "volume_traded") {
            stmt.bind(index, row.volume_traded);
        } else if (name == "remark") {
            stmt.bind(index, row.remark);
        } else if (name == "time") {
            stmt.bind(index, row.time);
        } else if (name == "avg_price") {
            stmt.bind(index, row.avg_price);
        } else if (name == "seq") {
            stmt.bind(index, row.seq);
        } else if (name == "gateway") {
            stmt.bind(index, row.gateway);
        } else if (name == "ext") {
            stmt.bind(index, std::string("{}"));
        } else {
            fail_bind("orders", name);
        }
    }
}

void bind_trade_row(SQLite::Statement& stmt, const TradeSeedRow& row) {
    const auto columns = dztrader::tdstore::trades_schema().columns;
    for (size_t i = 0; i < columns.size(); ++i) {
        const int index = static_cast<int>(i) + 1;
        const std::string_view name = columns[i].name;
        if (name == "account_id") {
            stmt.bind(index, row.account_id);
        } else if (name == "date") {
            stmt.bind(index, row.date);
        } else if (name == "trade_id") {
            stmt.bind(index, row.trade_id);
        } else if (name == "order_id") {
            stmt.bind(index, row.order_id);
        } else if (name == "strategy_id") {
            stmt.bind(index, row.strategy_id);
        } else if (name == "instrument_id") {
            stmt.bind(index, row.instrument_id);
        } else if (name == "exchange_id") {
            stmt.bind(index, row.exchange_id);
        } else if (name == "direction") {
            stmt.bind(index, row.direction);
        } else if (name == "position_effect") {
            stmt.bind(index, row.position_effect);
        } else if (name == "price") {
            stmt.bind(index, row.price);
        } else if (name == "volume") {
            stmt.bind(index, row.volume);
        } else if (name == "time") {
            stmt.bind(index, row.time);
        } else if (name == "seq") {
            stmt.bind(index, row.seq);
        } else if (name == "gateway") {
            stmt.bind(index, row.gateway);
        } else if (name == "ext") {
            stmt.bind(index, std::string("{}"));
        } else {
            fail_bind("trades", name);
        }
    }
}

/// 合约元数据（4 个品种，覆盖不同 price_tick → 行 JSON 的精度派生路径真实执行）。
void insert_instruments(SQLite::Database& db) {
    static constexpr struct {
        const char* instrument_id;
        const char* exchange_id;
        double price_tick;
        double volume_multiple;
    } kInstruments[] = {
        {"rb2601", "SHFE", 1.0, 10.0},
        {"ag2612", "SHFE", 1.0, 15.0},
        {"cu2601", "SHFE", 10.0, 5.0},
        {"i2601", "DCE", 0.5, 100.0},
    };
    SQLite::Statement stmt(db,
                           "INSERT INTO \"instruments\" (\"instrument_id\", \"exchange_id\","
                           " \"price_tick\", \"volume_multiple\") VALUES (?, ?, ?, ?)");
    for (const auto& instrument : kInstruments) {
        stmt.bind(1, std::string(instrument.instrument_id));
        stmt.bind(2, std::string(instrument.exchange_id));
        stmt.bind(3, instrument.price_tick);
        stmt.bind(4, instrument.volume_multiple);
        stmt.exec();
        stmt.reset();
    }
}

/// 委托时间分布（覆盖日盘 / 夜盘的 18:00 锚点回卷；确定性序列保证跨运行可比）。
int64_t seed_time(int row_index) {
    switch (row_index % 10) {
        case 8:
            return 75600 + (static_cast<int64_t>(row_index) * 53) % 7200;  // 夜盘 21:00–23:00
        case 9:
            return 79200 + (static_cast<int64_t>(row_index) * 37) % 4800;  // 夜盘 22:00–23:20
        default:
            return 32400 + (static_cast<int64_t>(row_index) * 41) % 8100;  // 日盘 09:00–11:15
    }
}

std::string account_id_of(int account_index) { return std::format("ACC{:02d}", account_index); }

/// 批量写入 orders + trades（各一个事务），返回耗时（ms）与总行数由调用方打印。
struct SeedTiming {
    int64_t orders = 0;
    int64_t trades = 0;
    double orders_ms = 0.0;
    double trades_ms = 0.0;
    int64_t max_order_id = 0;
};

SeedTiming seed_all(SQLite::Database& db, int rows_per_account, int trades_per_account) {
    SeedTiming timing;
    timing.orders = static_cast<int64_t>(kAccounts) * rows_per_account;
    timing.trades = static_cast<int64_t>(kAccounts) * trades_per_account;

    // ---- orders：一个事务 20 万行（预编译语句，无逐行连接开销）----
    {
        SQLite::Statement stmt(db, build_insert_sql(dztrader::tdstore::orders_schema()));
        const auto begin = steady_clock::now();
        {
            SQLite::Transaction transaction(db);
            for (int account = 0; account < kAccounts; ++account) {
                for (int row = 0; row < rows_per_account; ++row) {
                    OrderSeedRow seed;
                    seed.account_id = account_id_of(account);
                    seed.order_id = static_cast<int64_t>(account) * rows_per_account + row + 1;
                    seed.seq = row + 1;
                    seed.instrument_id = (row % 4 == 0)   ? "rb2601"
                                         : (row % 4 == 1) ? "ag2612"
                                         : (row % 4 == 2) ? "cu2601"
                                                          : "i2601";
                    seed.exchange_id = seed.instrument_id == "i2601" ? "DCE" : "SHFE";
                    seed.direction = (row % 2 == 0) ? 1 : -1;
                    seed.position_effect = row % 3;
                    seed.price_type = row % 2;
                    seed.status = row % 6;
                    seed.price = 3500.0 + static_cast<double>(row % 200) * 0.5;
                    seed.volume = 1 + row % 3;
                    seed.volume_traded = row % 4;
                    seed.avg_price = seed.volume_traded > 0 ? seed.price + 0.5 : 0.0;
                    seed.remark = (row % 7 == 0) ? "seed" : "";
                    seed.strategy_id = std::format("stg{}", row % 5 + 1);
                    seed.time = static_cast<int64_t>(seed_time(row));
                    stmt.reset();
                    stmt.clearBindings();
                    bind_order_row(stmt, seed);
                    stmt.exec();
                    ++timing.max_order_id;
                }
            }
            transaction.commit();
        }
        timing.orders_ms = ms_between(begin, steady_clock::now());
    }

    // ---- trades：一个事务 2 万行 ----
    {
        SQLite::Statement stmt(db, build_insert_sql(dztrader::tdstore::trades_schema()));
        const auto begin = steady_clock::now();
        {
            SQLite::Transaction transaction(db);
            int64_t trade_index = 0;
            for (int account = 0; account < kAccounts; ++account) {
                for (int row = 0; row < trades_per_account; ++row) {
                    TradeSeedRow seed;
                    seed.account_id = account_id_of(account);
                    seed.trade_id = std::format("{:010d}", ++trade_index);
                    seed.order_id = static_cast<int64_t>(account) * rows_per_account + row + 1;
                    seed.instrument_id = (row % 4 == 0)   ? "rb2601"
                                         : (row % 4 == 1) ? "ag2612"
                                         : (row % 4 == 2) ? "cu2601"
                                                          : "i2601";
                    seed.exchange_id = seed.instrument_id == "i2601" ? "DCE" : "SHFE";
                    seed.direction = (row % 2 == 0) ? 1 : -1;
                    seed.position_effect = row % 3;
                    seed.price = 3500.0 + static_cast<double>(row % 200) * 0.5;
                    seed.volume = 1 + row % 3;
                    seed.time = static_cast<int64_t>(seed_time(row));
                    seed.seq = row + 1;
                    stmt.reset();
                    stmt.clearBindings();
                    bind_trade_row(stmt, seed);
                    stmt.exec();
                }
            }
            transaction.commit();
        }
        timing.trades_ms = ms_between(begin, steady_clock::now());
    }
    return timing;
}

// ============================================================
// 分页测量（走生产实现 TdOrdersQuery）
// ============================================================

struct PaginationResult {
    /// 冷启动样本：库刚写完（WAL 未 checkpoint）后的**第一次** execute()，不计入 first_page
    /// 分布（它混了首读页缓存 + WAL 恢复，会把均值抬高一倍以上）。
    double cold_first_page_ms = 0.0;
    Statistics first_page;
    Statistics continuous_page;
    Statistics deep_page;
    /// 连续翻页的逐页耗时（页序 = 1..N，便于看出「首页 vs 后续页」的差异来自哪个机制）。
    std::vector<double> continuous_page_samples;
    /// 首页之后的游标坐标（供 [3] 用同一坐标形状打印/计时「浅层游标页」）
    int32_t shallow_cursor_anchor = 0;
    int64_t shallow_cursor_order_id = 0;
    /// 深页游标（供 [3] 打印该页的查询计划）
    std::string deep_cursor;
    /// 深页最后一行的 (时间锚, order_id)：用于在 [3] 用同一游标形状打印查询计划
    int32_t deep_cursor_anchor = 0;
    int64_t deep_cursor_order_id = 0;
    int64_t deep_depth_rows = 0;
    int64_t deep_walk_pages = 0;
    double deep_walk_ms = 0.0;
    /// 深度扫描：{(深度百分比, 该深度单页 limit=200 耗时 ms)}，每点 1 个样本。
    /// 让「游标页成本随深度衰减」这件事有实测曲线，而不是只报一个 90% 点的数字。
    std::vector<std::pair<int, double>> depth_sweep;
};

std::optional<nlohmann::json> run_query(const TdOrdersQuery& query,
                                        int limit,
                                        const std::string& cursor,
                                        const std::vector<std::string>& online_accounts) {
    TdListParams params;
    params.limit = limit;
    params.cursor = cursor;
    return query.execute(params, online_accounts, std::string(kSeedTradingDay));
}

PaginationResult measure_pagination(const TdOrdersQuery& query,
                                    const std::vector<std::string>& online_accounts,
                                    int64_t total_rows,
                                    int page_reps) {
    PaginationResult result;

    // ---- 首页 ----
    // 冷启动单列：库刚由写连接关闭（WAL 尚未 checkpoint）时第一次读页最贵，
    // 计入分布会把均值抬高一倍以上（实测 quick 档 12.3ms vs 稳态 4ms）。
    std::vector<double> first_samples;
    std::string cursor;
    {
        const auto begin = steady_clock::now();
        const auto body = run_query(query, kPageSize, "", online_accounts);
        result.cold_first_page_ms = ms_between(begin, steady_clock::now());
        if (!expect(body.has_value() && (*body)["available"].get<bool>(), "cold first page")) {
            return result;
        }
        cursor = (*body)["next_cursor"].is_null() ? std::string{}
                                                  : (*body)["next_cursor"].get<std::string>();
        result.shallow_cursor_anchor =
            dztrader::webui::td_time_anchor((*body)["items"].back()["time"].get<int>());
        result.shallow_cursor_order_id = (*body)["items"].back()["order_id"].get<int64_t>();
    }
    for (int rep = 0; rep < page_reps; ++rep) {
        const auto begin = steady_clock::now();
        const auto body = run_query(query, kPageSize, "", online_accounts);
        first_samples.push_back(ms_between(begin, steady_clock::now()));
        if (!expect(body.has_value() && (*body)["available"].get<bool>(), "first page available")) {
            return result;
        }
        cursor = (*body)["next_cursor"].is_null() ? std::string{}
                                                  : (*body)["next_cursor"].get<std::string>();
    }
    result.first_page = summarize_values(first_samples);

    // ---- 连续翻页（首页之后 N 页；每页一次采样）----
    std::vector<double> page_samples;
    std::vector<int64_t> seen_order_ids;
    for (int page = 0; page < kContinuousPages && !cursor.empty(); ++page) {
        const auto begin = steady_clock::now();
        const auto body = run_query(query, kPageSize, cursor, online_accounts);
        page_samples.push_back(ms_between(begin, steady_clock::now()));
        if (!body.has_value() || !(*body)["available"].get<bool>()) {
            (void)expect(false, "continuous page available");
            return result;
        }
        for (const auto& item : (*body)["items"]) {
            seen_order_ids.push_back(item["order_id"].get<int64_t>());
        }
        cursor = (*body)["next_cursor"].is_null() ? std::string{}
                                                  : (*body)["next_cursor"].get<std::string>();
    }
    result.continuous_page = summarize_values(page_samples);
    result.continuous_page_samples = page_samples;
    // 自检：跨页不重复（游标分页契约 rest §2.9.2 的「不重不漏」在采样范围内成立）
    {
        std::vector<int64_t> sorted = seen_order_ids;
        std::ranges::sort(sorted);
        (void)expect(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
                     "sampled pages have no duplicate order_id");
    }

    // ---- 深度扫描 + 深页定位（一次走路，两件事）----
    // 游标页的成本不只看页大小：游标把 WHERE 变成「各账户 (anchor, order_id) 范围」，
    // 剩余行越多、走的索引分支越多，单页越贵。所以沿路在若干深度各取一个 limit=200 页
    // 的单样本，把「成本随深度衰减」画出来；最后在 ~90% 深度停住供 [2]/[3] 取样。
    // 走路耗时只报告，不计入深页样本（深页量的是该位置单页成本，不是走到那里的成本）。
    static constexpr int kDepthPercents[] = {1, 10, 25, 50, 75, 90, 99};
    std::string walk_cursor;
    int64_t walked_rows = 0;
    size_t next_depth = 0;
    const auto walk_begin = steady_clock::now();
    while (next_depth < std::size(kDepthPercents)) {
        const int64_t depth_target =
            static_cast<int64_t>(static_cast<double>(total_rows) *
                                 static_cast<double>(kDepthPercents[next_depth]) / 100.0);
        if (walked_rows >= depth_target) {
            const auto begin = steady_clock::now();
            const auto body = run_query(query, kPageSize, walk_cursor, online_accounts);
            const double elapsed = ms_between(begin, steady_clock::now());
            if (!body.has_value() || !(*body)["available"].get<bool>()) {
                (void)expect(false, "depth sweep page available");
                return result;
            }
            result.depth_sweep.emplace_back(kDepthPercents[next_depth], elapsed);
            if (kDepthPercents[next_depth] == 90) {
                // 90% 深度的游标：深页本体与 [3] 的游标探针共用
                result.deep_cursor = walk_cursor;
                const auto& rows = (*body)["items"];
                (void)expect(!rows.empty(), "depth sweep 90% returns rows");
                result.deep_cursor_anchor =
                    dztrader::webui::td_time_anchor(rows.back()["time"].get<int>());
                result.deep_cursor_order_id = rows.back()["order_id"].get<int64_t>();
            }
            ++next_depth;
            continue;
        }
        const auto body = run_query(query, kDeepWalkLimit, walk_cursor, online_accounts);
        if (!body.has_value() || !(*body)["available"].get<bool>()) {
            (void)expect(false, "deep walk page available");
            return result;
        }
        walked_rows += static_cast<int64_t>((*body)["items"].size());
        ++result.deep_walk_pages;
        if ((*body)["next_cursor"].is_null()) {
            // 走到底：剩余深度点用同一游标（游标已到列表尾，页面为空）不再采样
            break;
        }
        walk_cursor = (*body)["next_cursor"].get<std::string>();
    }
    result.deep_walk_ms = ms_between(walk_begin, steady_clock::now());
    result.deep_depth_rows = walked_rows;

    // ---- 深页本体：同一深游标重复采样（页大小回到生产默认 200）----
    std::vector<double> deep_samples;
    for (int rep = 0; rep < page_reps && !result.deep_cursor.empty(); ++rep) {
        const auto begin = steady_clock::now();
        const auto body = run_query(query, kPageSize, result.deep_cursor, online_accounts);
        deep_samples.push_back(ms_between(begin, steady_clock::now()));
        if (!expect(body.has_value() && (*body)["available"].get<bool>(), "deep page available")) {
            return result;
        }
        (void)expect(!(*body)["items"].empty(), "deep page returns rows");
        const auto& rows = (*body)["items"];
        result.deep_cursor_anchor = dztrader::webui::td_time_anchor(rows.back()["time"].get<int>());
        result.deep_cursor_order_id = rows.back()["order_id"].get<int64_t>();
    }
    result.deep_page = summarize_values(deep_samples);
    return result;
}

// ============================================================
// 18:00 排序表达式测量
// ============================================================

/// 委托列表分页游标的 SQL 坐标条件（与 `td_orders_query.cpp` 的 `EXECUTE` 形状一致）。
struct ProbeCursor {
    bool present = false;
    int32_t anchor = 0;
    int64_t order_id = 0;
};

/// 基准内重建的「同一 SQL 形状」探针：与 `TdOrdersQuery::execute` 的主查询只差
/// ORDER BY 一项（`anchor_order=false` 时用 `o.time` 作对照）与可选游标条件。
/// 仅用于隔离排序表达式的成本与打印查询计划，**不是**第二条生产实现
/// （生产查询仍走 `TdOrdersQuery`）。
std::string probe_sql(size_t account_count, bool anchor_order, const ProbeCursor& cursor = {}) {
    std::string sql =
        "SELECT o.account_id, o.date, o.order_id, o.strategy_id, o.instrument_id,"
        " o.exchange_id, o.direction, o.position_effect, o.price_type, o.status,"
        " o.price, o.volume, o.volume_traded, o.remark, o.time, o.avg_price, o.seq,"
        " i.price_tick"
        " FROM orders o LEFT JOIN instruments i ON i.instrument_id = o.instrument_id"
        " WHERE o.date = ?1 AND o.account_id IN (";
    for (size_t i = 0; i < account_count; ++i) {
        sql += i == 0 ? "?" : ",?";
    }
    sql += ")";
    const std::string anchor = dztrader::webui::td_time_anchor_sql("o");
    if (cursor.present) {
        sql += std::format(" AND ({} < ? OR ({} = ? AND o.order_id < ?))", anchor, anchor);
    }
    sql += anchor_order ? std::format(" ORDER BY {} DESC, o.order_id DESC LIMIT ?", anchor)
                        : std::string(" ORDER BY o.time DESC, o.order_id DESC LIMIT ?");
    return sql;
}

void bind_probe_args(SQLite::Statement& stmt,
                     const std::vector<std::string>& accounts,
                     int limit,
                     const ProbeCursor& cursor = {}) {
    int index = 1;
    stmt.bind(index++, std::string(kSeedTradingDay));
    for (const auto& account : accounts) {
        stmt.bind(index++, account);
    }
    if (cursor.present) {
        stmt.bind(index++, cursor.anchor);
        stmt.bind(index++, cursor.anchor);
        stmt.bind(index++, cursor.order_id);
    }
    stmt.bind(index, limit);
}

/// 在同一只读连接上执行一次探针 SQL，返回耗时（ms）。
///
/// @param expect_full_page 是否自检「返回满页」。**深游标探针必须传 false**：
///        cursor 已接近该账户数据末端时，剩余行本就不足一页（`--quick` 档每账户只有
///        `kQuickRowsPerAccount` 行，深游标之后常为空），此时「不足一页」是**正确结果**
///        而不是失败。这个参数存在的原因：早期版本对所有调用点无条件自检满页，
///        在 `--quick` 档恒失败（且该失败曾被 `(void)expect` 静默丢弃）。
double time_probe_once(SQLite::Database& db,
                       const std::string& sql,
                       const std::vector<std::string>& accounts,
                       const ProbeCursor& cursor = {},
                       bool expect_full_page = true) {
    const auto begin = steady_clock::now();
    SQLite::Statement stmt(db, sql);
    bind_probe_args(stmt, accounts, kPageSize + 1, cursor);
    int64_t rows = 0;
    while (stmt.executeStep()) {
        ++rows;
    }
    const double elapsed = ms_between(begin, steady_clock::now());
    if (expect_full_page) {
        (void)expect(rows >= kPageSize, "probe sql returns a full page");
    }
    return elapsed;
}

/// 同一只读连接上重复执行探针 SQL（连接打开一次 → 隔离 SQL 成本）。
std::vector<double> time_probe(SQLite::Database& db,
                               const std::string& sql,
                               const std::vector<std::string>& accounts,
                               int reps,
                               const ProbeCursor& cursor = {},
                               bool expect_full_page = true) {
    std::vector<double> samples;
    for (int rep = 0; rep < reps; ++rep) {
        samples.push_back(time_probe_once(db, sql, accounts, cursor, expect_full_page));
    }
    return samples;
}

/// 交错采样两个探针（A/B/A/B…）：页缓存与 CPU 频率在同一次运行内漂移时，
/// 顺序执行会把漂移全算到「后跑的那一个」头上（实测顺序采样下 delta 在 +1.1~+6.1ms 间摆动）。
std::pair<std::vector<double>, std::vector<double>> time_probe_pair(
    SQLite::Database& db,
    const std::string& sql_a,
    const std::string& sql_b,
    const std::vector<std::string>& accounts,
    int reps) {
    std::vector<double> a;
    std::vector<double> b;
    for (int rep = 0; rep < reps; ++rep) {
        a.push_back(time_probe_once(db, sql_a, accounts));
        b.push_back(time_probe_once(db, sql_b, accounts));
    }
    return {std::move(a), std::move(b)};
}

std::pair<std::vector<double>, std::vector<double>> time_execute_vs_probe(
    const dztrader::webui::TdOrdersQuery& query,
    SQLite::Database& probe_db,
    const std::string& sql,
    const std::vector<std::string>& online_accounts,
    int reps) {
    std::vector<double> production;
    std::vector<double> probe;
    dztrader::webui::TdListParams params;
    params.limit = kPageSize;
    for (int rep = 0; rep < reps; ++rep) {
        {
            const auto begin = steady_clock::now();
            const auto body = query.execute(params, online_accounts, std::string(kSeedTradingDay));
            production.push_back(ms_between(begin, steady_clock::now()));
            (void)expect(body.has_value() && (*body)["available"].get<bool>(),
                         "paired production page available");
        }
        probe.push_back(time_probe_once(probe_db, sql, online_accounts));
    }
    return {std::move(production), std::move(probe)};
}

Statistics paired_delta(const std::vector<double>& a, const std::vector<double>& b) {
    std::vector<double> delta;
    delta.reserve(a.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        delta.push_back(a[i] - b[i]);
    }
    return summarize_values(delta);
}

void print_query_plan(SQLite::Database& db,
                      const std::string& sql,
                      const std::vector<std::string>& accounts,
                      const char* label,
                      const ProbeCursor& cursor = {}) {
    std::printf("  query plan (%s):\n", label);
    SQLite::Statement stmt(db, "EXPLAIN QUERY PLAN " + sql);
    bind_probe_args(stmt, accounts, kPageSize + 1, cursor);
    while (stmt.executeStep()) {
        std::printf("    %s\n", stmt.getColumn(3).getText());
    }
}

// ============================================================
// main
// ============================================================

}  // namespace

int main(int argc, char** argv) {
    const bool quick = quick_mode(argc, argv);
    const int rows_per_account = quick ? kQuickRowsPerAccount : kRowsPerAccount;
    const int trades_per_account = quick ? kQuickTradesPerAccount : kTradesPerAccount;
    const int page_reps = quick ? kQuickPageReps : kPageReps;
    const int max_order_id_reps = quick ? kQuickMaxOrderIdReps : kMaxOrderIdReps;
    const int64_t total_rows = static_cast<int64_t>(kAccounts) * rows_per_account;

    // ---- 报告头：机器 / 构建 / 规模（数值可复现的前提）----
    std::printf("bench_td_read_path (#253 AC4) - trade.db read path\n");
    std::printf(
        "scope: seed a production-shaped trade.db (20 accounts / %lld orders / %lld trades), then "
        "measure TdOrdersQuery cursor pagination, the 18:00 sort expression, and MAX(order_id)\n",
        static_cast<long long>(total_rows),
        static_cast<long long>(static_cast<int64_t>(kAccounts) * trades_per_account));
    std::printf("date: %s | mode: %s\n", today_text().c_str(), quick ? "quick" : "full");
    std::printf("build: %s | compiler: %s | platform: %s\n", build_type_name().c_str(),
                compiler_name().c_str(), platform_name().c_str());
    std::printf("cpu: %s | logical_cpus=%u\n", cpu_brand().c_str(),
                std::thread::hardware_concurrency());
    std::printf(
        "timer: steady_clock (pair overhead is tens of ns, negligible at ms scale) | "
        "report-only, no threshold assertions\n");
    std::printf(
        "pagination scope: limit=%d, online_accounts=%d, trading_day=%s, every execute() opens "
        "its own read-only connection + schema-version probe\n",
        kPageSize, kAccounts, kSeedTradingDay);
#ifndef NDEBUG
    std::printf("warning: debug build, numbers are not representative of production\n");
#endif
    std::printf("\n");

    const std::filesystem::path root = project_root();
    if (root.empty()) {
        std::fprintf(stderr,
                     "project root not found | hint=\"run from the build tree inside the repo\"\n");
        return 1;
    }
    const std::filesystem::path temp_dir =
        root / ".cache" / std::format("bench_td_read_path_{}", dztrader::this_process::pid());
    {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);
        if (ec) {
            std::fprintf(stderr, "temp dir create failed | path=%s error=\"%s\"\n",
                         temp_dir.string().c_str(), ec.message().c_str());
            return 1;
        }
    }
    const std::string db_path = (temp_dir / "trade.db").string();
    bool checks_ok = true;
    int exit_code = 0;

    try {
        // ---- 建库（tdstore 门面：schema v1 + 表达式索引的唯一入口）----
        const auto db_begin = steady_clock::now();
        const auto report = dztrader::tdstore::ensure_trade_db(db_path);
        const double db_ms = ms_between(db_begin, steady_clock::now());
        std::printf(
            "[setup] ensure_trade_db (schema v1 + 5 tables + indexes incl. idx_orders_sort)\n");
        std::printf(
            "  created=%s wall=%.3f ms path=%s\n",
            report.status == dztrader::tdstore::TradeDbInitReport::Status::Created ? "yes" : "no",
            db_ms, db_path.c_str());

        // 写连接只在 seed 期间持有（析构即关闭；SQLiteCpp 无显式 close）。
        SeedTiming seed;
        {
            SQLite::Database db(db_path, SQLite::OPEN_READWRITE);
            db.exec("PRAGMA busy_timeout=10000");
            db.exec("PRAGMA synchronous=FULL");
            db.exec("PRAGMA cache_size=-8000");
            db.exec("PRAGMA temp_store=MEMORY");
            insert_instruments(db);

            // ---- 1. 批量插入 ----
            seed = seed_all(db, rows_per_account, trades_per_account);

            // 自检：行数与种子上限一致（路径身份，不是性能阈值）
            SQLite::Statement count_orders(db, "SELECT COUNT(*) FROM orders");
            (void)count_orders.executeStep();
            checks_ok &= expect(count_orders.getColumn(0).getInt64() == seed.orders,
                                "orders row count matches seed");
            SQLite::Statement count_trades(db, "SELECT COUNT(*) FROM trades");
            (void)count_trades.executeStep();
            checks_ok &= expect(count_trades.getColumn(0).getInt64() == seed.trades,
                                "trades row count matches seed");
        }
        std::printf("\n[1] bulk insert (one transaction per table, prepared statement)\n");
        std::printf("  orders  rows=%lld wall=%.1f ms throughput=%.0f rows/s\n",
                    static_cast<long long>(seed.orders), seed.orders_ms,
                    static_cast<double>(seed.orders) / (seed.orders_ms / 1000.0));
        std::printf("  trades  rows=%lld wall=%.1f ms throughput=%.0f rows/s\n",
                    static_cast<long long>(seed.trades), seed.trades_ms,
                    static_cast<double>(seed.trades) / (seed.trades_ms / 1000.0));
        std::printf(
            "  note: production writes go through td PersistWriter in per-batch transactions "
            "(synchronous=FULL); this single-transaction bulk seed is the throughput ceiling\n");

        // ---- 2. 游标分页（生产实现 TdOrdersQuery）----
        std::vector<std::string> online_accounts;
        online_accounts.reserve(static_cast<size_t>(kAccounts));
        for (int account = 0; account < kAccounts; ++account) {
            online_accounts.push_back(account_id_of(account));
        }
        const TdOrdersQuery query([&db_path]() { return db_path; });
        const PaginationResult pagination =
            measure_pagination(query, online_accounts, total_rows, page_reps);
        // 连接打开成本单列：分页项每页都独立开只读连接 + PRAGMA user_version 校验
        // （契约 rest §2.9.8 的真实 REST 路径）。拆出来才能说明「execute() 比纯 SQL 贵多少」。
        std::vector<double> open_samples;
        for (int rep = 0; rep < page_reps; ++rep) {
            const auto begin = steady_clock::now();
            {
                const dztrader::webui::TdReadConnection conn =
                    dztrader::webui::TdReadConnection::open(db_path, 200);
                (void)expect(conn.connected(), "read connection opens");
            }
            open_samples.push_back(ms_between(begin, steady_clock::now()));
        }
        const Statistics open_stats = summarize_values(open_samples);

        std::printf("\n[2] cursor pagination via TdOrdersQuery (production code path, limit=%d)\n",
                    kPageSize);
        std::printf("  cold first page (single sample, WAL not checkpointed)  %.3f ms\n",
                    pagination.cold_first_page_ms);
        print_stats("first page (warm, no cursor)", pagination.first_page, "ms");
        print_stats("connection open + schema probe", open_stats, "ms");
        print_stats("continuous pages (after first)", pagination.continuous_page, "ms");
        print_stats("deep page @~90% depth", pagination.deep_page, "ms");
        std::printf("  continuous page sequence (ms, in order): ");
        for (const double sample : pagination.continuous_page_samples) {
            std::printf("%.1f ", sample);
        }
        std::printf("\n");
        std::printf(
            "  deep sampling depth_rows=%lld (walked in %lld pages of limit=%d, walk_wall=%.1f ms "
            "not counted as samples)\n",
            static_cast<long long>(pagination.deep_depth_rows),
            static_cast<long long>(pagination.deep_walk_pages), kDeepWalkLimit,
            pagination.deep_walk_ms);
        // 成本随深度衰减：游标页的 WHERE 是「各账户 (anchor, order_id) 范围」，剩余行越多、
        // 每个账户要走的 idx_orders_sort 分支越多，单页越贵。所以「翻到后面会更快」不是异常。
        std::printf("  page cost vs depth (limit=%d, 1 sample per point): ", kPageSize);
        for (const auto& [percent, elapsed] : pagination.depth_sweep) {
            std::printf("%d%%=%.1fms ", percent, elapsed);
        }
        std::printf("\n");

        // ---- 3. 18:00 排序表达式 ----
        // 3a. 生产形状（20 账户 IN 列表）走生产实现；3b. 单账户（索引可服务 ORDER BY）；
        // 3c. 基准内探针：同一 SQL 形状下 anchor ORDER BY vs 普通 time ORDER BY（隔离表达式成本）。
        std::vector<double> single_account_samples;
        {
            TdListParams params;
            params.limit = kPageSize;
            for (int rep = 0; rep < page_reps; ++rep) {
                const auto begin = steady_clock::now();
                const auto body = query.execute(params, {"ACC00"}, std::string(kSeedTradingDay));
                single_account_samples.push_back(ms_between(begin, steady_clock::now()));
                (void)expect(body.has_value() && (*body)["available"].get<bool>(),
                             "single account page available");
            }
        }
        SQLite::Database probe_db(db_path, SQLite::OPEN_READONLY);
        probe_db.exec("PRAGMA busy_timeout=5000");
        const std::string probe_anchor_20 = probe_sql(online_accounts.size(), true);
        const std::string probe_plain_20 = probe_sql(online_accounts.size(), false);
        const std::string probe_anchor_1 = probe_sql(1, true);
        // 交错采样 anchor / plain：顺序采样会把同一次运行内的页缓存与频率漂移
        // 全算到后跑的那一个头上（实测 delta 在 +1.1~+6.1ms 间摆动）。
        auto [anchor_20_samples, plain_20_samples] =
            time_probe_pair(probe_db, probe_anchor_20, probe_plain_20, online_accounts, page_reps);
        const std::vector<double> anchor_1_samples =
            time_probe(probe_db, probe_anchor_1, {"ACC00"}, page_reps);
        const Statistics anchor_20 = summarize_values(anchor_20_samples);
        const Statistics plain_20 = summarize_values(plain_20_samples);
        const Statistics anchor_1 = summarize_values(anchor_1_samples);

        std::printf("\n[3] 18:00 anchor sort expression ((time + 21600) %% 86400)\n");
        std::printf(
            "  3a production shape via TdOrdersQuery, 20 accounts IN list (same as [2] first "
            "page)\n");
        print_stats("  first page (reference)", pagination.first_page, "ms");
        print_stats("  single account via TdOrdersQuery", summarize_values(single_account_samples),
                    "ms");
        std::printf(
            "  3b bench-local probe, one prepared read connection (same SQL shape as production, "
            "only ORDER BY differs)\n");
        print_stats("  20 accounts, anchor ORDER BY", anchor_20, "ms");
        print_stats("  20 accounts, plain o.time ORDER BY (control)", plain_20, "ms");
        print_stats("  single account, anchor ORDER BY", anchor_1, "ms");
        std::printf(
            "  delta(anchor vs plain, 20 accounts) = %+.3f ms (%.1f%%)\n",
            anchor_20.mean - plain_20.mean,
            plain_20.mean > 0.0 ? (anchor_20.mean - plain_20.mean) / plain_20.mean * 100.0 : 0.0);
        std::printf(
            "  production execute() first page vs probe are measured in separate phases, so their "
            "raw difference is not meaningful; the paired A/B below isolates the non-SQL cost\n");
        std::printf(
            "  note: 20-account IN list makes SQLite scan idx_orders_account_date per account and "
            "sort all matching rows in a TEMP B-TREE; single account can use idx_orders_sort for "
            "the ORDER BY and avoids it\n");
        print_query_plan(probe_db, probe_anchor_20, online_accounts,
                         "20 accounts, anchor ORDER BY");
        print_query_plan(probe_db, probe_plain_20, online_accounts,
                         "20 accounts, plain o.time ORDER BY");
        print_query_plan(probe_db, probe_anchor_1, {"ACC00"}, "single account, anchor ORDER BY");
        // 游标页（连续翻页 / 深页）的计划：多出 (anchor, order_id) 范围条件 → SQLite 走
        // MULTI-INDEX OR（每个账户各扫一段 idx_orders_sort），这解释了它与首页的成本差异。
        ProbeCursor cursor_probe;
        cursor_probe.present = true;
        cursor_probe.anchor = pagination.deep_cursor_anchor;
        cursor_probe.order_id = pagination.deep_cursor_order_id;
        const std::string probe_cursor_20 = probe_sql(online_accounts.size(), true, cursor_probe);
        {
            // 深游标已接近数据末端 → 剩余行可能不足一页（--quick 档必定如此），
            // 因此本探针不自检满页。
            const std::vector<double> cursor_samples =
                time_probe(probe_db, probe_cursor_20, online_accounts, page_reps, cursor_probe,
                           /*expect_full_page=*/false);
            const Statistics cursor_stats = summarize_values(cursor_samples);
            print_stats("  20 accounts, anchor ORDER BY + deep cursor", cursor_stats, "ms");
            std::printf(
                "  (production deep page mean for the same page shape: %.3f ms — the delta is "
                "per-call connection open + schema probe + JSON serialization)\n",
                pagination.deep_page.mean);
        }
        print_query_plan(probe_db, probe_cursor_20, online_accounts,
                         "20 accounts, anchor ORDER BY + deep cursor", cursor_probe);

        // 浅层游标页：这是「连续翻页比首页慢」的现场。游标把 WHERE 变成按账户的
        // (anchor, order_id) 范围 + 全局 TOP-N 归并，行的读取量仍是全账户 × 全量，
        // 只有取 TOP-201 之后才停 —— 与首页走同一条扫描路径但多一层 MULTI-INDEX OR。
        ProbeCursor shallow_probe;
        shallow_probe.present = true;
        shallow_probe.anchor = pagination.shallow_cursor_anchor;
        shallow_probe.order_id = pagination.shallow_cursor_order_id;
        const std::string probe_shallow_20 = probe_sql(online_accounts.size(), true, shallow_probe);
        {
            const std::vector<double> samples =
                time_probe(probe_db, probe_shallow_20, online_accounts, page_reps, shallow_probe);
            print_stats("  20 accounts, anchor ORDER BY + shallow cursor",
                        summarize_values(samples), "ms");
            std::printf(
                "  (production continuous-page mean: %.3f ms; first page without cursor: %.3f "
                "ms)\n",
                pagination.continuous_page.mean, pagination.first_page.mean);
        }
        print_query_plan(probe_db, probe_shallow_20, online_accounts,
                         "20 accounts, anchor ORDER BY + shallow cursor", shallow_probe);

        // 非 SQL 成本（生产 execute 每请求：开只读连接 + schema 探测 + 行映射 + JSON 序列化），
        // 用交错配对把「同一时刻的机器状态」固定在两边，差值才可读。
        {
            auto [production_samples, probe_samples] =
                time_execute_vs_probe(query, probe_db, probe_anchor_20, online_accounts, page_reps);
            const Statistics production = summarize_values(production_samples);
            const Statistics probe = summarize_values(probe_samples);
            const Statistics delta = paired_delta(production_samples, probe_samples);
            print_stats("  paired: production execute()", production, "ms");
            print_stats("  paired: probe SQL only", probe, "ms");
            print_stats("  paired delta (production - sql)", delta, "ms");
            std::printf(
                "  paired delta is per-request read-only connection + schema probe + row mapping + "
                "200-row JSON serialization (connection open alone measured at %.3f ms above)\n",
                open_stats.mean);
        }

        // ---- 4. tdstore::read_max_order_id ----
        std::vector<double> max_id_samples;
        int64_t max_order_id = 0;
        for (int rep = 0; rep < max_order_id_reps; ++rep) {
            const auto begin = steady_clock::now();
            max_order_id = dztrader::tdstore::read_max_order_id(db_path);
            max_id_samples.push_back(ms_between(begin, steady_clock::now()));
        }
        std::printf(
            "\n[4] tdstore::read_max_order_id() on %lld rows (opens a read-only connection per "
            "call)\n",
            static_cast<long long>(total_rows));
        print_stats("read_max_order_id", summarize_values(max_id_samples), "ms");
        std::printf("  value=%lld (expected=%lld)\n", static_cast<long long>(max_order_id),
                    static_cast<long long>(seed.max_order_id));
        checks_ok &= expect(max_order_id == seed.max_order_id,
                            "read_max_order_id matches seeded max order_id");
        std::printf("\n");
        exit_code = (checks_ok && g_checks_ok) ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "bench failed | error=\"%s\"\n", error.what());
        exit_code = 1;
    }

    {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
        if (ec) {
            std::fprintf(stderr, "temp dir cleanup failed | path=%s error=\"%s\"\n",
                         temp_dir.string().c_str(), ec.message().c_str());
        }
    }
    return exit_code;
}
