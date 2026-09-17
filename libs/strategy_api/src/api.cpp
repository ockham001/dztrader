#include <dztrader/api.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <cfloat>
#include <chrono>
#include <format>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <dztrader/error.h>
#include <dztrader/data_type.h>
#include <dztrader/core/last_error.h>
#include <dztrader/core/path.h>
#include <dztrader/core/random.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/tdstore/records_store.h>
#include <dztrader/version.h>

#include "strategy_context.h"
#include "result_set.h"
#include "vector_result_set.h"
#include "cursor_result_set.h"
#include "output_limit.h"
#include "db_database.h"

using namespace dztrader;

namespace {

/// 当前会话上下文登记: 仅 dz_init 重复调用检测与 dz_release 清登记使用;
/// 其余函数一律走 ctx 参数 (spec §4.2), 不得引用本变量。
/// 用函数内 static 而非文件级非 const 全局, 满足
/// cppcoreguidelines-avoid-non-const-global-variables。
/// 非线程安全: 生命周期由调用方单线程保证 (api.h 句柄契约)。
DzContext*& context_registry() {
    static DzContext* context = nullptr;
    return context;
}

// ── TD ingest 接线辅助 (契约 strategy "SDK ingest 过滤职责") ──────────────
// 本组函数仅服务 dz_init (水位装载) / dispatch_frame (断档回补) /
// dz_next_event (replay 派发), 均走 ctx 参数, 不触碰 context_registry。

using strategy_api_internal::DbQueryResult;
using strategy_api_internal::Row;
using strategy_api_internal::ColumnValue;

/// 行内取字符串列 (variant 无值/非字符串返回空串)
const char* row_string(const Row& row, size_t index) {
    if (index >= row.size()) {
        return "";
    }
    const auto* s = std::get_if<std::string>(&row[index]);
    return s ? s->c_str() : "";
}

/// 行内取整数列 (variant 无值/非 int64 返回 0)
int64_t row_int64(const Row& row, size_t index) {
    if (index >= row.size()) {
        return 0;
    }
    const auto* v = std::get_if<int64_t>(&row[index]);
    return v ? *v : 0;
}

/// 行内取 CHAR(1) 编码的数值列 (direction/position_effect/price_type/status):
/// td 写端绑定为数值, SQLite TEXT affinity 落盘为文本 '1'/'2'/…, 查询端按声明
/// 类型读成 string — 直接 row_int64 会得 0 (方向/状态全错)。按字符串解析数值,
/// 非数字回落 0。
int64_t row_char_code(const Row& row, size_t index) {
    if (index >= row.size()) {
        return 0;
    }
    if (const auto* s = std::get_if<std::string>(&row[index])) {
        try {
            return std::stoll(*s);
        } catch (const std::exception&) {
            return 0;
        }
    }
    return row_int64(row, index);
}

/// 行内取浮点列 (variant 无值/非 double 返回 0)
double row_double(const Row& row, size_t index) {
    if (index >= row.size()) {
        return 0.0;
    }
    const auto* v = std::get_if<double>(&row[index]);
    return v ? *v : 0.0;
}

/// 列名 -> 索引 + 值读取的轻量包装 (避免每行重复查列名)
struct ColumnMap {
    explicit ColumnMap(const DbQueryResult& r) {
        for (size_t i = 0; i < r.columns.size(); ++i) {
            index_by_name.emplace(r.columns[i].name, i);
        }
    }
    size_t idx(const std::string& name, size_t fallback = SIZE_MAX) const {
        const auto it = index_by_name.find(name);
        return it == index_by_name.end() ? fallback : it->second;
    }
    std::unordered_map<std::string, size_t> index_by_name;
};

/// 只读打开 td 统一库: <DZTRADER_HOME>/db/td.db (所有 td 网关共写)
/// 失败 (库不存在/打不开) 返回 nullptr (调用方降级不过滤)。
std::unique_ptr<DzDatabase> open_td_db() {
    const auto db_path = dztrader::paths::td_db();
    try {
        return strategy_api_internal::db_open_readonly(db_path.string());
    } catch (const Exception& e) {
        dz_diag((std::string("td ingest db unavailable (degraded, no filtering): ") + e.what())
                    .c_str());
    } catch (const std::exception& e) {
        dz_diag((std::string("td ingest db unavailable (degraded, no filtering): ") + e.what())
                    .c_str());
    } catch (...) {
        dz_diag("td ingest db unavailable (degraded, no filtering): unknown exception");
    }
    return nullptr;
}

/// 装载全部账户水位: 四表无过滤查询, 按账户求 MAX(seq) (spec §5.1)。
/// 四表读取包进单只读快照 (begin_snapshot RAII): 分次独立查询存在竞态 —
/// 先查表 A 后写端提交 A 中 seq∈(W_A, W] 的行, 再查表 B 得 W_B ≥ W, 该行
/// "快照没有却被 W 判定已含" → 静默误吞。单快照使四表共享同一视图,
/// 写端短暂阻塞由 busy_timeout=5000 吸收 (查询总时长 <10ms, 仅 init 低频路径)。
/// 单表查询失败 (表缺失/库不完整) 跳过该表, 不整体失败 (降级 = 部分表无快照水位,
/// 该表数据经帧全量放行, 回补时同样按表容错)。
void load_all_watermarks(DzContext* ctx) {
    try {
        auto db = open_td_db();
        if (db == nullptr) {
            return;
        }
        // 快照作用域覆盖四表聚合循环; 作用域结束 (析构) 即结束只读事务。
        auto snapshot = db->session->begin_snapshot();
        std::unordered_map<std::string, uint64_t> max_seq;
        for (const char* resource : {"order", "trade", "position", "trading_account"}) {
            // 终检发现 E: 聚合查询 (每账户 MAX(seq)) 替代 SELECT * 全行物化 —
            // 库随历史线性增长时全表装载线性恶化, 聚合只物化每账户一行。
            try {
                for (const auto& [acct, seq] :
                     strategy_api_internal::db_query_max_seq_by_account(db.get(), resource)) {
                    auto it = max_seq.find(acct);
                    if (it == max_seq.end() || seq > it->second) {
                        max_seq[acct] = seq;
                    }
                }
            } catch (const std::exception&) {
                continue;  // 表缺失/查询失败: 跳过该表, 不整体失败
            }
        }
        for (const auto& [acct, w] : max_seq) {
            ctx->ingest_gate.set_watermark(acct, w);
        }
        if (!max_seq.empty()) {
            dz_diag(
                std::format("td ingest watermarks loaded | accounts={}", max_seq.size()).c_str());
        }
    } catch (const std::exception& e) {
        dz_diag((std::string("td ingest watermark load failed (degraded, no filtering): ") +
                 e.what())
                    .c_str());
    } catch (...) {
        dz_diag("td ingest watermark load failed (degraded, no filtering)");
    }
}

/// 重查单账户新水位 (spec §5.5 重置): 四表按账户 MAX(seq)。
/// 四表读取包进单只读快照 (同 load_all_watermarks 的快照读竞态论证)。
/// 库不可用时返回 0 (重置为新基准, gate 过滤 seq≤0 即不拦 seq≥1)。
uint64_t rebuild_watermark(const std::string& account_id) {
    try {
        auto db = open_td_db();
        if (db == nullptr) {
            return 0;
        }
        // 快照作用域覆盖四表聚合循环; 作用域结束 (析构) 即结束只读事务。
        auto snapshot = db->session->begin_snapshot();
        uint64_t max_seq = 0;
        for (const char* resource : {"order", "trade", "position", "trading_account"}) {
            // 终检发现 E: 聚合查询 (单账户 MAX(seq)) 替代该账户全行物化。
            try {
                const uint64_t s = strategy_api_internal::db_query_account_max_seq(
                    db.get(), resource, account_id);
                if (s > max_seq) {
                    max_seq = s;
                }
            } catch (const std::exception&) {
                continue;  // 表缺失: 跳过该表
            }
        }
        return max_seq;
    } catch (const std::exception& e) {
        dz_diag((std::string("td ingest watermark rebuild failed (reset to empty): ") + e.what())
                    .c_str());
    } catch (...) {
        dz_diag("td ingest watermark rebuild failed (reset to empty)");
    }
    return 0;
}

/// "YYYYMMDD" 文本 -> DzDate (距纪元天数); 非法回落 0。
int32_t parse_trading_day_to_epoch(const std::string_view text) {
    if (text.size() != 8) {
        return 0;
    }
    try {
        const int32_t y = std::stoi(std::string(text.substr(0, 4)));
        const int32_t m = std::stoi(std::string(text.substr(4, 2)));
        const int32_t d = std::stoi(std::string(text.substr(6, 2)));
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    } catch (...) {
        return 0;
    }
}

/// epoch 秒 -> 距午夜秒 (DzTime, struct.h "时间（距午夜秒数）")。
/// DB 时间列 (insert_time/update_time/trade_time) 存 epoch 秒 = 日期*86400 + 当日秒;
/// rpt.time 只需当日秒部分 (日期已由 rpt.date 表达)。非法 (≤0) 回落 0。
int32_t epoch_secs_to_tod(int64_t epoch_secs) {
    if (epoch_secs <= 0) {
        return 0;
    }
    return static_cast<int32_t>(epoch_secs % 86400);
}

/// DzDate (距纪元天数) -> "YYYYMMDD" 8 位文本 (gate 去重段键 day 分量)。
/// 非法日期回落空串 (调用方按 0 日处理)。
std::string format_epoch_day_to_yyyyMMdd(int32_t days) {
    if (days <= 0) {
        return "";
    }
    try {
        const dztrader::Date d{days};
        return std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
    } catch (...) {
        return "";
    }
}

/// TRADE_REPORT 成交去重二道防线 (契约 strategy §5.4 / "SDK ingest 过滤职责"):
/// 从帧 payload 取 (account_id, date, trade_id) 调 admit_trade。日期以帧 date 为真源
/// (帧 date 为实时/回补路径均已填的 DzDate), 转 "YYYYMMDD" 文本对齐 gate 段键。
/// 返回 false = 已存在重复 (拦截, 不返回策略用户)。
bool admit_trade_report(DzContext* ctx, const DzTradeReport& rpt) {
    const std::string day = format_epoch_day_to_yyyyMMdd(rpt.date);
    if (day.empty()) {
        return true;  // 解析失败: 不拦截 (宁可放行, 去重以帧 date 缺失时不强行拦)
    }
    return ctx->ingest_gate.admit_trade(rpt.account_id, day.c_str(), rpt.trade_id);
}

/// 2018 ACCOUNT_STATUS 推送: 携带 trading_day 时驱动 gate 交易日切换
/// (清该账户旧日去重段, 契约 strategy "成交去重…交易日切换清理")。
/// 未携带 (trading_day=0) 或解析失败: no-op (gate 维持已见日, 由 admit_trade 自清)。
void on_account_status_trading_day(DzContext* ctx, const DzAccountStatus& st) {
    const std::string day = format_epoch_day_to_yyyyMMdd(st.trading_day);
    if (day.empty()) {
        return;
    }
    ctx->ingest_gate.on_trading_day_changed(st.account_id, day.c_str());
}

/// 终检发现 D【Important】: td 重启复用 seq 吞新事件 — PositionRebuild DELETE 全平持仓
/// 行会压低 DB MAX(seq) → td 重启 seq_counter_ = 压低后 MAX → 复用 seq 撞在线策略
/// last_applied → admit 跳过新事件, 且策略无自愈路径 (2018 Offline 在 SDK 只清成交去重段,
/// 不动 gate)。修复: 检测同账户 2018 Offline→Ready 翻转 (dzweb 有 2018 重建自愈) 时,
/// 对 SDK ingest_gate reset_account(rebuild_watermark) + 清该账户 gap_retry 状态 —
/// 与 dzweb 2018 重建对齐。注意: 2018 Ready 后紧接的 repush/重放不会重复 (td 过滤器吞同),
/// gate 重置安全; applied_trades 清空由 trade 去重段同清 (reset_account 已做)。
void on_account_status_gate_reset(DzContext* ctx, const DzAccountStatus& st) {
    const std::string account(st.account_id);
    if (account.empty()) {
        return;
    }
    const bool seen_before =
        ctx->account_status_states.find(account) != ctx->account_status_states.end();
    const DzAccountState prev = seen_before ? ctx->account_status_states[account]
                                            : DZ_ACCOUNT_LOGGING_IN;  // 首见不计翻转
    const bool flip_to_ready = seen_before && prev == DZ_ACCOUNT_OFFLINE &&
                               st.state == DZ_ACCOUNT_READY;
    ctx->account_status_states[account] = st.state;
    if (!flip_to_ready) {
        return;
    }
    // Offline→Ready: 该账户数据被整体重建 (td 重启/重连重登收尾)。重查 DB 新水位
    // (PositionRebuild 删行后可能低于在线 last_applied) + reset_account 复位 last_applied,
    // 使复用 seq 的新帧不再被旧 last_applied 拦截。
    const uint64_t w = rebuild_watermark(account);
    ctx->ingest_gate.reset_account(account, w);
    // 作废在途 gap 重试: 旧 regime 的回补区间在新水位下无意义, 暂存帧/触发帧不得在
    // 重置后补发 (否则旧状态覆盖新状态)。
    if (ctx->gap_retry.active && ctx->gap_retry.account_id == account) {
        dz_diag("ingest gap retry cancelled on account status flip to ready");
        ctx->gap_retry.clear();
    }
    dz_diag((std::string("ingest gate reset on 2018 offline->ready: account=") + account).c_str());
}

/// 断档回补重试上限 (契约 td-data-sync §5.2 "几十 ms 短重试; 耗尽放行"):
/// 无 sleep 设计 (dz_next_event 热路径不能阻塞), 每次调用查一次库, 上限耗尽后
/// 按"崩溃丢失"放行触发帧 + ERROR 日志。
constexpr uint32_t kGapRetryMax = 10;

/// gap 区间四表查询结果 (每表独立容错: 表缺失/查询失败置 ok=false, 该表按 0 行
/// 参与覆盖判定 — 库不完整时其余表仍可覆盖, 行数不足时则值得重试)。
struct GapBackfillRows {
    DbQueryResult orders;
    DbQueryResult trades;
    DbQueryResult positions;
    DbQueryResult accounts;
    bool orders_ok = false;
    bool trades_ok = false;
    bool positions_ok = false;
    bool accounts_ok = false;
};

/// 查询 gap 区间四表行 (不转换不入缓冲)。
GapBackfillRows query_gap_tables(DzDatabase* db, const std::string& account_id, uint64_t from,
                                 uint64_t to) {
    const auto query_table = [&](const char* resource, bool* ok) -> DbQueryResult {
        try {
            *ok = true;
            return strategy_api_internal::db_query_seq_range(db, resource, account_id, from, to);
        } catch (const std::exception& e) {
            dz_diag((std::string("ingest backfill table skipped: ") + e.what()).c_str());
            return DbQueryResult{};
        }
    };
    GapBackfillRows rows;
    rows.orders = query_table("order", &rows.orders_ok);
    rows.trades = query_table("trade", &rows.trades_ok);
    rows.positions = query_table("position", &rows.positions_ok);
    rows.accounts = query_table("trading_account", &rows.accounts_ok);
    return rows;
}

/// 覆盖判定 (契约 td-data-sync §74): 追加流 + 绝对态容差。
/// 四表共享一个 seq 取号器, 区间内每个 seq 恰有一行落在某表。
/// - 追加流 (orders/trades) 不删行: 四表总行数 ≥ 区间宽 = 区间每个 seq 恰有一行
///   (含绝对态现存的) 即精确覆盖;
/// - 绝对态 (positions/trading_accounts) 存在 PositionRebuild 删行语义
///   (td_persist_writer.cpp DELETE), 被删行在 DB 无行 → 行数 < span 会误判未覆盖 →
///   10 次重试耗尽放行且不回补现存行 (追加流缺条)。放宽: 该账户当前 MAX(seq)
///   (四表聚合, 与 W 同语义) ≥ gap 上界即视为覆盖 — 缺的中间 seq 是被删的历史行,
///   当前态已到位。persist 单写者 FIFO 提交: 有 ≥ 上界的行提交 ⇒ 区间内缺席行永久
///   缺席 (删行或崩溃丢失), 重试无益, 直接回补现存行更优。
bool gap_covered(DzDatabase* db, const GapBackfillRows& rows, const std::string& account_id,
                 uint64_t from, uint64_t to) {
    const uint64_t span = to - from + 1;
    const uint64_t total = rows.orders.rows.size() + rows.trades.rows.size() +
                           rows.positions.rows.size() + rows.accounts.rows.size();
    if (total >= span) {
        return true;
    }
    uint64_t account_max = 0;
    for (const char* resource : {"order", "trade", "position", "trading_account"}) {
        try {
            account_max = std::max(
                account_max,
                strategy_api_internal::db_query_account_max_seq(db, resource, account_id));
        } catch (const std::exception&) {
            continue;  // 表缺失/查询失败: 跳过该表 (容差聚合逐表容错)
        }
    }
    return account_max >= to;
}

/// 断档回补单次快照查询 + 覆盖判定 (begin_snapshot 只读快照, 与水位装载同型):
/// 范围行查询 + 绝对态容差聚合查询必须共享同一快照 — 否则写端在范围查询后、容差聚合前
/// 提交区间内行, 容差聚合见新行 (account_max ≥ 上界) 判覆盖, 而范围行仍缺该行 →
/// 回补缺条 (该行 seq ≤ W 被过滤, 追加流永久缺)。单快照使两查询同视图, 写端短暂阻塞
/// 由 busy_timeout=5000 吸收。返回 true = 覆盖; rows 为同快照的范围行结果。
bool gap_covered_snapshot(DzDatabase* db, GapBackfillRows* rows, const std::string& account_id,
                          uint64_t from, uint64_t to) {
    // 快照作用域覆盖范围查询 + 覆盖判定聚合; 作用域结束 (析构) 即结束只读事务。
    auto snapshot = db->session->begin_snapshot();
    *rows = query_gap_tables(db, account_id, from, to);
    return gap_covered(db, *rows, account_id, from, to);
}

/// 回补帧入 replay 缓冲 (覆盖确认后调用): 查询结果转 Dz*Report 填 seq 入缓冲。
/// 行序 = seq 序 (db_query_seq_range ORDER BY seq ASC), 各表内部有序; 跨表简化为逐表入缓冲,
/// 每表内部 seq 序 (回补消费端按帧类型独立, 不要求跨表严格交错)。
/// 返回 false = 缓冲溢出 (gap 区间过宽, 回补被截断)。
bool enqueue_gap_rows(DzContext* ctx, const GapBackfillRows& rows) {
    bool ok = true;
    // orders -> DzOrderReport (帧 DZ_FRAME_ORDER_REPORT)
    {
        const DbQueryResult& result = rows.orders;
        const ColumnMap cols(result);
        const auto acct_c = cols.idx("account_id", 1);
        const auto day_c = cols.idx("trading_day", 2);
        const auto oid_c = cols.idx("order_id", 3);
        const auto inst_c = cols.idx("instrument_id", 7);
        const auto exch_c = cols.idx("exchange_id", 8);
        const auto dir_c = cols.idx("direction", 9);
        const auto pe_c = cols.idx("position_effect", 10);
        const auto pt_c = cols.idx("price_type", 11);
        const auto st_c = cols.idx("status", 12);
        const auto pr_c = cols.idx("price", 13);
        const auto vol_c = cols.idx("volume", 14);
        const auto vt_c = cols.idx("volume_traded", 15);
        const auto sid_c = cols.idx("strategy_id", 21);
        const auto seq_c = cols.idx("seq", 23);
        const auto itime_c = cols.idx("insert_time", 17);
        const auto utime_c = cols.idx("update_time", 18);
        for (const Row& row : result.rows) {
            DzOrderReport rpt{};
            dztrader::copy_string(rpt.account_id, row_string(row, acct_c), true);
            dztrader::copy_string(rpt.instrument_id, row_string(row, inst_c), true);
            dztrader::copy_string(rpt.exchange_id, row_string(row, exch_c), true);
            dztrader::copy_string(rpt.strategy_id, row_string(row, sid_c), true);
            rpt.order_id = row_int64(row, oid_c);
            rpt.direction = static_cast<DzDirection>(row_char_code(row, dir_c));
            rpt.position_effect = static_cast<DzPositionEffect>(row_char_code(row, pe_c));
            rpt.price_type = static_cast<DzPriceType>(row_char_code(row, pt_c));
            rpt.status = static_cast<DzOrderStatus>(row_char_code(row, st_c));
            rpt.price = row_double(row, pr_c);
            rpt.volume = static_cast<DzVolume>(row_int64(row, vol_c));
            rpt.volume_traded = static_cast<DzVolume>(row_int64(row, vt_c));
            rpt.date = parse_trading_day_to_epoch(row_string(row, day_c));
            // 回补时间语义 (评审发现 2): 实时帧 time 为 CTP InsertTime/UpdateTime 当日秒,
            // DB insert_time/update_time 为 epoch 秒; 取当日秒填 rpt.time 与实时一致。
            // 优先 update_time (最新状态时间), 缺省回落 insert_time。
            rpt.time = epoch_secs_to_tod(
                row_int64(row, utime_c) != 0 ? row_int64(row, utime_c)
                                             : row_int64(row, itime_c));
            rpt.seq = static_cast<uint64_t>(row_int64(row, seq_c));
            ok = ctx->enqueue_replay_frame(DZ_FRAME_ORDER_REPORT, &rpt, sizeof(rpt)) && ok;
        }
    }
    // trades -> DzTradeReport (帧 DZ_FRAME_TRADE_REPORT)
    {
        const DbQueryResult& result = rows.trades;
        const ColumnMap cols(result);
        const auto acct_c = cols.idx("account_id", 1);
        const auto day_c = cols.idx("trading_day", 2);
        const auto tid_c = cols.idx("trade_id", 3);
        const auto oid_c = cols.idx("order_id", 4);
        const auto inst_c = cols.idx("instrument_id", 5);
        const auto exch_c = cols.idx("exchange_id", 6);
        const auto dir_c = cols.idx("direction", 7);
        const auto pe_c = cols.idx("position_effect", 8);
        const auto pr_c = cols.idx("price", 9);
        const auto vol_c = cols.idx("volume", 10);
        const auto sid_c = cols.idx("strategy_id", 14);
        const auto seq_c = cols.idx("seq", 15);
        const auto ttime_c = cols.idx("trade_time", 11);
        for (const Row& row : result.rows) {
            DzTradeReport rpt{};
            dztrader::copy_string(rpt.account_id, row_string(row, acct_c), true);
            dztrader::copy_string(rpt.instrument_id, row_string(row, inst_c), true);
            dztrader::copy_string(rpt.exchange_id, row_string(row, exch_c), true);
            dztrader::copy_string(rpt.strategy_id, row_string(row, sid_c), true);
            dztrader::copy_string(rpt.trade_id, row_string(row, tid_c), true);
            rpt.order_id = row_int64(row, oid_c);
            rpt.direction = static_cast<DzDirection>(row_char_code(row, dir_c));
            rpt.position_effect = static_cast<DzPositionEffect>(row_char_code(row, pe_c));
            rpt.price = row_double(row, pr_c);
            rpt.volume = static_cast<DzVolume>(row_int64(row, vol_c));
            rpt.date = parse_trading_day_to_epoch(row_string(row, day_c));
            // 回补时间语义 (评审发现 2): 实时帧 time 为 CTP TradeTime 当日秒,
            // DB trade_time 为 epoch 秒; 取当日秒填 rpt.time 与实时一致。
            rpt.time = epoch_secs_to_tod(row_int64(row, ttime_c));
            rpt.seq = static_cast<uint64_t>(row_int64(row, seq_c));
            ok = ctx->enqueue_replay_frame(DZ_FRAME_TRADE_REPORT, &rpt, sizeof(rpt)) && ok;
        }
    }
    // positions -> DzPositionInfo (帧 DZ_FRAME_POSITION_INFO)
    {
        const DbQueryResult& result = rows.positions;
        const ColumnMap cols(result);
        const auto acct_c = cols.idx("account_id", 0);
        const auto day_c = cols.idx("trading_day", 1);
        const auto inst_c = cols.idx("instrument_id", 2);
        const auto exch_c = cols.idx("exchange_id", 3);
        const auto dir_c = cols.idx("direction", 4);
        const auto vol_c = cols.idx("volume", 5);
        const auto fz_c = cols.idx("frozen_volume", 6);
        const auto td_c = cols.idx("today_volume", 7);
        const auto yd_c = cols.idx("yd_volume", 8);
        const auto pr_c = cols.idx("price", 9);
        const auto seq_c = cols.idx("seq", 10);
        for (const Row& row : result.rows) {
            DzPositionInfo rpt{};
            dztrader::copy_string(rpt.account_id, row_string(row, acct_c), true);
            dztrader::copy_string(rpt.instrument_id, row_string(row, inst_c), true);
            dztrader::copy_string(rpt.exchange_id, row_string(row, exch_c), true);
            rpt.direction = static_cast<DzDirection>(row_char_code(row, dir_c));
            rpt.volume = row_int64(row, vol_c);
            rpt.frozen_volume = row_int64(row, fz_c);
            rpt.today_volume = row_int64(row, td_c);
            rpt.yd_volume = row_int64(row, yd_c);
            rpt.price = row_double(row, pr_c);
            rpt.date = parse_trading_day_to_epoch(row_string(row, day_c));
            rpt.seq = static_cast<uint64_t>(row_int64(row, seq_c));
            ok = ctx->enqueue_replay_frame(DZ_FRAME_POSITION_INFO, &rpt, sizeof(rpt)) && ok;
        }
    }
    // trading_accounts -> DzTradingAccount (帧 DZ_FRAME_TRADING_ACCOUNT)
    {
        const DbQueryResult& result = rows.accounts;
        const ColumnMap cols(result);
        const auto acct_c = cols.idx("account_id", 0);
        const auto day_c = cols.idx("trading_day", 1);
        const auto seq_c = cols.idx("seq", 10);
        for (const Row& row : result.rows) {
            DzTradingAccount rpt{};
            dztrader::copy_string(rpt.account_id, row_string(row, acct_c), true);
            rpt.balance = row_double(row, cols.idx("balance", 2));
            rpt.available = row_double(row, cols.idx("available", 3));
            rpt.frozen = row_double(row, cols.idx("frozen", 4));
            rpt.commission = row_double(row, cols.idx("commission", 5));
            rpt.margin = row_double(row, cols.idx("margin", 6));
            rpt.withdraw_quota = row_double(row, cols.idx("withdraw_quota", 7));
            rpt.deposit = row_double(row, cols.idx("deposit", 8));
            rpt.withdraw = row_double(row, cols.idx("withdraw", 9));
            rpt.date = parse_trading_day_to_epoch(row_string(row, day_c));
            rpt.seq = static_cast<uint64_t>(row_int64(row, seq_c));
            ok = ctx->enqueue_replay_frame(DZ_FRAME_TRADING_ACCOUNT, &rpt, sizeof(rpt)) && ok;
        }
    }
    return ok;
}

/// 补发被拦截的触发帧 + 重试期间暂存的后续帧 (回补完整或重试耗尽时, 经 replay 缓冲
/// 前置派发)。调用方 (drive_gap_retry) 先入回补行再调用本函数 → 派发序 = 回补行 →
/// 触发帧 → 暂存帧 (按 seq 序, 终检发现 B)。返回 false = 缓冲溢出 (触发帧/暂存帧被丢弃,
/// 宁缺勿乱)。
bool release_held_trigger(DzContext* ctx) {
    auto& rt = ctx->gap_retry;
    bool ok = true;
    if (rt.trigger_payload.empty()) {
        // 防御: 无暂存触发帧 (理论不可达) — 仍派发暂存帧, 不整体跳过
    } else if (!ctx->enqueue_replay_frame(rt.trigger_type, rt.trigger_payload.data(),
                                          static_cast<uint32_t>(rt.trigger_payload.size()))) {
        dz_diag("ingest gap retry trigger release dropped (replay buffer full)");
        ok = false;
    }
    for (const auto& sf : rt.staged_frames) {
        if (!ctx->enqueue_replay_frame(sf.type, sf.payload.data(),
                                       static_cast<uint32_t>(sf.payload.size()))) {
            dz_diag("ingest gap retry staged frame dropped (replay buffer full)");
            ok = false;
        }
    }
    return ok;
}

/// 断档回补重试驱动 (每次调用尝试一次; 由 dz_next_event 入口与后续 ingest 调用驱动):
/// 覆盖 → 回补行 + 暂存触发帧 + 暂存后续帧入 replay 缓冲 (按 seq 序: 回补行 → 触发帧 →
/// 暂存帧), 重试结束; 仍不足 → 计数, 耗尽 (kGapRetryMax) → ERROR + 放行触发帧 + 暂存帧
/// (按"崩溃丢失", 契约 §5.2)。终检发现 B: 释放顺序必须回补行在前 (seq 最小), 否则
/// 同键旧状态覆盖新状态; 暂存帧最后 (seq 最大), 后续帧不回退。
void drive_gap_retry(DzContext* ctx) {
    auto& rt = ctx->gap_retry;
    if (!rt.active) {
        return;
    }
    bool covered = false;
    bool enqueued = false;
    auto db = open_td_db();
    if (db != nullptr) {
        GapBackfillRows rows;
        covered = gap_covered_snapshot(db.get(), &rows, rt.account_id, rt.from, rt.to);
        if (covered) {
            // 回补行先入缓冲 (seq 最小), 再释放触发帧 + 暂存帧 (seq 更大) —
            // 派发序 = 回补行 → 触发帧 → 暂存帧 = 按 seq 序 (终检发现 B)。
            enqueued = enqueue_gap_rows(ctx, rows);
            release_held_trigger(ctx);
        }
    }
    if (covered && enqueued) {
        dz_diag("ingest gap backfill retry covered, trigger released");
        rt.clear();
        return;
    }
    if (covered && !enqueued) {
        // 覆盖但缓冲溢出 (回补被截断): 与首次溢出语义一致, 不再重试
        // (缓冲已满, 再查同区间仍溢出)。
        dz_diag("ingest gap backfill retry overflow, give up retry");
        rt.clear();
        return;
    }
    if (++rt.attempts >= kGapRetryMax) {
        dz_diag("ingest gap backfill retry exhausted, release trigger frame (treat as crash loss)");
        release_held_trigger(ctx);
        rt.clear();
    }
}

/// 断档回补入口: 处理 gate 新检测的 gap (首帧) 与既有重试 (每次调用尝试一次)。
/// 返回 false = 触发帧被拦截:
///   - 重试开始 (覆盖不足, persist 在途窗口): 触发帧暂存, 待回补完整/耗尽时补发;
///   - 重试在途 (终检发现 B): 后续帧暂存而非放行 — 直接放行会插到回补帧之前,
///     同键时旧状态覆盖新状态且无再修复; 统一暂存, 覆盖后按 seq 序派发;
///   - 缓冲溢出 (gap 区间过宽, 回补被截断): 既有语义, 宁缺勿乱。
/// 触发帧参数供暂存补发 (payload 副本; 仅 gap 检测帧传入, 后续帧传 nullptr — 后续帧
/// 经 active 分支按完整帧暂存, 无需单独参数)。
bool handle_gap(DzContext* ctx, const std::byte* trigger_frame) {
    if (ctx->gap_retry.active) {
        // 重试在途: 本帧 (seq > 触发帧) 不得直接放行 — 直接放行会让新状态帧先于
        // 回补的旧状态帧到达, 同键 (同 order_id) 时回补旧状态覆盖新状态且无再修复
        // (终检发现 B)。统一暂存 (经 trigger_frame 的完整帧 payload 副本), 覆盖/耗尽
        // 时按 seq 序 (回补行 → 触发帧 → 暂存帧) 经 replay 缓冲派发。
        // 暂存帧已在 ingest_td_frame 的 admit 阶段推进 last_applied, 派发时不再过
        // ingest (与触发帧补发同语义), 天然 seq 序。
        if (trigger_frame != nullptr) {
            const shm::FrameView view(trigger_frame);
            if (!ctx->gap_retry.stage(view.type(), trigger_frame + sizeof(DzFrameHeader),
                                      view.frame_size() - sizeof(DzFrameHeader))) {
                dz_diag("ingest gap retry staged frame overflow, drop frame");
            }
        }
        // 每次调用顺带驱动一次重试 (无新 gap — gate 的 pending_gap 已被首次尝试消费)。
        drive_gap_retry(ctx);
        return false;  // 拦截本帧 (宁缺勿乱: 本帧与回补帧的 seq 序不可被破坏)
    }
    auto gap = ctx->ingest_gate.take_pending_gap();
    if (!gap.has_value()) {
        return true;
    }
    auto db = open_td_db();
    if (db == nullptr) {
        dz_diag("ingest gap but td db unavailable, skip backfill");
        return true;  // 无库: 不拦截触发帧 (降级全放行语义)
    }
    GapBackfillRows rows;
    if (!gap_covered_snapshot(db.get(), &rows, gap->account_id, gap->from, gap->to)) {
        // 行数不足以覆盖 gap 区间 (触发帧到达 ≠ persist 已提交): 保留 gap 重试,
        // 拦截触发帧 (宁缺勿乱), 每次调用重试一次直至覆盖或耗尽 (契约 §5.2)。
        if (trigger_frame != nullptr) {
            const shm::FrameView view(trigger_frame);
            ctx->gap_retry.begin(
                gap->account_id, gap->from, gap->to, view.type(),
                trigger_frame + sizeof(DzFrameHeader),
                view.frame_size() - sizeof(DzFrameHeader));
        }
        dz_diag("ingest gap backfill incomplete (rows not committed yet), will retry");
        return false;
    }
    if (!enqueue_gap_rows(ctx, rows)) {
        return false;  // 缓冲溢出: 拦截触发帧, 宁缺勿乱 (不重试, 缓冲已满)
    }
    return true;
}

/// 单帧 TD ingest 过滤 + 断档回补 (DZ_FRAME_ORDER_REPORT-DZ_FRAME_TRADING_ACCOUNT 共用):
/// 先 detect_reset (倒退) 后 admit (W 过滤), kSkip 拦截; gap 时查库填 replay 缓冲。
/// 返回 true = 通过 ingest (调用方再做策略定向/放行)。
template <typename ReportT>
bool ingest_td_frame(DzContext* ctx, const std::byte* frame) {
    const shm::FrameView view(frame);
    if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(ReportT)) {
        return false;  // 截断帧防御: 读不出 payload 的帧一律拦截
    }
    // 终检发现 C【Important】: 帧头损坏/版本错配的更大帧 (frame_size 无上界) 在回补
    // 暂存/回放路径会按全量 payload memcpy 越界写坏相邻槽。ingest 层统一加上界钳制:
    // payload 超 replay 槽联合容量 (最大 Dz*Report) 即拦截 + 日志 (宁缺勿乱)。
    if (view.frame_size() > sizeof(DzFrameHeader) + DzContext::REPLAY_FRAME_MAX_PAYLOAD) {
        dz_diag("ingest td frame payload exceeds replay slot capacity, frame dropped");
        return false;
    }
    const auto& v = view.payload<ReportT>();
    if (ctx->ingest_gate.detect_reset(v.account_id, v.seq)) {
        const uint64_t w = rebuild_watermark(v.account_id);
        ctx->ingest_gate.reset_account(v.account_id, w);
        // 终检发现 D: 重置路径作废该账户在途 gap 重试 — 旧 regime 的回补区间
        // (回补行/触发帧/暂存帧) 在新水位下无意义, 不得在重置后补发 (旧状态覆盖新状态)。
        if (ctx->gap_retry.active && ctx->gap_retry.account_id == v.account_id) {
            dz_diag("ingest gap retry cancelled on account reset");
            ctx->gap_retry.clear();
        }
    }
    if (ctx->ingest_gate.admit(v.account_id, v.seq) == TdIngestGate::Verdict::kSkip) {
        return false;
    }
    // 回补处理: 覆盖不足 → 触发帧拦截暂存重试; 缓冲溢出 (gap 过宽) → 拦截触发帧,
    // 宁缺勿乱 (见 handle_gap/enqueue 注释)。
    if (!handle_gap(ctx, frame)) {
        return false;
    }
    return true;
}

}  // namespace

