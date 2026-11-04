#ifndef DZTRADER_WEBUI_TESTS_TD_ORDER_ROW_WRITER_H_
#define DZTRADER_WEBUI_TESTS_TD_ORDER_ROW_WRITER_H_

/**
 * @file td_order_row_writer.h
 * @brief 测试用「往真 schema 的 trade.db 写一行委托」的**唯一实现**。
 *
 * 为什么必须只有一份（`docs/unit-test-standard.md` §5.4）：`orders` 的 19 列清单是建库
 * 声明的投影（真相源 `libs/tdstore/src/schema_catalog.cpp`）。每个测试文件各抄一份时，
 * schema 加列/改名只会让抄得最晚的那份失败，其余静默继续写旧形状——而测试仍绿。
 * 建库已经统一走 `tdstore::ensure_trade_db`，写行同理收敛到这里。
 *
 * **header-only**：`apps/webui/CMakeLists.txt` 为 `tests/` 下每个测试源文件建一个 target，
 * 没有共享测试库；与 `libs/` 各模块的共享夹具一样用 inline 头。
 */
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace dztrader::test {

/// 一条委托行的构造参数（默认值 = 契约 rest §2.9.3 的常见形态，用例只改关心的字段）。
struct TdOrderRowSeed {
    std::string account_id = "ACC1";  ///< 多数用例写 ACC1；需要别的账户时覆盖
    std::string date = "20261009";    ///< 交易日 TEXT "YYYYMMDD"
    int64_t order_id = 0;
    std::string strategy_id = "stg1";
    std::string instrument_id = "rb2601";
    std::string exchange_id = "SHFE";
    int direction = 1;        ///< DzDirection（1 = Long）
    int position_effect = 1;  ///< DzPositionEffect（1 = Open）
    int price_type = 0;       ///< DzPriceType（0 = Limit）
    int status = 2;           ///< DzOrderStatus（2 = NotTraded）
    double price = 3500.0;
    int volume = 1;
    int volume_traded = 0;
    std::string remark{};  ///< 空 = 无备注
    int time = 32400;      ///< 距午夜秒数
    double avg_price = 0.0;
    uint64_t seq = 1;
    std::string gateway = "dztd_ctp";
};

/// 往 `trade_db_path` 写一行委托（`ext` 固定 `'{}'`：本仓 TD 读路径用例不消费它）。
/// @return 空串 = 成功；否则为可直接放进断言消息的错误描述。
[[nodiscard]] inline std::string insert_td_order_row(const std::string& trade_db_path,
                                                     const TdOrderRowSeed& seed) {
    // 列清单只在下面这一处（与 `schema_catalog.cpp` 的 orders 声明序一致）。
    constexpr const char* kInsertOrderSql =
        "INSERT INTO orders (account_id, date, order_id, strategy_id, instrument_id,"
        " exchange_id, direction, position_effect, price_type, status, price, volume,"
        " volume_traded, remark, time, avg_price, seq, gateway, ext)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'{}')";

    sqlite3* db = nullptr;
    if (sqlite3_open(trade_db_path.c_str(), &db) != SQLITE_OK) {
        const std::string error = db != nullptr ? sqlite3_errmsg(db) : "open failed";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return "open trade db failed: " + error;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kInsertOrderSql, -1, &stmt, nullptr) != SQLITE_OK) {
        const std::string error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return "prepare insert orders failed: " + error;
    }
    sqlite3_bind_text(stmt, 1, seed.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, seed.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, seed.order_id);
    sqlite3_bind_text(stmt, 4, seed.strategy_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, seed.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, seed.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, seed.direction);
    sqlite3_bind_int(stmt, 8, seed.position_effect);
    sqlite3_bind_int(stmt, 9, seed.price_type);
    sqlite3_bind_int(stmt, 10, seed.status);
    sqlite3_bind_double(stmt, 11, seed.price);
    sqlite3_bind_int(stmt, 12, seed.volume);
    sqlite3_bind_int(stmt, 13, seed.volume_traded);
    sqlite3_bind_text(stmt, 14, seed.remark.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 15, seed.time);
    sqlite3_bind_double(stmt, 16, seed.avg_price);
    sqlite3_bind_int64(stmt, 17, static_cast<int64_t>(seed.seq));
    sqlite3_bind_text(stmt, 18, seed.gateway.c_str(), -1, SQLITE_TRANSIENT);

    std::string error;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        error = std::string("insert orders failed: ") + sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return error;
}

}  // namespace dztrader::test

#endif  // DZTRADER_WEBUI_TESTS_TD_ORDER_ROW_WRITER_H_
