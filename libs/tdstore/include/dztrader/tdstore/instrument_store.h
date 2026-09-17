/**
 * @file instrument_store.h
 * @brief instruments 表 store ops (记录 upsert / 投影查询 / symbol 定向解析)
 */
#ifndef DZTRADER_TDSTORE_INSTRUMENT_STORE_H_
#define DZTRADER_TDSTORE_INSTRUMENT_STORE_H_

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/db/database.h>
#include <dztrader/tdstore/records.h>

namespace dztrader::tdstore {

/// 承诺字段白名单 (schema 序; query 默认返回全部)
const std::vector<std::string>& instrument_columns();

/// upsert 单条合约记录 (INSERT OR REPLACE, PK=instrument_id; updated_at 由调用方填)
void upsert_instrument(dztrader::db::Database& db, const InstrumentRecord& record);

/// 预编译复用的合约 upsert 器: 构造时 prepare 一次, 之后 upsert 仅 bind/execute/reset
/// (批量写路径复用, 避免逐行 prepare 开销; 自由函数 upsert_instrument 为一次性薄包装).
/// 线程约束: 非线程安全, 与所绑连接同线程独占使用 (PersistWriter 中仅 writer 线程触碰).
class InstrumentUpserter {
public:
    explicit InstrumentUpserter(dztrader::db::Database& db);

    /// 复用预编译语句写入单条记录 (覆盖语义同 upsert_instrument).
    void upsert(const InstrumentRecord& record);

private:
    std::unique_ptr<dztrader::db::Statement> stmt_;
};

/// 查询合约: fields 为空 = 全部承诺列; 非法/重复列名抛 Exception(DZ_EC_INVALID_PARAM);
/// 行序 instrument_id 升序; instrument_id 空 = 全量。列元信息按请求声明填充 (0 行仍正确)。
dztrader::db::QueryResult query_instruments(dztrader::db::Database& db,
                                            std::string_view instrument_id,
                                            std::span<const std::string> fields);

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_INSTRUMENT_STORE_H_