/* ── 生命周期 ── */

DZ_API DzContext* dz_init(void) {
    DzContext*& context = context_registry();
    if (context != nullptr) {
        LastError::set(DZ_EC_STRATEGY_ALREADY_INITIALIZED, "dz_init called twice");
        return nullptr;
    }
    try {
        context = new DzContext();  // NOLINT
        // TD ingest 水位装载: 尝试打开 td 库查 W 填 gate; 失败/无库降级为不过滤全放行
        // (日志注明, 见 load_all_watermarks)。降级不视为 init 失败。
        load_all_watermarks(context);
        return context;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return nullptr;
}

DZ_API void dz_release(DzContext* ctx) {
    // 契约(api.h 句柄契约): dz_release 恰好一次, 且只能传当前会话句柄。
    // NULL 为 no-op; 非当前句柄/重复释放属 UB, 不设防。
    if (ctx == nullptr) {
        return;
    }
    DzContext*& context = context_registry();
    if (ctx == context) {
        context = nullptr;  // 先清登记再 delete
    }
    delete ctx;  // NOLINT
}

/* ── 等待 / 事件读取 ── */

namespace {

/// dz_next_event 单次调用连续消费内部帧的上限 (防内部帧洪峰饿死 dz_next_md)
constexpr uint32_t kMaxInternalFramesPerCall = 32;

/// TD 回报定向过滤: payload.strategy_id == 本策略裸名才放行。
/// 空 strategy_id (外部单/手工单, 非任何策略所下) 与非本策略回报一律拦截。
template <typename ReportT>
bool is_own_report(const DzContext* ctx, const std::byte* frame) noexcept {
    const shm::FrameView view(frame);
    if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(ReportT)) {
        return false;  // 截断帧防御: 读不出 payload 的帧一律拦截
    }
    const auto& rpt = view.payload<ReportT>();
    return std::string_view(rpt.strategy_id) == std::string_view(ctx->strategy_id);
}

/// NOTIFY_MD_STARTED 自动补订阅 (定义见 write_subscribe_req 之后)
void on_md_started_internal(DzContext* ctx, const std::byte* frame);

/// 单帧分派: 用户帧返回 true (放行给策略用户), 其余返回 false (SDK 内部消费/丢弃)。
/// 逐帧类型 switch, 替代原 is_user_frame 白名单 + handle_internal_frame 双开关。
bool dispatch_frame(DzContext* ctx, const std::byte* frame, DzFrameType type) {
    switch (type) {
        case DZ_FRAME_ORDER_REPORT:
            if (!ingest_td_frame<DzOrderReport>(ctx, frame)) {
                return false;
            }
            return is_own_report<DzOrderReport>(ctx, frame);
        case DZ_FRAME_TRADE_REPORT:
            if (!ingest_td_frame<DzTradeReport>(ctx, frame)) {
                return false;
            }
            // 定向过滤先行 (strategy_id 不匹配/空不污染去重段), 再成交去重二道防线。
            if (!is_own_report<DzTradeReport>(ctx, frame)) {
                return false;
            }
            return admit_trade_report(ctx, shm::FrameView(frame).payload<DzTradeReport>());
        case DZ_FRAME_UI_INPUT:
            // 定向帧: 仅 instance_id == 裸策略名 的属于本策略
            return std::string_view(shm::FrameView(frame).ext_inst_id()) == ctx->strategy_id;
        case DZ_FRAME_SHUTDOWN:
            if (std::string_view(shm::FrameView(frame).ext_inst_id()) == ctx->strategy_id) {
                ctx->internal_cleanup_on_shutdown();  // 内部清理后再放行
                return true;
            }
            return false;
        case DZ_FRAME_PRELOAD_EVENT_SHM: {
            const auto& params = shm::FrameView(frame).payload<DzShmPreload>();
            ctx->internal_event_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(DzContext::INTERNAL_TOKEN_EVENT,
                                           dztrader::core::random_jitter(0, 5000));
            return false;
        }
        case DZ_FRAME_PRELOAD_MD_SHM: {
            const shm::FrameView view(frame);
            // 仅本策略绑定的行情源 (instance_id = 行情通道名)
            if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
                return false;
            }
            if (view.ext_inst_payload_size() < sizeof(DzShmPreload)) {
                return false;
            }
            const auto& params = *reinterpret_cast<const DzShmPreload*>(view.ext_inst_payload());
            ctx->internal_md_preload = params;  // 覆盖参数槽 (只保留最新)
            ctx->schedule_internal_preload(DzContext::INTERNAL_TOKEN_MD,
                                           dztrader::core::random_jitter(0, 5000));
            return false;
        }
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER:
            ctx->event_writer.refresh_subscribers();
            return false;
        case DZ_FRAME_NOTIFY_MD_STARTED:
            on_md_started_internal(ctx, frame);
            return false;
        // 其余 TD 回报帧 DZ_FRAME_POSITION_INFO-DZ_FRAME_ACCOUNT_STATUS (持仓/资金/网关状态等): 暂不按策略过滤, 全量放行。
        // DZ_FRAME_POSITION_INFO/DZ_FRAME_TRADING_ACCOUNT 为 ingest 帧 (含 seq/account_id): 完整帧过 gate (W 过滤/断档/倒退),
        // 截断帧保持透传 (既有语义: 不按策略过滤, 引擎侧 payload_size_matches 丢弃);
        // 除上述四个 ingest 帧外的 TD 回报帧无 seq 字段, 不 ingest, 直接全量放行。
        case DZ_FRAME_POSITION_INFO: {
            const shm::FrameView view(frame);
            if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(DzPositionInfo)) {
                return true;  // 截断: 不透传 payload, 保持全量放行语义
            }
            return ingest_td_frame<DzPositionInfo>(ctx, frame);
        }
        case DZ_FRAME_TRADING_ACCOUNT: {
            const shm::FrameView view(frame);
            if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(DzTradingAccount)) {
                return true;  // 截断: 不透传 payload, 保持全量放行语义
            }
            return ingest_td_frame<DzTradingAccount>(ctx, frame);
        }
        case DZ_FRAME_ACCOUNT_STATUS: {
            // 2018 携带 trading_day (DzAccountStatus.trading_day, Offline 为 0):
            // 驱动 gate 交易日切换清旧日去重段 (契约 strategy "成交去重…交易日切换清理")。
            // 截断帧防御: 读不出 payload 不驱动 (仍全量放行, 引擎侧 payload_size_matches 丢弃)。
            if (shm::FrameView(frame).frame_size() >=
                sizeof(DzFrameHeader) + sizeof(DzAccountStatus)) {
                const auto& st = shm::FrameView(frame).payload<DzAccountStatus>();
                on_account_status_trading_day(ctx, st);
                // 终检发现 D: Offline→Ready 翻转触发该账户 gate 重置 + 清 gap_retry
                // (td 重启复用 seq 吞新事件的自愈, 与 dzweb 2018 重建对齐)。
                on_account_status_gate_reset(ctx, st);
            }
            return true;  // 2018 仍全量放行给策略用户 (on_account_status 回调, 引擎测试覆盖)
        }
        case DZ_FRAME_TD_RISK_REJECT:
        case DZ_FRAME_TD_TRANSFER_REQ:
        case DZ_FRAME_TD_TRANSFER_RSP:
        case DZ_FRAME_TD_TRANSFER_RTN:
        case DZ_FRAME_TD_PASSWORD_UPDATE_REQ:
        case DZ_FRAME_TD_PASSWORD_UPDATE_RSP:
            return true;
        default:
            return false;  // 其余平台帧 (日志/SHM 配置/进程控制/TD 控制帧等) 丢弃
    }
}

}  // namespace

