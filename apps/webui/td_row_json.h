#ifndef DZTRADER_WEBUI_TD_ROW_JSON_H_
#define DZTRADER_WEBUI_TD_ROW_JSON_H_

/**
 * @file td_row_json.h
 * @brief 交易数据行（委托 / 成交）的内部形态 + 行 JSON + 派生字段
 *        （契约 rest §2.9.3/§2.9.6）的**唯一实现**。
 *
 * REST 行与 WS 交易事件的 payload 是同一份 schema（契约 webui-ws §2.5：payload 即行对象）：
 * 两条路径都先把 DB 行 / 推送帧归一到这里，再走同一个 `td_*_row_json`。禁止在域服务里
 * 另写一份行序列化——两份会随契约字段增减漂移，让 REST 与 WS 的同一记录形状不一致。
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace dztrader::webui {

/// 委托行（契约 rest §2.9.3）在后的内部形态：DB 行与 ORDER_REPORT 帧都先归一到这里。
struct TdOrderRecord {
    std::string account_id;
    std::string date;  ///< 交易日 "YYYYMMDD"
    int64_t order_id = 0;
    std::string strategy_id;
    std::string instrument_id;
    std::string exchange_id;
    int direction = 0;        ///< DzDirection（-1/0/1）
    int position_effect = 0;  ///< DzPositionEffect
    int price_type = 0;       ///< DzPriceType
    int status = 0;           ///< DzOrderStatus
    double price = 0.0;
    int volume = 0;
    int volume_traded = 0;
    std::string remark;
    int time = 0;  ///< 距午夜秒数 0–86399
    double avg_price = 0.0;
    uint64_t seq = 0;
};

/// 成交行（契约 rest §2.9.3）在后的内部形态：DB 行与 TRADE_REPORT 帧都先归一到这里。
struct TdTradeRecord {
    std::string account_id;
    std::string date;  ///< 交易日 "YYYYMMDD"
    std::string trade_id;
    int64_t order_id = 0;
    std::string strategy_id;
    std::string instrument_id;
    std::string exchange_id;
    int direction = 0;        ///< DzDirection（-1/0/1）
    int position_effect = 0;  ///< DzPositionEffect
    double price = 0.0;
    int volume = 0;
    int time = 0;  ///< 距午夜秒数 0–86399
    uint64_t seq = 0;
};

/// 持仓行（契约 rest §2.9.4）在后的内部形态：DB 行与 POSITION_INFO 帧都先归一到这里。
///
/// 与委托/成交的关键差异：**没有 `time` / `sort_key`**——持仓是绝对态，`positions` 表无
/// 时间列，列表顺序由服务端的 `(account_id, instrument_id, direction)` 升序决定
/// （契约 rest §2.9.2）。前端按业务键覆盖合并，不做排序插入。
struct TdPositionRecord {
    std::string account_id;
    std::string date;  ///< 该账户锚点日 "YYYYMMDD"（不是全局当前交易日）
    std::string instrument_id;
    std::string exchange_id;
    int direction = 0;  ///< DzDirection（-1/0/1）
    int volume = 0;     ///< 总持仓（含今仓与昨仓）
    int frozen_volume = 0;
    int today_volume = 0;
    int yd_volume = 0;
    double price = 0.0;  ///< 持仓均价（double 全精度）
    uint64_t seq = 0;
};

/// 委托行 JSON（契约 rest §2.9.3）：枚举为 PascalCase 字符串，`avg_price` 在
/// `volume_traded == 0` 时为 null，`sort_key` 由 `td_order_sort_key` 生成。
nlohmann::json td_order_row_json(const TdOrderRecord& row, int price_decimals);

/// 成交行 JSON（契约 rest §2.9.3）：枚举为 PascalCase 字符串；成交行**没有**
/// `avg_price` / `avg_price_decimals`（契约 §2.9.3 成交行字段表）。
nlohmann::json td_trade_row_json(const TdTradeRecord& row, int price_decimals);

/// 持仓行 JSON（契约 rest §2.9.4）：枚举为 PascalCase 字符串，无 `sort_key`。
/// REST `items` 与 WS `td_position_info` 的 payload 是同一份 schema（契约 webui-ws §2.5）。
nlohmann::json td_position_row_json(const TdPositionRecord& row, int price_decimals);

/// 价格显示小数位（契约 rest §2.9.6）：`price_tick > 0` 时取最小 d ∈ [0,8] 使
/// `price_tick × 10^d` 与最近整数之差 ≤ 1e-9；查不到合约 / `price_tick ≤ 0` 时用
/// `price` 的最短十进制小数位兜底；仍无法确定取 0。
int td_price_decimals(double price_tick, double price);

/// 成交均价显示小数位（契约 rest §2.9.6）：`clamp(price_decimals + 2, 2, 6)`。
int td_avg_price_decimals(int price_decimals);

/// 委托排序键（契约 rest §2.9.6）：时间锚降序 + order_id 降序的定宽字符串，
/// 字符串降序 = 列表顺序（前端只做字符串比较）。格式固定 `"%05d-%020d"`。
std::string td_order_sort_key(int32_t time, int64_t order_id);

/// 成交排序键（契约 rest §2.9.6）：时间锚 + account_id/exchange_id/trade_id 的降序友好编码。
///
/// 编码 = `"%05d-<hex(account)>-<hex(exchange)>-<hex(trade_id)>"`。byte → 2 位 hex，
/// 使 hex 字符序 = 原始字节序，且 hex 字符与 `-` 的字典序不破坏三段之间的比较
/// （hex 字符均 > '-'）。
std::string td_trade_sort_key(int32_t time,
                              const std::string& account_id,
                              const std::string& exchange_id,
                              const std::string& trade_id);

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ROW_JSON_H_
