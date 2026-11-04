#ifndef DZTRADER_WEBUI_TD_ORDERS_QUERY_H_
#define DZTRADER_WEBUI_TD_ORDERS_QUERY_H_

/**
 * @file td_orders_query.h
 * @brief `GET /api/td/orders` 的只读直查实现（契约 rest §2.9.1–§2.9.3/§2.9.6/§2.9.8）。
 *
 * 分层（与 sysmon_history_service 一致）：
 * - **本文件**负责：HTTP 参数解析/校验、每请求独立只读连接、游标分页 SQL、委托行 JSON
 *   与派生字段（price_decimals / avg_price_decimals / sort_key）；
 * - **controller** 只做参数来源适配与响应码选择；
 * - **域服务**复用 `td_order_row_json` 生成 WS `td_order_report` 的同 schema 载荷
 *   （契约 webui-ws §2.5：payload 与 REST 行同形状，唯一真相源是契约 rest §2.9.3）。
 *
 * 降级（§2.9.8）：读库失败一律 200 `{"available":false,"reason":<code>}`，
 * 不抛异常、不返回 5xx、不阻塞 IO 线程。
 *
 * 线程约定：每次调用独立打开只读连接（REST 回调在同一 IO 线程串行），无共享状态。
 */

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dztrader::webui {

/// dzweb 只读端认可的 trade.db schema 版本（SQLite `PRAGMA user_version`，契约 rest §2.9.8）。
///
/// **必须等于** `dztrader::tdstore::kTdSchemaVersion`：dzweb 的生产链接不引入 dztdstore
/// （只是读库，不需要建库/声明门面），因此这条耦合由测试守卫——
/// `td_orders_query_test.cpp` 里 `static_assert(kTdOrdersReadSchemaVersion ==
/// tdstore::kTdSchemaVersion)` 会在 schema 升版而此处忘改时直接编译失败。
inline constexpr int kTdOrdersReadSchemaVersion = 1;

/// 契约 rest §2.9.2：默认 200 行/页、上限 1000 行/页。
inline constexpr int kTdOrdersDefaultLimit = 200;
inline constexpr int kTdOrdersMaxLimit = 1000;

/// 委托行（契约 rest §2.9.3）在后的内部形态：DB 行与 ORDER_REPORT 帧都先归一到这里，
/// 保证 REST 与 WS 用同一份序列化实现（禁止两份行 schema）。
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

/// 委托行 JSON（契约 rest §2.9.3）：枚举为 PascalCase 字符串，`avg_price` 在
/// `volume_traded == 0` 时为 null，`sort_key` 由 `td_order_sort_key` 生成。
nlohmann::json td_order_row_json(const TdOrderRecord& row, int price_decimals);

/// 价格显示小数位（契约 rest §2.9.6）：`price_tick > 0` 时取最小 d ∈ [0,8] 使
/// `price_tick × 10^d` 与最近整数之差 ≤ 1e-9；查不到合约 / `price_tick ≤ 0` 时用
/// `price` 的最短十进制小数位兜底；仍无法确定取 0。
int td_price_decimals(double price_tick, double price);

/// 成交均价显示小数位（契约 rest §2.9.6）：`clamp(price_decimals + 2, 2, 6)`。
int td_avg_price_decimals(int price_decimals);

/// 排序键（契约 rest §2.9.6）：时间锚降序 + order_id 降序的定宽字符串，
/// 字符串降序 = 列表顺序（前端只做字符串比较）。
std::string td_order_sort_key(int32_t time, int64_t order_id);

/// 查合约 `instruments.price_tick`（WS 转发事件推导精度用；库/合约不可用 = nullopt，
/// 调用方走价格小数位兜底，契约 rest §2.9.6）。打开失败不抛、不降级整条事件。
std::optional<double> td_query_price_tick(const std::string& trade_db_path,
                                          int busy_timeout_ms,
                                          const std::string& instrument_id);

/// 委托列表查询（`GET /api/td/orders`）。
class TdOrdersQuery {
public:
    /// 已校验的请求参数（游标分页，契约 rest §2.9.2）。
    struct Params {
        int limit = kTdOrdersDefaultLimit;  ///< 1..1000，默认 200
        std::string cursor;                 ///< 空 = 首页
        std::vector<std::string> accounts;  ///< 已去空白/去空项/去重；空 = 全部在线
    };

    /// @param trade_db_path 回调返回交易库绝对路径（空串 = 库不可用，降级 not_found）
    /// @param busy_timeout_ms 只读连接等锁上限（契约 rest §2.9：200ms）
    explicit TdOrdersQuery(std::function<std::string()> trade_db_path, int busy_timeout_ms = 200);

    /// 解析并校验 HTTP query 参数：`nullopt` = 应回 400 `{"error":"bad request"}`
    /// （limit 越界/非整数、cursor/accounts 无法解码）。
    static std::optional<Params> parse(const std::function<std::string(std::string_view)>& param);

    /// 执行查询并返回响应体。
    /// @param online_accounts 在线（Ready）账户集合，用于 accounts 求交
    /// @param trading_day 当前交易日 "YYYYMMDD"（无在线账户 = nullopt）
    /// @return 正常/降级响应体；`nullopt` = 游标非法或作用域不符（应回 400）
    std::optional<nlohmann::json> execute(const Params& params,
                                          const std::vector<std::string>& online_accounts,
                                          const std::optional<std::string>& trading_day) const;

private:
    std::function<std::string()> trade_db_path_;
    int busy_timeout_ms_ = 200;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ORDERS_QUERY_H_