DZ_API void dz_wait(DzContext* ctx) {
    // 纯等待: 不做任何定时器计算与触发 (唯一推进点在 dz_next_event)。
    // 定时器是事件流的一部分, 契约要求策略调用 dz_next_event 消费;
    // 不调用则不触发, 不设防。
    if (ctx->has_pending_timers()) {
        const uint32_t timeout_ms = ctx->next_timer_wait_ms();
        if (timeout_ms == 0) {
            return;  // 已到期: 免一次 syscall, 立即返回, 由 dz_next_event 触发
        }
        static_assert(noexcept(ctx->sem.wait_for(timeout_ms)));
        (void)ctx->sem.wait_for(timeout_ms);
    } else {
        static_assert(noexcept(ctx->sem.wait()));
        ctx->sem.wait();
    }
}

DZ_API const void* dz_next_event(DzContext* ctx) {
    // 处理优先级: 回补帧 > 用户帧 > 定时器帧 > 内部帧。
    // 回补帧 (断档回补合成的 Dz*Report) 前置 FIFO 派发: 先于任何 shm 实时帧,
    // 保证回补数据按 seq 序在触发帧之后、后续实时帧之前送达 (契约 strategy:
    // 启动竞态窗口的缺失区间由回补补齐, 无断档)。
    // 回补帧已过 ingest gate (W/断档/去重), 不再重复过滤; 但 DZ_FRAME_ORDER_REPORT/DZ_FRAME_TRADE_REPORT 仍按
    // strategy_id 定向 (回补查询按账户全量, 含他策略/外部单, 仅本策略放行)。
    // 断档回补重试驱动 (契约 td-data-sync §5.2): 回补不足时每次调用查一次库
    // (无 sleep, 调用间隔即天然"短重试"间隔); 成功/耗尽后触发帧 + 回补帧经
    // 下面的 replay FIFO 前置派发。置于 replay 消费前, 使结果本次即可见。
    if (ctx->gap_retry.active) {
        try {
            drive_gap_retry(ctx);
        } catch (const std::exception& e) {
            dz_diag((std::string("gap retry drive failed: ") + e.what()).c_str());
        } catch (...) {
            dz_diag("gap retry drive failed: unknown exception");
        }
    }
    for (;;) {
        const void* replay = ctx->pop_replay_frame();
        if (replay == nullptr) {
            break;
        }
        const DzFrameType rtype = shm::FrameView(static_cast<const std::byte*>(replay)).type();
        if (rtype == DZ_FRAME_ORDER_REPORT) {
            if (!is_own_report<DzOrderReport>(ctx, static_cast<const std::byte*>(replay))) {
                continue;  // 他策略/外部单回补: 定向丢弃
            }
        } else if (rtype == DZ_FRAME_TRADE_REPORT) {
            if (!is_own_report<DzTradeReport>(ctx, static_cast<const std::byte*>(replay))) {
                continue;
            }
        }
        return replay;
    }
    // 用户帧路径零计时器开销 (不 tick 不 pop); 通道无用户帧 (空/32 让位) 时才
    // tick 定时器并返回定时器帧; 内部帧在扫描用户帧时顺带消费 (轻量处理,
    // 预加载重活已由随机延迟定时器承担)。
    uint32_t internal_count = 0;
    for (;;) {
        static_assert(noexcept(ctx->event_reader.next_frame()));
        const std::byte* frame = ctx->event_reader.next_frame();
        if (frame == nullptr) {
            break;  // 通道空: 服务计时器
        }
        const DzFrameType type = shm::FrameView(frame).type();
        try {
            if (dispatch_frame(ctx, frame, type)) {
                return frame;  // 用户帧最高优先
            }
        } catch (const std::exception& e) {
            dz_diag((std::string("frame dispatch failed: ") + e.what()).c_str());
        } catch (...) {
            dz_diag("frame dispatch failed: unknown exception");
        }
        if (++internal_count >= kMaxInternalFramesPerCall) {
            break;  // 连续 32 条内部帧: 让位, 防饿死 dz_next_md
        }
    }
    // 无用户帧可给 (通道空或 32 让位): 服务计时器
    try {
        ctx->tick_timers();
    } catch (const std::exception& e) {
        dz_diag((std::string("timer tick failed: ") + e.what()).c_str());
    } catch (...) {
        dz_diag("timer tick failed: unknown exception");
    }
    return ctx->pop_timer_frame();
}

