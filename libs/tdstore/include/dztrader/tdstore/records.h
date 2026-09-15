/**
 * @file records.h
 * @brief td 库规范记录 (后端无关 POD; 不含 SQL/后端痕迹)
 */
#ifndef DZTRADER_TDSTORE_RECORDS_H_
#define DZTRADER_TDSTORE_RECORDS_H_

#include <cstdint>
#include <string>

namespace dztrader::tdstore {

/// 合约信息 (instruments 表; 哨兵语义见帧契约 instrument.md)
struct InstrumentRecord {
    std::string instrument_id;           ///< 平台唯一键 (PK)
    std::string exchange_id;             ///< 平台注册表值
    std::string symbol;                  ///< 场所原生码
    std::string name;                    ///< UTF-8
    int32_t product_class = 0;           ///< DZ_PRODUCT_* (UNKNOWN=0)
    std::string product_code;            ///< 品种代码 (CTP ProductID)
    int32_t settle_cycle = -1;           ///< -1=不适用, 0=T+0, 1=T+1, 2=T+2
    std::string currency;                ///< 计价/结算货币 (空=账户本币)
    std::string base_asset;              ///< 基础资产 (空=不适用)
    int32_t is_inverse = 0;              ///< 0=线性, 1=反向
    double volume_multiple = 0;          ///< 价值乘数 (>0)
    double volume_step = 1;              ///< 1 平台单位的原生量 (<=0 视为 1)
    double price_tick = 0;               ///< 最小变动价位 (>0)
    int64_t min_limit_order_volume = 0;  ///< 限价单最小量 (<=0 未提供)
    int64_t max_limit_order_volume = 0;  ///< 限价单最大量 (<=0 无限制)
    int64_t min_market_order_volume = 0; ///< 市价单最小量 (<=0 未提供)
    int64_t max_market_order_volume = 0; ///< 市价单最大量 (<=0 无限制)
    int32_t listed_date = 0;             ///< 上市日 (DzDate; 0=NA)
    int32_t delisted_date = 0;           ///< 退市/到期日 (DzDate; 0=NA)
    int32_t option_type = 0;             ///< DZ_OPTION_*; 0=非期权
    double option_strike = 0;            ///< 行权价
    std::string underlying_id;           ///< 标的 instrument_id
    double underlying_multiple = 0;      ///< 合约基础商品乘数 (<=0=NA)
    std::string update_day;              ///< "YYYYMMDD"
    int64_t updated_at = 0;              ///< epoch ms (每次 upsert 推进)
};

}  // namespace dztrader::tdstore

#endif  // DZTRADER_TDSTORE_RECORDS_H_
