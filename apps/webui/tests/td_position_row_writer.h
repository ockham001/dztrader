#ifndef DZTRADER_WEBUI_TESTS_TD_POSITION_ROW_WRITER_H_
#define DZTRADER_WEBUI_TESTS_TD_POSITION_ROW_WRITER_H_

/**
 * @file td_position_row_writer.h
 * @brief 测试用「往真 schema 的 trade.db 写一行持仓 / 资金」——**`apps/webui/tests/`
 * 内的唯一实现**。
 *
 * 为什么只在 dzweb 测试目录内收敛（`docs/unit-test-standard.md` §5.4）：`positions` 的 13 列
 * 与 `trading_accounts` 的 13 列都是建库声明的投影（真相源
 * `libs/tdstore/src/schema_catalog.cpp`）。同一目录内的测试文件各抄一份时，schema 加列/改名
 * 只会让抄得最晚的那份失败，其余静默继续写旧形状——而测试仍绿。
 *
 * **作用域说明**：本文件是 header-only 且被 `apps/webui/CMakeLists.txt` 的每个 `tests/`
 * 目标共享；它**不能**跨模块复用——`apps/ctp/tests/` 与 `libs/strategy_api/tests/` 是各自的
 * CMake target（链接的库、include 路径都不同），各有自己的行构造方式。因此「唯一实现」的
 * 范围是**本目录**，不是全仓；跨模块统一需要把这些夹具提出成共享测试库（另票，不在本票范围）。
 * 与同目录的 `td_order_row_writer.h` / `td_trade_row_writer.h` 同型：建库统一走
 * `tdstore::ensure_trade_db`，写行收敛到本文件。
 */
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace dztrader::test {

/// 一条持仓行的构造参数（默认值 = 契约 rest §2.9.4 的常见形态，用例只改关心的字段）。
struct TdPositionRowSeed {
    std::string account_id = "ACC1";
    std::string date = "20261009";  ///< 交易日 TEXT "YYYYMMDD"
    std::string instrument_id = "rb2601";
    std::string exchange_id = "SHFE";
    int direction = 1;  ///< DzDirection（1 = Long）
    int volume = 2;
    int frozen_volume = 0;
    int today_volume = 1;
    int yd_volume = 1;
    double price = 3500.0;
    uint64_t seq = 1;
    std::string gateway = "dztd_ctp";
};

/// 一条资金行的构造参数（#250 用到；本文件一并提供以免两份列清单）。
struct TdTradingAccountRowSeed {
    std::string account_id = "ACC1";
    std::string date = "20261009";
    double balance = 100000.0;
    double available = 85000.0;
    double frozen = 0.0;
    double commission = 0.0;
    double margin = 15000.0;
    double withdraw_quota = 80000.0;
    double deposit = 0.0;
    double withdraw = 0.0;
    uint64_t seq = 1;
    std::string gateway = "dztd_ctp";
};

/// 打开库、执行一段绑定好的 INSERT，然后收尾（两个写行函数共用）。
/// @return 空串 = 成功；否则为可直接放进断言消息的错误描述。
[[nodiscard]] inline std::string finish_insert(sqlite3* db, sqlite3_stmt* stmt, const char* what) {
    std::string error;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        error = std::string(what) + " failed: " + sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return error;
}

/// 往 `trade_db_path` 写一行持仓（`ext` 固定 `'{}'`：本仓 TD 读路径用例不消费它）。
/// @return 空串 = 成功；否则为可直接放进断言消息的错误描述。
[[nodiscard]] inline std::string insert_td_position_row(const std::string& trade_db_path,
                                                        const TdPositionRowSeed& seed) {
    // 列清单只在下面这一处（与 `schema_catalog.cpp` 的 positions 声明序一致）。
    constexpr const char* kInsertPositionSql =
        "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction,"
        " volume, frozen_volume, today_volume, yd_volume, price, seq, gateway, ext)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,'{}')";

    sqlite3* db = nullptr;
    if (sqlite3_open(trade_db_path.c_str(), &db) != SQLITE_OK) {
        const std::string error = db != nullptr ? sqlite3_errmsg(db) : "open failed";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return "open trade db failed: " + error;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kInsertPositionSql, -1, &stmt, nullptr) != SQLITE_OK) {
        const std::string error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return "prepare insert positions failed: " + error;
    }
    sqlite3_bind_text(stmt, 1, seed.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, seed.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, seed.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, seed.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, seed.direction);
    sqlite3_bind_int(stmt, 6, seed.volume);
    sqlite3_bind_int(stmt, 7, seed.frozen_volume);
    sqlite3_bind_int(stmt, 8, seed.today_volume);
    sqlite3_bind_int(stmt, 9, seed.yd_volume);
    sqlite3_bind_double(stmt, 10, seed.price);
    sqlite3_bind_int64(stmt, 11, static_cast<int64_t>(seed.seq));
    sqlite3_bind_text(stmt, 12, seed.gateway.c_str(), -1, SQLITE_TRANSIENT);
    return finish_insert(db, stmt, "insert positions");
}

/// 往 `trade_db_path` 写一行资金（同理，唯一列清单）。
[[nodiscard]] inline std::string insert_td_trading_account_row(
    const std::string& trade_db_path, const TdTradingAccountRowSeed& seed) {
    constexpr const char* kInsertAccountSql =
        "INSERT INTO trading_accounts (account_id, date, balance, available, frozen, commission,"
        " margin, withdraw_quota, deposit, withdraw, seq, gateway, ext)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,'{}')";

    sqlite3* db = nullptr;
    if (sqlite3_open(trade_db_path.c_str(), &db) != SQLITE_OK) {
        const std::string error = db != nullptr ? sqlite3_errmsg(db) : "open failed";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return "open trade db failed: " + error;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kInsertAccountSql, -1, &stmt, nullptr) != SQLITE_OK) {
        const std::string error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return "prepare insert trading_accounts failed: " + error;
    }
    sqlite3_bind_text(stmt, 1, seed.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, seed.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, seed.balance);
    sqlite3_bind_double(stmt, 4, seed.available);
    sqlite3_bind_double(stmt, 5, seed.frozen);
    sqlite3_bind_double(stmt, 6, seed.commission);
    sqlite3_bind_double(stmt, 7, seed.margin);
    sqlite3_bind_double(stmt, 8, seed.withdraw_quota);
    sqlite3_bind_double(stmt, 9, seed.deposit);
    sqlite3_bind_double(stmt, 10, seed.withdraw);
    sqlite3_bind_int64(stmt, 11, static_cast<int64_t>(seed.seq));
    sqlite3_bind_text(stmt, 12, seed.gateway.c_str(), -1, SQLITE_TRANSIENT);
    return finish_insert(db, stmt, "insert trading_accounts");
}

}  // namespace dztrader::test

#endif  // DZTRADER_WEBUI_TESTS_TD_POSITION_ROW_WRITER_H_