DZ_API const void* dz_next_md(DzContext* ctx) {
    static_assert(noexcept(ctx->md_reader.next_frame()));
    return ctx->md_reader.next_frame();
}

DZ_API const char* dz_md_source_name(DzContext* ctx) { return ctx->md_source_name; }

DZ_API void dz_notify_self(DzContext* ctx) { ctx->sem.notify(); }

/* ── 定时器 ── */

namespace {

bool valid_delay_ms(int32_t delay_ms) { return delay_ms > 0; }

bool valid_time_of_day_ms(int32_t time_of_day_ms) {
    return time_of_day_ms >= 0 && time_of_day_ms <= 86'399'999;
}

DzTimerId schedule_relative(DzContext* ctx,
                            int32_t delay_ms,
                            DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        if (kind == DzContext::UserTimerEntry::Kind::Every) {
            entry.interval = std::chrono::milliseconds{delay_ms};
        }
        entry.next_deadline = DzContext::TimerClock::now() + std::chrono::milliseconds{delay_ms};
        return ctx->schedule_user_timer(std::move(entry));
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return DZ_TIMER_INVALID;
}

DzTimerId schedule_time_of_day(DzContext* ctx,
                               int32_t time_of_day_ms,
                               DzContext::UserTimerEntry::Kind kind) {
    try {
        DzContext::UserTimerEntry entry;
        entry.kind = kind;
        entry.time_of_day_ms = time_of_day_ms;
        entry.next_deadline = DzContext::TimerClock::now() + next_time_of_day_delay(time_of_day_ms);
        return ctx->schedule_user_timer(std::move(entry));
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return DZ_TIMER_INVALID;
}

}  // namespace

DZ_API DzTimerId dz_schedule_after(DzContext* ctx, int32_t delay_ms) {
    if (!valid_delay_ms(delay_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "delay_ms must be > 0");
        return DZ_TIMER_INVALID;
    }
    return schedule_relative(ctx, delay_ms, DzContext::UserTimerEntry::Kind::After);
}

DZ_API DzTimerId dz_schedule_every(DzContext* ctx, int32_t delay_ms) {
    if (!valid_delay_ms(delay_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "delay_ms must be > 0");
        return DZ_TIMER_INVALID;
    }
    return schedule_relative(ctx, delay_ms, DzContext::UserTimerEntry::Kind::Every);
}

DZ_API DzTimerId dz_schedule_at(DzContext* ctx, int32_t time_of_day_ms) {
    if (!valid_time_of_day_ms(time_of_day_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "time_of_day_ms must be in [0, 86_399_999]");
        return DZ_TIMER_INVALID;
    }
    return schedule_time_of_day(ctx, time_of_day_ms, DzContext::UserTimerEntry::Kind::AtOnce);
}

DZ_API DzTimerId dz_schedule_daily(DzContext* ctx, int32_t time_of_day_ms) {
    if (!valid_time_of_day_ms(time_of_day_ms)) {
        LastError::set(DZ_EC_INVALID_PARAM, "time_of_day_ms must be in [0, 86_399_999]");
        return DZ_TIMER_INVALID;
    }
    return schedule_time_of_day(ctx, time_of_day_ms, DzContext::UserTimerEntry::Kind::Daily);
}

DZ_API bool dz_schedule_cancel(DzContext* ctx, DzTimerId timer_id) {
    if (!ctx->cancel_user_timer(timer_id)) {
        LastError::set(DZ_EC_TIMER_NOT_FOUND, "timer not found");
        return false;
    }
    return true;
}

DZ_API bool dz_schedule_cancel_all(DzContext* ctx) {
    ctx->cancel_all_user_timers();
    return true;
}

DZ_API const char* dz_strategy_home(DzContext* ctx) { return ctx->strategy_home.c_str(); }

DZ_API const char* dz_strategy_id(DzContext* ctx) { return ctx->strategy_id; }

/* ── 交易接口 ── */

DZ_API DzOrderId dz_place_order(DzContext* ctx,
                                const char* account_id,
                                const char* instrument_id,
                                DzDirection direction,
                                DzPriceType price_type,
                                double price,
                                DzVolume volume,
                                DzPositionEffect position_effect) {
    // 本函数经 extern "C" 边界导出, 不允许异常逃逸; 体内所有操作均为 noexcept,
    // 故无需 try/catch。static_assert 强制约束, 防止后续引入会抛异常的操作时
    // 异常静默穿越 C ABI 边界 (UB)。
    static_assert(
        noexcept(ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_REQ, sizeof(DzOrderReq))));
    static_assert(noexcept(ctx->order_id.generate()));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    auto* req = reinterpret_cast<DzOrderReq*>(
        ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_REQ, sizeof(DzOrderReq)));
    if (req == nullptr) {
        // open_frame 失败时已设置 LastError, 直接透传
        return -1;
    }
    const auto order_id = ctx->order_id.generate();
    copy_string(req->strategy_id, ctx->strategy_id, true);
    copy_string(req->account_id, account_id, true);
    copy_string(req->instrument_id, instrument_id, true);
    req->remark[0] = '\0';
    req->direction = direction;
    req->price_type = price_type;
    req->price = price;
    req->volume = volume;
    req->position_effect = position_effect;
    req->order_id = order_id;
    ctx->event_writer.close_frame();
    ctx->event_writer.notify_subscribers();
    return order_id;
}

