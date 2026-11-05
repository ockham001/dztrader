#ifndef DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_

/**
 * @file td_domain_service.h
 * @brief TD 数据域服务：在线账户集合 + 日切感知 + WS 交易事件转发 + 建连补发环 +
 *        统计计数器（契约 rest §2.9.5）。
 *
 * 依 ADR 0043 / 契约 td-data-sync §8：dzweb **不保留**委托/成交/持仓/资金记录镜像，也不做
 * 记录 ingest 的 `seq` 水位过滤/断档回补（那是策略 SDK 的唯一职责）。本服务保留的只有允许
 * 的状态：
 * - 在线（`Ready`）账户集合与各账户会话交易日（`ACCOUNT_STATUS`）；
 * - 每账户最近 300 条交易事件的建连补发环（`td_recent`，契约 webui-ws §2.5）；
 * - 合约 `price_tick` 的进程内缓存（派生 `price_decimals` 用，契约 rest §2.9.6）；
 * - 统计计数器（4 值 + 活动委托表 + 每账户重算水位，契约 rest §2.9.5）。这里的「水位」是
 *   统计计数器的**重放守卫**（跳过重算已纳入的帧），不是记录 ingest 水位——它只由重算
 *   从 DB 写入，增量永不推进，见 `td_stats.h`。
 *
 * 委托行与成交行形状的唯一真相源是契约 rest §2.9.3，序列化复用 `td_row_json.h` 的
 * `td_order_row_json` / `td_trade_row_json`（REST 与 WS 同 schema，禁止两份）。
 *
 * **线程语义（register_raw）**：帧 handler 在监听线程同步执行（`FrameView` 有效期内），
 * 解析并拷贝字段后经 `FrameRouter::poster()` 投递到 IO 线程，与 REST/WS 连接回调串行，
 * 因此内部状态（含统计计数器）无需加锁。测试注入同步 poster 等价执行。
 */

#include "data_change_notifier.h"
#include "frame_router.h"
#include "td_stats.h"
#include "td_stats_query.h"
#include "ws_broadcaster.h"

#include <dztrader/struct.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
    /// @param router 帧路由（ctor 内 register_raw 注册 1002 ORDER_REPORT + 1003 TRADE_REPORT
    ///        + 1012 ACCOUNT_STATUS）
    /// @param ws 广播薄接口（WsController 实现）
    /// @param notifier 数据变更通知薄接口（WsController 实现）
    TdDomainService(FrameRouter& router, WsBroadcaster& ws, DataChangeNotifier& notifier);
    ~TdDomainService() = default;

    // 含引用成员，禁拷贝/移动
    TdDomainService(const TdDomainService&) = delete;
    TdDomainService& operator=(const TdDomainService&) = delete;
    TdDomainService(TdDomainService&&) = delete;
    TdDomainService& operator=(TdDomainService&&) = delete;

    /// 建连补发载荷：`{"events":[{"type":"td_order_report"|"td_trade_report","data":<行>},...]}`。
    /// 内容 = 各在线账户补发环从旧到新；同一账户环**同时容纳委托与成交事件**
    /// （契约 td-data-sync §8：每账户最近 300 条交易事件，四类共享容量）。
    /// 不同账户之间顺序不承诺。无事件 = 空数组。
    nlohmann::json recent_events() const;

    /// 在线（Ready）账户集合，按 account_id 升序（契约 rest §2.9.1）。
    std::vector<std::string> online_accounts() const;

    /// 当前交易日 "YYYYMMDD"：在线账户 date 最大值；无在线账户 = nullopt（§2.9.1）。
    std::optional<std::string> current_trading_day() const;

    /// 交易库路径提供者（派生精度查 `instruments.price_tick` 用）。未设置 = 库不可用，
    /// 全部走价格小数位兜底（契约 rest §2.9.6）。
    void set_trade_db_path_provider(std::function<std::string()> provider);

    /// 统计重算查询（main 注入；未注入 = 统计恒 not_ready，不影响委托列表）。
    /// 注入后**立即**重算一次（不放在 ctor：ctor 时查询尚未注入，只会得到 not_ready；
    /// main 的顺序是「构造 → 注入 → 起调度器」）。
    void set_stats_query(std::shared_ptr<TdStatsQuery> query);

    /// 周期对账调度器（可注入：默认不启动；main 注入 drogon IO 循环 runEvery）。
    /// 调度器职责：按 interval 反复调用传入的 task，直到进程退出（无需取消——服务与
    /// IO 循环同生命周期）。测试注入假调度器后手动 tick，不依赖真实定时器。
    void set_reconcile_scheduler(
        std::function<void(double interval, std::function<void()> task)> scheduler);

    /// 立即重算一次（启动 / Ready / 在线集合变化 / 日切 / 周期对账 / 测试直接触发）。
    ///
    /// 未注入 `TdStatsQuery` → `stats_ready_ = false`（不崩、不影响委托列表）；
    /// `recompute` 返回 nullopt（库不可读）→ 保持既有计数器与 ready 状态
    /// （契约 rest §2.9.5：计数器就绪后即使交易库暂时不可读也不降级）。
    void reconcile_stats();

    /// `GET /api/td/stats` 响应体（契约 rest §2.9.5）。
    nlohmann::json stats_body() const;

    /// 帧 handler 的公开入口（供 register_raw 与单测直接调用；两处走同一实现）。
    void on_account_status(const DzAccountStatus& status);
    void on_order_report(const DzOrderReport& report);
    void on_trade_report(const DzTradeReport& report);

private:
    /// 注册 1002 / 1003 / 1012（ctor 调用）。
    void register_handlers(FrameRouter& router);

    /// 该合约的价格小数位（契约 rest §2.9.6）：按 instrument_id 缓存 price_tick 查询结果；
    /// 查不到合约 / 库不可用 → 用该行委托价的小数位兜底。
    int price_decimals_for(const std::string& instrument_id, double price);

    /// 该账户补发环追加一条（超过容量丢最旧）。委托与成交共用同一环与容量
    /// （契约 td-data-sync §8；`type` 区分事件种类）。
    void push_recent(const std::string& account_id, const char* type, nlohmann::json row);

    /// 当前交易日（内部形态）：在线账户 date 最大值；无在线账户 nullopt。
    std::optional<int32_t> current_trading_day_epoch() const;

    /// 帧 handler 投递到 IO 线程（与 REST/WS 连接回调同线程串行）。
    void post(std::function<void()> task);

    FrameRouter& router_;
    WsBroadcaster& ws_;
    DataChangeNotifier& notifier_;
    std::function<std::string()> trade_db_path_;

    /// 统计重算查询（未注入 = 统计恒 not_ready）。
    std::shared_ptr<TdStatsQuery> stats_query_;
    TdStatsCounter stats_;
    bool stats_ready_ = false;

    /// account_id -> 会话交易日（`DzDate` 距纪元天数；契约 account-status）。
    std::map<std::string, int32_t> online_date_;
    /// account_id -> 最近事件环（从旧到新）。
    std::map<std::string, std::deque<nlohmann::json>> recent_;
    /// instrument_id -> price_tick（nullopt = 该合约查不到，避免重复查库）。
    std::unordered_map<std::string, std::optional<double>> tick_cache_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_DOMAIN_SERVICE_H_
