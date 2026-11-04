#ifndef DZTRADER_CTP_TD_DAY_LOAD_H_
#define DZTRADER_CTP_TD_DAY_LOAD_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SQLiteCpp/Database.h>

#include <dztrader/core/core_struct.h>  // DzOrderReq

#include "td/td_persist_records.h"  // OrderRecord / TradeRecord

namespace dztrader::ctp {

/// 登录日装载结果（只装当前交易日；历史只在库中，ADR 0045 决策 5）。
struct DayLoadData {
    uint64_t start_seq = 0;           ///< 该账户四表 MAX(seq)（跨日累积，装载 seq 基准）
    std::vector<OrderRecord> orders;  ///< 当日订单最新态（重放匹配基准 + order_ref/sys_id 映射）
    std::vector<TradeRecord> trades;  ///< 当日成交（去重键基准；均价重建归 #248）
};

/// 该账户四表 orders/trades/positions/trading_accounts 的 MAX(seq)；空账户 0。
uint64_t query_max_seq(SQLite::Database& db, const std::string& account_id);

/// DzDate（距纪元天数）-> "YYYYMMDD" 文本（登录日装载的 date 列过滤值 / 拒单行 date 列）。
/// 0 或非法值回落 "00000000"。
std::string format_day_text(int32_t day);

/// 装载该账户当前交易日的订单与成交（date = day，TEXT "YYYYMMDD"）。行序 = seq 升序。
/// db 为独立只读连接（busy_timeout 5000）。查询失败抛 SQLite::Exception（调用方按登录失败处理）。
DayLoadData load_trading_day(SQLite::Database& db,
                             const std::string& account_id,
                             const std::string& day);

/// 无会话账户的账户级 seq 分配器（#13）：主循环线程独占，按账户从 DB MAX(seq) 播种。
/// 已分配值只增不减（不因 DB 未提交回退），且每次分配取 max(已分配值, DB MAX(seq))：
/// 同账户会话推进过的已提交 seq 会被吸收，避免会话断线后拒单撞号。
/// 供 AccountSession 登录日装载时抬高 seq 计数器起点。
class AccountSeqAllocator {
public:
    /// 只读连接提供器（返回 nullptr = 库不可用 → 播种失败，从 1 开始并记 WARN）。
    void set_db_provider(std::function<SQLite::Database*()> provider);

    /// 分配下一个 seq（基准 = max(该账户已分配值, DB MAX(seq))）。
    uint64_t next(const std::string& account_id);

    /// 该账户已分配到的最大值（未用过 0）。
    uint64_t current(const std::string& account_id) const;

private:
    std::function<SQLite::Database*()> db_provider_;
    std::unordered_map<std::string, uint64_t> allocated_;
};

/// 无会话拒单记录：date = 本地自然日（无会话交易日可用——已知限制，写入组件文档），
/// time = 当前本地秒（0..86399），seq 由调用方分配。
OrderRecord build_no_session_reject(const DzOrderReq& req,
                                    int32_t date,
                                    int32_t time,
                                    std::string_view reason);

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_DAY_LOAD_H_