DZ_API bool dz_cancel_order(DzContext* ctx, const char* account_id, DzOrderId order_id) {
    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(
        ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_CANCEL_REQ, sizeof(DzOrderCancelReq))));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    auto* req = reinterpret_cast<DzOrderCancelReq*>(
        ctx->event_writer.open_frame(DZ_FRAME_TD_ORDER_CANCEL_REQ, sizeof(DzOrderCancelReq)));
    if (req == nullptr) {
        // open_frame 失败时已设置 LastError, 直接透传
        return false;
    }
    req->order_id = order_id;
    copy_string(req->account_id, account_id, true);
    ctx->event_writer.close_frame();
    ctx->event_writer.notify_subscribers();
    return true;
}

namespace {

bool write_subscribe_req(DzContext* ctx, SubscribeReq& req) {
    try {
        if (!shm::write_ext_inst_json(ctx->event_writer, DZ_FRAME_REQUEST_MD_SUBSCRIBE,
                                      ctx->md_source_name, req)) {
            return false;
        }
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

void fill_instruments(SubscribeReq& req, const char* const instruments[], uint32_t count) {
    req.instruments.reserve(count);
    std::unordered_set<std::string_view> seen;
    seen.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (instruments[i] == nullptr) {
            continue;
        }
        std::string_view inst(instruments[i]);
        if (seen.insert(inst).second) {
            req.instruments.emplace_back(inst);
        }
    }
}

}  // namespace

DZ_API bool dz_subscribe(DzContext* ctx,
                         const char* const instruments[],
                         uint32_t count,
                         bool replace_previous) {
    if (instruments == nullptr || count == 0) {
        LastError::set(DZ_EC_INVALID_PARAM, "instruments is null or count is 0");
        return false;
    }

    std::vector<std::string> new_instruments;
    new_instruments.reserve(count);
    {
        std::unordered_set<std::string_view> seen;
        seen.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (instruments[i] == nullptr) {
                continue;
            }
            const std::string_view inst(instruments[i]);
            if (seen.insert(inst).second) {
                new_instruments.emplace_back(inst);
            }
        }
    }
    if (new_instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    // 候选期望集合：不直接提交，写帧成功后才替换。
    std::set<std::string> candidate =
        replace_previous ? std::set<std::string>{} : ctx->md_desired_instruments;
    candidate.insert(new_instruments.begin(), new_instruments.end());

    SubscribeReq req;
    req.instance_id = strategy_identity(ctx->strategy_id);
    req.action = SubscribeAction::Subscribe;
    req.replace = replace_previous;
    req.instruments.assign(candidate.begin(), candidate.end());
    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    if (!write_subscribe_req(ctx, req)) {
        return false;
    }
    ctx->md_desired_instruments = std::move(candidate);
    return true;
}

DZ_API bool dz_unsubscribe(DzContext* ctx, const char* const instruments[], uint32_t count) {
    SubscribeReq req;
    req.instance_id = strategy_identity(ctx->strategy_id);

    if (instruments == nullptr || count == 0) {
        req.action = SubscribeAction::UnsubscribeAll;
        if (!write_subscribe_req(ctx, req)) {
            return false;
        }
        ctx->md_desired_instruments.clear();
        return true;
    }

    req.action = SubscribeAction::Unsubscribe;
    fill_instruments(req, instruments, count);
    if (req.instruments.empty()) {
        LastError::set(DZ_EC_INVALID_PARAM, "no valid instruments after dedup");
        return false;
    }

    std::set<std::string> candidate = ctx->md_desired_instruments;
    for (const auto& inst : req.instruments) {
        candidate.erase(inst);
    }

    if (!write_subscribe_req(ctx, req)) {
        return false;
    }
    ctx->md_desired_instruments = std::move(candidate);
    return true;
}

namespace {

void on_md_started_internal(DzContext* ctx, const std::byte* frame) {
    // dz_next_event 内部自动补订阅: 仅本策略绑定行情源, 全量重放期望集合。
    try {
        const shm::FrameView view(frame);
        if (view.type() != DZ_FRAME_NOTIFY_MD_STARTED) {
            return;  // 防御: handle_internal_frame 已按类型分派
        }
        if (std::string_view(view.ext_inst_id()) != ctx->md_source_name) {
            return;  // 非本策略源，静默忽略
        }
        if (ctx->md_desired_instruments.empty()) {
            return;  // 无需补订阅
        }

        SubscribeReq req;
        req.instance_id = strategy_identity(ctx->strategy_id);
        req.action = SubscribeAction::Subscribe;
        req.replace = true;
        req.instruments.assign(ctx->md_desired_instruments.begin(),
                               ctx->md_desired_instruments.end());
        (void)write_subscribe_req(ctx, req);
    } catch (const Exception& e) {
        dz_diag((std::string("auto resubscribe on md started failed: ") + e.what()).c_str());
    } catch (const std::exception& e) {
        dz_diag((std::string("auto resubscribe on md started failed: ") + e.what()).c_str());
    } catch (...) {
        dz_diag("auto resubscribe on md started failed: unknown exception");
    }
}

}  // namespace

/* ── 逻辑持仓 ── */

DZ_API bool dz_set_logical_position(DzContext* ctx,
                                    const char* account_id,
                                    const char* instrument_id,
                                    int32_t net_volume) {
    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(
        ctx->event_writer.open_frame(DZ_FRAME_SET_LOGICAL_POSITION, sizeof(DzLogicalPosition))));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    auto* pos = reinterpret_cast<DzLogicalPosition*>(
        ctx->event_writer.open_frame(DZ_FRAME_SET_LOGICAL_POSITION, sizeof(DzLogicalPosition)));
    if (pos == nullptr) {
        // open_frame 失败时已设置 LastError, 直接透传
        return false;
    }
    copy_string(pos->account_id, account_id, true);
    copy_string(pos->instrument_id, instrument_id, true);
    copy_string(pos->strategy_id, ctx->strategy_id, true);
    pos->net_volume = net_volume;
    ctx->event_writer.close_frame();
    ctx->event_writer.notify_subscribers();
    return true;
}

