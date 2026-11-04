#ifndef DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_

/**
 * @file td_domain_service.h
 * @brief TD 数据域服务（订单部分）：在线账户集合 + 日切感知 + WS 交易事件转发 + 建连补发环。
 *
 * 依 ADR 0043 / 契约 td-data-sync §8：dzweb **不保留**委托/成交/持仓/资金记录镜像，也不维护
 * `seq` 水位。本服务只保留允许的三样状态：
 * - 在线（`Ready`）账户集合与各账户会话交易日（`ACCOUNT_STATUS`）；
 * - 每账户最近 300 条交易事件的建连补发环（`td_recent`，契约 webui-ws §2.5）；
 * - 合约 `price_tick` 的进程内缓存（派生 `price_decimals` 用，契约 rest §2.9.6）。
 *
 * 委托行形状的唯一真相源是契约 rest §2.9.3，序列化复用 `td_orders_query.h` 的
 * `td_order_row_json`（REST 与 WS 同 schema，禁止两份）。
 *
 * **线程语义（register_raw）**：帧 handler 在监听线程同步执行（`FrameView` 有效期内），
 * 解析并拷贝字段后经 `FrameRouter::poster()` 投递到 IO 线程，与 REST/WS 连接回调串行，
 * 因此内部状态无需加锁。测试注入同步 poster 等价执行。
 */

#include "data_change_notifier.h"
#include "frame_router.h"
#include "ws_broadcaster.h"

#include <dztrader/struct.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader::webui {

/// 每账户建连补发容量（契约 td-data-sync §8 / webui-ws §2.5：每账户最近 300 条）。
inline constexpr size_t kTdRecentCapacityPerAccount = 300;

/// 交易数据基线失效的 `data_changed` scope（契约 webui-ws §2.3）。
inline constexpr const char* kTdDataChangedScope = "td";

class TdDomainService {
public:
    /// @param router 帧路由（ctor 内 register_raw 注册 1002 ORDER_REPORT + 1012 ACCOUNT_STATUS）
    /// @param ws 广播薄接口（WsController 实现）
    /// @param notifier 数据变更通知薄接口（WsController 实现）
    TdDomainService(FrameRouter& router, WsBroadcaster& ws, DataChangeNotifier& notifier);
    ~TdDomainService() = default;

    // 含引用成员，禁拷贝/移动
    TdDomainService(const TdDomainService&) = delete;
    TdDomainService& operator=(const TdDomainService&) = delete;
    TdDomainService(TdDomainService&&) = delete;
    TdDomainService& operator=(TdDomainService&&) = delete;

    /// 建连补发载荷：`{"events":[{"type":"td_order_report","data":<委托行>},...]}`。
    /// 内容 = 各在线账户补发环从旧到新；不同账户之间顺序不承诺。无事件 = 空数组。
    nlohmann::json recent_events() const;

    /// 在线（Ready）账户集合，按 account_id 升序（契约 rest §2.9.1）。
    std::vector<std::string> online_accounts() const;

    /// 当前交易日 "YYYYMMDD"：在线账户 date 最大值；无在线账户 = nullopt（§2.9.1）。
    std::optional<std::string> current_trading_day() const;

    /// 交易库路径提供者（派生精度查 `instruments.price_tick` 用）。未设置 = 库不可用，
    /// 全部走价格小数位兜底（契约 rest §2.9.6）。
    void set_trade_db_path_provider(std::function<std::string()> provider);

    /// 帧 handler 的公开入口（供 register_raw 与单测直接调用；两处走同一实现）。
    void on_account_status(const DzAccountStatus& status);
    void on_order_report(const DzOrderReport& report);

private:
    /// 注册 1002 / 1012（ctor 调用）。
    void register_handlers(FrameRouter& router);

    /// 该合约的价格小数位（契约 rest §2.9.6）：按 instrument_id 缓存 price_tick 查询结果；
    /// 查不到合约 / 库不可用 → 用该行委托价的小数位兜底。
    int price_decimals_for(const std::string& instrument_id, double price);

    /// 该账户补发环追加一条（超过容量丢最旧）。
    void push_recent(const std::string& account_id, nlohmann::json row);

    /// 当前交易日（内部形态）：在线账户 date 最大值；无在线账户 nullopt。
    std::optional<int32_t> current_trading_day_epoch() const;

    /// 帧 handler 投递到 IO 线程（与 REST/WS 连接回调同线程串行）。
    void post(std::function<void()> task);

    FrameRouter& router_;
    WsBroadcaster& ws_;
    DataChangeNotifier& notifier_;
    std::function<std::string()> trade_db_path_;

    /// account_id -> 会话交易日（`DzDate` 距纪元天数；契约 account-status）。
    std::map<std::string, int32_t> online_date_;
    /// account_id -> 最近事件环（从旧到新）。
    std::map<std::string, std::deque<nlohmann::json>> recent_;
    /// instrument_id -> price_tick（nullopt = 该合约查不到，避免重复查库）。
    std::unordered_map<std::string, std::optional<double>> tick_cache_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_
