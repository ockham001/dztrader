#include "td_data_service.h"

#include <dztrader/date_time/date.h>
#include <dztrader/td_ingest.h>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// sqlite3 列读取: 文本列（空/NULL 回落空串）。
std::string col_text(sqlite3_stmt* stmt, int col) {
    const auto* ptr = sqlite3_column_text(stmt, col);
    if (ptr != nullptr) {
        return reinterpret_cast<const char*>(ptr);
    }
    return {};
}

/// sqlite3 列读取: 整数列。
int64_t col_int(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_int64(stmt, col);
}

/// sqlite3 列读取: 浮点列。
double col_double(sqlite3_stmt* stmt, int col) {
    return sqlite3_column_double(stmt, col);
}

/// "YYYYMMDD" 文本 -> DzDate (距纪元天数); 非法回落 0。
int32_t parse_trading_day_to_epoch(const std::string& text) {
    if (text.size() != 8) {
        return 0;
    }
    try {
        const int32_t y = std::stoi(text.substr(0, 4));
        const int32_t m = std::stoi(text.substr(4, 2));
        const int32_t d = std::stoi(text.substr(6, 2));
        return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
    } catch (...) {
        return 0;
    }
}

/// 查该账户最新交易日 (YYYYMMDD) — orders/trades 表的最大 trading_day 文本 (字典序
/// = 时间序)。镜像日界过滤 (终检发现 G) 与日切判定用; 无行回落空串。
std::string latest_trading_day(sqlite3* db, const std::string& account_id) {
    for (const char* table : {"orders", "trades"}) {
        sqlite3_stmt* stmt = nullptr;
        const std::string sql =
            std::string("SELECT MAX(trading_day) FROM ") + table + " WHERE account_id = ?";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const auto* ptr = sqlite3_column_text(stmt, 0);
                if (ptr != nullptr) {
                    const std::string day = reinterpret_cast<const char*>(ptr);
                    sqlite3_finalize(stmt);
                    return day;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    return {};
}

}  // namespace

TdDataService::TdDataService(FrameRouter& router, std::function<std::string()> td_db_path)
    : router_(router), td_db_path_(std::move(td_db_path)) {
    register_handlers(router_);
}

void TdDataService::register_handlers(FrameRouter& router) {
    // 2000-2003 为二进制 struct payload（write_struct 写帧），register_json 的 JSON decode
    // 必失败——必须 register_raw。handler 在监听线程同步执行、FrameView 有效期内解析，
    // 拷贝字段后经 poster 投递到 IO 线程执行实际更新（与 REST/WS 连接回调同线程串行，
    // 镜像/gate 访问无竞争；严禁捕获 FrameView 引用）。
    const auto post = [this](std::function<void()> f) { router_.poster()(std::move(f)); };
    router.register_raw(DZ_FRAME_ORDER_REPORT,
        [this, post](const shm::FrameView& v) {
            const DzOrderReport rpt = v.payload<DzOrderReport>();  // 值拷贝, 投递安全
            post([this, rpt]() { on_order_report(rpt); });
        });
    router.register_raw(DZ_FRAME_TRADE_REPORT,
        [this, post](const shm::FrameView& v) {
            const DzTradeReport rpt = v.payload<DzTradeReport>();
            post([this, rpt]() { on_trade_report(rpt); });
        });
    router.register_raw(DZ_FRAME_POSITION_INFO,
        [this, post](const shm::FrameView& v) {
            const DzPositionInfo pos = v.payload<DzPositionInfo>();
            post([this, pos]() { on_position_info(pos); });
        });
    router.register_raw(DZ_FRAME_TRADING_ACCOUNT,
        [this, post](const shm::FrameView& v) {
            const DzTradingAccount acct = v.payload<DzTradingAccount>();
            post([this, acct]() { on_trading_account(acct); });
        });

    // 2018 ACCOUNT_STATUS (struct payload, 契约 account-status): Ready → rebuild();
    // Offline → 清空该账户镜像 (spec §5.5"清空必须显式", 防幽灵持仓残留)。
    router.register_raw(DZ_FRAME_ACCOUNT_STATUS,
        [this, post](const shm::FrameView& v) {
            const DzAccountStatus st = v.payload<DzAccountStatus>();
            post([this, st]() { on_account_status(st); });
        });
}

void TdDataService::on_order_report(const DzOrderReport& rpt) {
    if (!ingest(rpt)) {
        return;
    }
    orders_.push_back(rpt);
}

void TdDataService::on_trade_report(const DzTradeReport& rpt) {
    if (!ingest(rpt)) {
        return;
    }
    // 成交去重二道防线 (spec §5.4): (account_id, trading_day, trade_id) 存在性。
    // 日期以帧 date 为真源 (实时/回补路径均已填 DzDate), 转 "YYYYMMDD" 对齐 gate 段键。
    if (!gate_.admit_trade(rpt.account_id, trading_day_of(rpt.date), rpt.trade_id)) {
        return;
    }
    trades_.push_back(rpt);
}

void TdDataService::on_position_info(const DzPositionInfo& pos) {
    if (!ingest(pos)) {
        return;
    }
    // 绝对态覆盖 (spec §3.2/§4.1): 同 (account_id, instrument_id, direction) 覆盖。
    const auto key = position_key(pos);
    for (auto it = positions_.begin(); it != positions_.end(); ++it) {
        if (position_key(*it) == key) {
            *it = pos;
            return;
        }
    }
    positions_.push_back(pos);
}

void TdDataService::on_trading_account(const DzTradingAccount& acct) {
    if (!ingest(acct)) {
        return;
    }
    // 绝对态覆盖: 同 account_id 覆盖。
    for (auto it = trading_accounts_.begin(); it != trading_accounts_.end(); ++it) {
        if (std::strcmp(it->account_id, acct.account_id) == 0) {
            *it = acct;
            return;
        }
    }
    trading_accounts_.push_back(acct);
}

void TdDataService::on_account_status(const DzAccountStatus& st) {
    const std::string account(st.account_id);
    if (account.empty()) {
        return;
    }
    switch (st.state) {
        case DZ_ACCOUNT_READY:
            // 契约 account-status/td-data-sync: Ready 时 td 已完成登录收尾协议
            // (持仓/资金查询 → persist flush → 广播 Ready), DB 已稳定 → 重建该账户镜像。
            // 按账户过滤重建: 只清 + 只重设触发账户, 不动其他账户镜像/水位 (多账户下
            // A 在途追加帧 "先广播后异步落库" 不被 B 的 Ready 误清)。
            rebuild(account);
            break;
        case DZ_ACCOUNT_OFFLINE:
            // spec §5.5"清空必须显式": 重置后若无持仓帧到达, 旧镜像幽灵持仓永无人覆盖。
            clear_account(account);
            break;
        default:
            break;  // LoggingIn: 不动作 (镜像保持到 Ready/Offline)
    }
}

void TdDataService::clear_account(const std::string& account_id, bool reset_gate) {
    const auto erase_for = [&](auto& vec, auto&& get_key) {
        for (auto it = vec.begin(); it != vec.end();) {
            if (get_key(*it) == account_id) {
                it = vec.erase(it);
            } else {
                ++it;
            }
        }
    };
    // positions/trading_accounts 按账户清; orders/trades 同样按账户清 (镜像为全量)。
    erase_for(positions_, [](const DzPositionInfo& p) { return std::string(p.account_id); });
    erase_for(trading_accounts_,
              [](const DzTradingAccount& a) { return std::string(a.account_id); });
    erase_for(orders_, [](const DzOrderReport& o) { return std::string(o.account_id); });
    erase_for(trades_, [](const DzTradeReport& t) { return std::string(t.account_id); });
    // gate 重置: 新水位 0 (Offline 后无快照, 重新 Ready 时再重建/设 W)。
    // 终检发现 E: 断档 gap 补齐路径 (reset_gate=false) 只清镜像不重置 gate —
    // 保留 last_applied 防后续帧 re-gap 循环。
    if (reset_gate) {
        gate_.reset_account(account_id, 0);
    }
    SPDLOG_INFO("td data mirror cleared | account={}", account_id);
}

void TdDataService::rebuild(const std::string& account_id, bool reset_gate) {
    // 1. 只清触发账户镜像 (spec §5.5: 重建=新基准, 旧镜像残留会被误当当前状态)。
    //    多账户下不动其他账户镜像/水位, 避免误清他账户在途追加数据 (orders/trades
    //    是追加流, 不清自愈 — 在途帧 seq 已在 DB 提交前入镜像, 整库清则永久缺条)。
    //    reset_gate=false (断档 gap 补齐): 只清镜像不重置 gate (保留 last_applied)。
    clear_account(account_id, reset_gate);

    const std::string path = td_db_path_();
    if (path.empty()) {
        // 库不可用降级: 该账户镜像已清 + W=0 (reset_account 置 has_watermark 且 w=0,
        // 等价全放行), 绝不保留旧 W — 否则旧 W 过滤掉快照不含的帧, 幽灵抑制 (有帧却镜像空白)。
        SPDLOG_WARN("td db path empty, rebuild skipped | account={} mirror=empty W=0", account_id);
        return;
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        SPDLOG_WARN("td db open failed (rebuild as empty) | account={} path={} err={} W=0",
                    account_id, path, db != nullptr ? sqlite3_errmsg(db) : "unknown");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return;
    }
    // 只读连接设 busy_timeout (与生产写端 td_persist_writer.cpp:125 一致):
    // td Writer 批量提交持写锁窗口内 rebuild() 不得立即 SQLITE_BUSY 降级 (清镜像+W=0) —
    // 否则 dzweb 镜像永久停在"全放行但无快照", 直到下一 Ready/Offline 才重试.
    sqlite3_busy_timeout(db, 5000);

    // 四表读取包进单只读事务 (BEGIN DEFERRED 快照读, 与 SDK load_all_watermarks 同型):
    // 分次独立查询存在竞态 — 先查表 A 后写端提交 A 中 seq∈(W_A, W] 的行, 再查表 B 得
    // W_B ≥ W, 该行"快照没有却被 W 判定已含" → 静默误吞。单事务使首条 SELECT 起
    // 四表共享同一快照 (SHARED 锁保持到 COMMIT), 写端短暂阻塞由 busy_timeout 吸收。
    char* begin_err = nullptr;
    if (sqlite3_exec(db, "BEGIN DEFERRED", nullptr, nullptr, &begin_err) != SQLITE_OK) {
        SPDLOG_WARN("td db begin snapshot failed (rebuild as empty) | account={} err={}",
                    account_id, begin_err != nullptr ? begin_err : "unknown");
        sqlite3_free(begin_err);
        sqlite3_close(db);
        return;
    }

    // 2. 四表按账户过滤查询重建镜像 (spec §3.3: 只读打开, 表缺失/查询失败跳过该表)
    //    镜像键: positions (account_id,instrument_id,direction) / trading_accounts account_id。
    //    account_id 列均为索引首列 (orders/trades/positions UNIQUE、trading_accounts PK),
    //    按账户过滤只扫该账户行。
    //    终检发现 G【Important】: orders/trades 镜像加当日过滤 (trading_day = 最新交易日),
    //    防镜像随历史线性增长 (0.9GB/年)。positions/trading_accounts 单行绝对态无需日过滤。
    //    W 查询不带日过滤 (正确性: W 取四表 MAX(seq), 跨日累积单调)。
    const std::string day = latest_trading_day(db, account_id);
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT account_id, trading_day, instrument_id, exchange_id,"
                               " direction, volume, frozen_volume, today_volume, yd_volume,"
                               " price, seq FROM positions WHERE account_id = ?",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                DzPositionInfo p{};
                dztrader::copy_string(p.account_id, col_text(stmt, 0).c_str(), true);
                dztrader::copy_string(p.instrument_id, col_text(stmt, 2).c_str(), true);
                dztrader::copy_string(p.exchange_id, col_text(stmt, 3).c_str(), true);
                p.direction = static_cast<DzDirection>(col_int(stmt, 4));
                p.volume = static_cast<DzVolume>(col_int(stmt, 5));
                p.frozen_volume = static_cast<DzVolume>(col_int(stmt, 6));
                p.today_volume = static_cast<DzVolume>(col_int(stmt, 7));
                p.yd_volume = static_cast<DzVolume>(col_int(stmt, 8));
                p.price = col_double(stmt, 9);
                p.date = parse_trading_day_to_epoch(col_text(stmt, 1));
                p.seq = static_cast<uint64_t>(col_int(stmt, 10));
                positions_.push_back(p);
            }
        } else {
            SPDLOG_WARN("td db query failed | table=positions");
        }
        sqlite3_finalize(stmt);
    }
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT account_id, trading_day, balance, available, frozen,"
                               " commission, margin, withdraw_quota, deposit, withdraw, seq"
                               " FROM trading_accounts WHERE account_id = ?",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                DzTradingAccount a{};
                dztrader::copy_string(a.account_id, col_text(stmt, 0).c_str(), true);
                a.balance = col_double(stmt, 2);
                a.available = col_double(stmt, 3);
                a.frozen = col_double(stmt, 4);
                a.commission = col_double(stmt, 5);
                a.margin = col_double(stmt, 6);
                a.withdraw_quota = col_double(stmt, 7);
                a.deposit = col_double(stmt, 8);
                a.withdraw = col_double(stmt, 9);
                a.date = parse_trading_day_to_epoch(col_text(stmt, 1));
                a.seq = static_cast<uint64_t>(col_int(stmt, 10));
                trading_accounts_.push_back(a);
            }
        } else {
            SPDLOG_WARN("td db query failed | table=trading_accounts");
        }
        sqlite3_finalize(stmt);
    }
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT account_id, trading_day, order_id, instrument_id,"
                               " exchange_id, direction, position_effect, price_type, status,"
                               " price, volume, volume_traded, strategy_id, seq FROM orders"
                               " WHERE account_id = ? AND trading_day = ?",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, day.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                DzOrderReport o{};
                dztrader::copy_string(o.account_id, col_text(stmt, 0).c_str(), true);
                dztrader::copy_string(o.instrument_id, col_text(stmt, 3).c_str(), true);
                dztrader::copy_string(o.exchange_id, col_text(stmt, 4).c_str(), true);
                dztrader::copy_string(o.strategy_id, col_text(stmt, 12).c_str(), true);
                o.order_id = col_int(stmt, 2);
                o.direction = static_cast<DzDirection>(col_int(stmt, 5));
                o.position_effect = static_cast<DzPositionEffect>(col_int(stmt, 6));
                o.price_type = static_cast<DzPriceType>(col_int(stmt, 7));
                o.status = static_cast<DzOrderStatus>(col_int(stmt, 8));
                o.price = col_double(stmt, 9);
                o.volume = static_cast<DzVolume>(col_int(stmt, 10));
                o.volume_traded = static_cast<DzVolume>(col_int(stmt, 11));
                o.date = parse_trading_day_to_epoch(col_text(stmt, 1));
                o.seq = static_cast<uint64_t>(col_int(stmt, 13));
                orders_.push_back(o);
            }
        } else {
            SPDLOG_WARN("td db query failed | table=orders");
        }
        sqlite3_finalize(stmt);
    }
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT account_id, trading_day, trade_id, order_id,"
                               " instrument_id, exchange_id, direction, position_effect,"
                               " price, volume, strategy_id, seq FROM trades"
                               " WHERE account_id = ? AND trading_day = ?",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, day.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                DzTradeReport t{};
                dztrader::copy_string(t.account_id, col_text(stmt, 0).c_str(), true);
                dztrader::copy_string(t.instrument_id, col_text(stmt, 4).c_str(), true);
                dztrader::copy_string(t.exchange_id, col_text(stmt, 5).c_str(), true);
                dztrader::copy_string(t.trade_id, col_text(stmt, 2).c_str(), true);
                dztrader::copy_string(t.strategy_id, col_text(stmt, 10).c_str(), true);
                t.order_id = col_int(stmt, 3);
                t.direction = static_cast<DzDirection>(col_int(stmt, 6));
                t.position_effect = static_cast<DzPositionEffect>(col_int(stmt, 7));
                t.price = col_double(stmt, 8);
                t.volume = static_cast<DzVolume>(col_int(stmt, 9));
                t.date = parse_trading_day_to_epoch(col_text(stmt, 1));
                t.seq = static_cast<uint64_t>(col_int(stmt, 11));
                trades_.push_back(t);
            }
        } else {
            SPDLOG_WARN("td db query failed | table=trades");
        }
        sqlite3_finalize(stmt);
    }
    // 提交快照读 (只读事务 COMMIT 仅结束读; 失败防御性回滚)
    char* commit_err = nullptr;
    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &commit_err) != SQLITE_OK) {
        SPDLOG_WARN("td db commit snapshot failed | account={} err={}", account_id,
                    commit_err != nullptr ? commit_err : "unknown");
        sqlite3_free(commit_err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    // 3. 设该账户新 W (spec §5.1/§2.1: W = 该账户 MAX(seq), 账户级独立水位; 过滤 seq ≤ W 的
    //    后续帧 — 快照已含)。四表 (委托/成交/持仓/资金) 共享一个计数器, 故取四表最大值。
    //    终检发现 E/G: 不再遍历镜像 (orders/trades 已按当日过滤, 跨日高 seq 会漏算) —
    //    直接对 DB 四表跑聚合查询 (每账户/每表 MAX(seq), 无日过滤), 与 SDK W 装载同语义。
    //    镜像中无该账户行 = DB 无快照 → 不设 W (等价 W=0 全放行)。
    uint64_t account_max_seq = 0;
    const auto accumulate = [&account_max_seq](uint64_t seq) {
        account_max_seq = std::max(account_max_seq, seq);
    };
    for (const char* table :
         {"positions", "trading_accounts", "orders", "trades"}) {
        sqlite3_stmt* stmt = nullptr;
        const std::string sql = std::string("SELECT COALESCE(MAX(seq), 0) FROM ") + table +
                                " WHERE account_id = ?";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                accumulate(static_cast<uint64_t>(col_int(stmt, 0)));
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    if (account_max_seq > 0) {
        gate_.set_watermark(account_id, account_max_seq);
    }
    SPDLOG_INFO("td data mirror rebuilt | account={} path={} positions={} accounts={} orders={} trades={} W={}",
                account_id, path, positions_.size(), trading_accounts_.size(), orders_.size(),
                trades_.size(), account_max_seq);
}

void TdDataService::set_watermark(const std::string& account_id, uint64_t w) {
    gate_.set_watermark(account_id, w);
}

std::string TdDataService::position_key(const DzPositionInfo& p) {
    return std::string(p.account_id) + "\x1f" + p.instrument_id + "\x1f" +
           std::to_string(p.direction);
}

const char* TdDataService::trading_day_of(int32_t date) {
    // 静态缓冲以返回 C 字符串给 gate（admit_trade 立即拷贝），单线程串行安全。
    static thread_local std::string cached;
    try {
        const dztrader::Date d{date};
        cached = std::format("{:04d}{:02d}{:02d}", d.year(), d.month(), d.day());
    } catch (...) {
        cached.clear();
    }
    return cached.c_str();
}

}  // namespace dztrader::webui