DZ_API bool dz_query_account_status(DzContext* ctx, const char* account_id) {
    // 同 dz_set_logical_position: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept,
    // 免 try/catch, static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(ctx->event_writer.open_frame(
        DZ_FRAME_TD_QUERY_ACCOUNT_STATUS, sizeof(DzAccountStatusReq))));
    static_assert(noexcept(ctx->event_writer.close_frame()));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    auto* req = reinterpret_cast<DzAccountStatusReq*>(
        ctx->event_writer.open_frame(DZ_FRAME_TD_QUERY_ACCOUNT_STATUS, sizeof(DzAccountStatusReq)));
    if (req == nullptr) {
        // open_frame 失败时已设置 LastError, 直接透传
        return false;
    }
    dztrader::copy_string(req->account_id, account_id == nullptr ? "" : account_id, true);
    ctx->event_writer.close_frame();
    ctx->event_writer.notify_subscribers();
    return true;
}

namespace {

// DzNotifyLevel -> 字符串, 与 log level 规范全称一致 (契约 notify-ui level 字段)
const char* notify_level_to_string(DzNotifyLevel level) {
    switch (level) {
        case DZ_NOTIFY_INFO:
            return "info";
        case DZ_NOTIFY_WARN:
            return "warning";
        case DZ_NOTIFY_ERROR:
            return "error";
        default:
            return "error";
    }
}

}  // namespace

/* ── UI 通知 ── */

DZ_API bool dz_notify_ui(DzContext* ctx, DzNotifyLevel level, const char* message, bool popup) {
    if (!message) {
        LastError::set(DZ_EC_INVALID_PARAM, "message is null");
        return false;
    }
    if (level != DZ_NOTIFY_INFO && level != DZ_NOTIFY_WARN && level != DZ_NOTIFY_ERROR) {
        LastError::set(DZ_EC_INVALID_PARAM, "level is invalid");
        return false;
    }
    try {
        nlohmann::json payload = {
            {"source", ctx->strategy_id},
            {"level", notify_level_to_string(level)},
            {"message", message},
            {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
            {"popup", popup},
        };
        if (!shm::write_ext_json(ctx->event_writer, DZ_FRAME_NOTIFY_UI, payload)) {
            LastError::set(DZ_EC_INTERNAL, "frame write failed");
            return false;
        }
        ctx->event_writer.notify_subscribers();
        return true;
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_INTERNAL, e.what());
    } catch (...) {
        LastError::set(DZ_EC_INTERNAL, "unknown exception");
    }
    return false;
}

DZ_API bool dz_output_ui(DzContext* ctx, const char* data) {
    if (!data) {
        LastError::set(DZ_EC_INVALID_PARAM, "data is null");
        return false;
    }

    // 同 dz_place_order: extern "C" 边界不允许异常逃逸; 体内操作均 noexcept, 免 try/catch,
    // static_assert 防止后续引入会抛异常的操作。
    static_assert(noexcept(output_ui_max_payload(ctx->event_writer.page_size())));
    static_assert(noexcept(ctx->event_writer.write_ext_inst_frame(
        DZ_FRAME_OUTPUT_UI, ctx->strategy_id, reinterpret_cast<const std::byte*>(data), 0u)));
    static_assert(noexcept(ctx->event_writer.notify_subscribers()));

    const auto len = strlen(data);
    // 页感知上限
    const auto cap = output_ui_max_payload(ctx->event_writer.page_size());
    if (len > 0 && cap == 0) {
        LastError::set(DZ_EC_BUFFER_TOO_SMALL, "page too small for output frame");
        return false;
    }
    const auto data_len = static_cast<uint32_t>(std::min<uint64_t>(len, cap));
    if (!ctx->event_writer.write_ext_inst_frame(DZ_FRAME_OUTPUT_UI, ctx->strategy_id,
                                                reinterpret_cast<const std::byte*>(data),
                                                data_len)) {
        // write_ext_inst_frame 为 noexcept bool: 唯一失败路径是 open_frame 返回 nullptr,
        // 其每条失败分支均已设置 LastError (writer.cpp), 此处直接透传, 不自设错误码
        return false;
    }
    ctx->event_writer.notify_subscribers();
    return true;
}

/* ── 数据库接口 ── */

namespace {

/// 查询结果装入 VectorResultSet (不新建 impl 类, 复用 vector_result_set)
std::unique_ptr<DzResultSet> db_rs_from_result(strategy_api_internal::DbQueryResult result) {
    auto rs = std::make_unique<DzResultSet>();
    rs->impl = std::make_unique<strategy_api_internal::VectorResultSet>(
        std::move(result.columns), std::move(result.rows));
    return rs;
}

using strategy_api_internal::db_open_readonly;
using strategy_api_internal::db_query_order_trade;
using strategy_api_internal::db_query_position;
using strategy_api_internal::db_query_trading_account;
using strategy_api_internal::to_db_query_result;

/// 逗号分隔列名 -> 列名列表 (逐项 trim 空格, 空项跳过); NULL/"" -> 空 (调用方回填承诺列)
std::vector<std::string> parse_fields(const char* fields) {
    std::vector<std::string> out;
    if (fields == nullptr || fields[0] == '\0') {
        return out;
    }
    std::string_view rest(fields);
    while (true) {
        const size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        const size_t begin = item.find_first_not_of(" \t");
        if (begin != std::string_view::npos) {
            const size_t end = item.find_last_not_of(" \t");
            out.emplace_back(item.substr(begin, end - begin + 1));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return out;
}

}  // namespace

DZ_API DzDatabase* dz_db_open(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        LastError::set(DZ_EC_INVALID_PARAM, "db path is null");
        return NULL;
    }
    try {
        // 只读打开: 库文件不存在时报错返回 NULL (策略自行降级)。
        return db_open_readonly(path).release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}
DZ_API bool dz_db_close(DzDatabase* db) {
    if (db == nullptr) {
        return true;  // NULL 安全: no-op 视为成功
    }
    delete db;  // NOLINT
    return true;
}

/* ── DzResultSet ── */

DZ_API bool dz_resultset_next(DzResultSet* rs) { return rs && rs->impl ? rs->impl->next() : false; }

DZ_API int32_t dz_resultset_status(DzResultSet* rs) {
    return rs && rs->impl ? rs->impl->status() : DZ_EC_INTERNAL;
}

DZ_API uint32_t dz_resultset_column_count(DzResultSet* rs) {
    return rs && rs->impl ? rs->impl->column_count() : 0u;
}

DZ_API DzColumnType dz_resultset_column_type(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->column_type(index) : DZ_COL_TYPE_NULL;
}

DZ_API const char* dz_resultset_column_name(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->column_name(index) : "";
}

DZ_API bool dz_resultset_is_null(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->is_null(index) : true;
}

DZ_API int64_t dz_resultset_get_int64(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_int64(index) : INT64_MAX;
}

DZ_API double dz_resultset_get_float64(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_float64(index) : DBL_MAX;
}

DZ_API const char* dz_resultset_get_string(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_string(index) : "";
}

DZ_API bool dz_resultset_get_bool(DzResultSet* rs, uint32_t index) {
    return rs && rs->impl ? rs->impl->get_bool(index) : false;
}

DZ_API void dz_resultset_close(DzResultSet* rs) {
    delete rs;  // NOLINT
    rs = nullptr;
}

/* ── 查询接口 ── */

DZ_API DzResultSet* dz_db_query_order(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id) {
    if (db == nullptr || db->session == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "db handle is null");
        return NULL;
    }
    try {
        return db_rs_from_result(db_query_order_trade(
                   db, account_id ? account_id : "", instrument_id ? instrument_id : "", "orders",
                   /*order_by_seq=*/true))
            .release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}
DZ_API DzResultSet* dz_db_query_trade(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id) {
    if (db == nullptr || db->session == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "db handle is null");
        return NULL;
    }
    try {
        return db_rs_from_result(db_query_order_trade(
                   db, account_id ? account_id : "", instrument_id ? instrument_id : "", "trades",
                   /*order_by_seq=*/true))
            .release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}
DZ_API DzResultSet* dz_db_query_position(DzDatabase* db,
                                         const char* account_id,
                                         const char* instrument_id) {
    if (db == nullptr || db->session == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "db handle is null");
        return NULL;
    }
    try {
        return db_rs_from_result(db_query_position(
                   db, account_id ? account_id : "", instrument_id ? instrument_id : ""))
            .release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}
DZ_API DzResultSet* dz_db_query_trading_account(DzDatabase* db, const char* account_id) {
    if (db == nullptr || db->session == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "db handle is null");
        return NULL;
    }
    try {
        return db_rs_from_result(db_query_trading_account(db, account_id ? account_id : ""))
            .release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}
DZ_API DzResultSet* dz_db_query_bar(DzDatabase* db,
                                    const char* instrument_id,
                                    int32_t bar_period,
                                    DzAdjustType adjust_type,
                                    DzDate start_date,
                                    DzDate end_date) {
    (void)db;
    (void)instrument_id;
    (void)bar_period;
    (void)adjust_type;
    (void)start_date;
    (void)end_date;
    LastError::set(DZ_EC_SYSTEM, "bar query not implemented");
    return NULL;
}
DZ_API DzResultSet* dz_db_query_instruments(DzDatabase* db,
                                            const char* instrument_id,
                                            const char* fields) {
    if (db == nullptr || db->session == nullptr) {
        LastError::set(DZ_EC_INVALID_PARAM, "db handle is null");
        return NULL;
    }
    try {
        std::vector<std::string> requested = parse_fields(fields);
        if (requested.empty()) {
            requested = tdstore::instrument_promised_fields();
        } else {
            // 显式请求字段必须过承诺列白名单 (契约 instrument §8: v3 保留列不可查询;
            // records_store 白名单 = 物理全列, 含 3 个保留列, 不满足 C API 词表)。
            const auto& promised = tdstore::instrument_promised_fields();
            for (const std::string& name : requested) {
                if (std::find(promised.begin(), promised.end(), name) == promised.end()) {
                    throw Exception(DZ_EC_INVALID_PARAM, "unknown field: {}", name);
                }
            }
        }
        auto result = tdstore::query_instruments(
            *db->session, instrument_id != nullptr ? instrument_id : "", requested);
        return db_rs_from_result(to_db_query_result(std::move(result))).release();
    } catch (const Exception& e) {
        LastError::set(e.code(), e.what());
    } catch (const std::exception& e) {
        LastError::set(DZ_EC_SYSTEM, e.what());
    } catch (...) {
        LastError::set(DZ_EC_SYSTEM, "unknown exception");
    }
    return NULL;
}

/* ── 错误信息三函数 ── */

DZ_API int32_t dz_errcode(void) { return dztrader::LastError::code(); }
DZ_API const char* dz_errstr(int32_t errcode) { return dztrader::LastError::str(errcode); }
DZ_API const char* dz_errmsg(void) { return dztrader::LastError::msg(); }

/* ── SDK 诊断输出 ── */

DZ_API void dz_diag(const char* message) {
    if (message == nullptr) {
        return;
    }
    std::fputs("[dzsdk] ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
}

/* ── 版本信息 ── */

DZ_API int32_t dz_version_major(void) { return DZ_VERSION_MAJOR; }
DZ_API int32_t dz_version_minor(void) { return DZ_VERSION_MINOR; }
DZ_API int32_t dz_version_patch(void) { return DZ_VERSION_PATCH; }
DZ_API const char* dz_version_string(void) { return DZ_VERSION_STRING; }
DZ_API int32_t dz_version_hex(void) { return DZ_VERSION_HEX; }
