#ifndef DZTRADER_WEBUI_TD_DATA_SERVICE_H_
#define DZTRADER_WEBUI_TD_DATA_SERVICE_H_

#include "frame_router.h"

#include <dztrader/td_ingest.h>
#include <dztrader/struct.h>
#include <spdlog/spdlog.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader::webui {

/// TD 数据 ingest 领域服务：消费 2000-2003 交易帧（经账户级 seq 水位过滤）
/// 维护内存镜像，消费 2018 ACCOUNT_STATUS 作重置/重建触发（spec §5.5）。
///
/// 语义（spec §5.1/§4.2/§5.5，契约 td-data-sync / account-status）：
///   - 2000-2003（二进制 struct payload，register_raw）解析 seq/account_id 后经
///     TdIngestGate 过滤（先 detect_reset 后 admit，对齐 SDK ingest 次序），
///     kApply 帧按绝对态更新内存镜像。
///   - 2018 Ready → rebuild()（清镜像 + 只读打开 td 库四表重建 + 设新 W）
///   - 2018 Offline → 清空该账户镜像（spec §5.5"清空必须显式"，防幽灵持仓残留）
///
/// **线程语义（register_raw）**：handler 在监听线程同步执行（FrameView 有效期内），
/// 解析并拷贝字段后投递 IO 线程（FrameRouter::Poster），与 REST/WS 连接回调同线程串行，
/// 镜像访问天然无竞争。测试注入同步 poster 等价执行。
class TdDataService {
public:
    /// @param router FrameRouter 引用，ctor 内 register_raw 注册 2000-2003 + 2018
    /// @param td_db_path 回调返回 td 库绝对路径（supervisor registry 提供 td 进程名 →
    ///   $DZTRADER_HOME/flow/<td进程名>/<td进程名>.db），rebuild() 只读打开
    TdDataService(FrameRouter& router, std::function<std::string()> td_db_path);

    // 含引用/不可拷贝成员（router_），禁拷贝/移动
    TdDataService(const TdDataService&) = delete;
    TdDataService& operator=(const TdDataService&) = delete;
    TdDataService(TdDataService&&) = delete;
    TdDataService& operator=(TdDataService&&) = delete;

    /// 设账户水位 W（测试/启动装载用；生产路径由 rebuild() 从 DB 查得设入）。
    void set_watermark(const std::string& account_id, uint64_t w);

    /// 重建该账户镜像：只清触发账户镜像 + 只读打开 td 库四表按账户查询重建 + 设该账户新 W。
    /// 2018 Ready(该账户) 触发；不动其他账户镜像/水位（多账户下其他账户在途数据不丢）。
    /// 失败（库不可用/路径空）降级为清空该账户镜像 + 该账户 W=0 全放行（不幽灵抑制）。
    /// @param reset_gate true (2018 Ready): 完整重置 (gate reset_account, 新基准);
    ///                   false (断档 gap 补齐, 终检发现 E): 只刷新镜像 + 重设 W,
    ///                   保留已推进的 last_applied — 否则后续帧 re-gap 循环重建。
    void rebuild(const std::string& account_id, bool reset_gate = true);

    /// 只读访问器（WS 暴露留给后续设计，spec §10 前端不在本设计内）
    const std::vector<DzPositionInfo>& positions() const { return positions_; }
    const std::vector<DzTradingAccount>& trading_accounts() const { return trading_accounts_; }
    const std::vector<DzOrderReport>& orders() const { return orders_; }
    const std::vector<DzTradeReport>& trades() const { return trades_; }

private:
    /// 注册各帧 handler（ctor 调用）。2000-2003 走 register_raw（struct payload）。
    void register_handlers(FrameRouter& router);

    /// 单账户复位：清该账户镜像（spec §5.5"清空必须显式"）+ gate reset_account。
    /// @param reset_gate false (断档 gap 补齐) 时只清镜像不重置 gate (保留 last_applied)。
    void clear_account(const std::string& account_id, bool reset_gate = true);

    /// 单帧 ingest 过滤：先 detect_reset（倒退→重建重设 W 后重新 admit）后 admit。
    /// kSkip 返回 false；kApply 由调用方按类型更新镜像。
    template <typename ReportT>
    bool ingest(const ReportT& report);

    /// 各帧 handler（register_raw 注册，监听线程解析后更新镜像）。
    void on_order_report(const DzOrderReport& rpt);
    void on_trade_report(const DzTradeReport& rpt);
    void on_position_info(const DzPositionInfo& pos);
    void on_trading_account(const DzTradingAccount& acct);
    void on_account_status(const DzAccountStatus& st);

    /// positions 镜像键: account_id + '\x1f' + instrument_id + '\x1f' + direction。
    static std::string position_key(const DzPositionInfo& p);

    /// DzDate (距纪元天数) -> "YYYYMMDD" 文本 (gate 去重段键 day 分量; 非法回落空串)。
    static const char* trading_day_of(int32_t date);

    FrameRouter& router_;
    std::function<std::string()> td_db_path_;
    TdIngestGate gate_;

    std::vector<DzPositionInfo> positions_;
    std::vector<DzTradingAccount> trading_accounts_;
    std::vector<DzOrderReport> orders_;
    std::vector<DzTradeReport> trades_;
};

/// 单帧 ingest 过滤（定义于头文件，模板）：先 detect_reset 后 admit，对齐 SDK ingest 次序。
/// 倒退（seq < last_applied）→ 重建重设 W 后重新 admit 本帧（spec §5.5）；
/// 返回 false 表示被 W 过滤/已应用防重拦截，调用方不更新镜像。
template <typename ReportT>
bool TdDataService::ingest(const ReportT& report) {
    const std::string account(report.account_id);
    if (gate_.detect_reset(account, report.seq)) {
        // 数据被重置: 重建该账户 (清镜像 + 重查 DB 新水位; 库不可用回落 0 = 全放行)
        rebuild(account, /*reset_gate=*/true);
    }
    if (gate_.admit(account, report.seq) == TdIngestGate::Verdict::kSkip) {
        return false;
    }
    // 终检发现 E【Important】: dzweb 无回补腿 — gap 武装后 (首帧 seq > W+1, DB 快照
    // 外缺条) 无人消费 take_pending_gap, orders/trades 缺条静默直到下次 2018 rebuild。
    // admit 已推进 last_applied 后检查待处理 gap: 非空 → 触发 rebuild(account) —
    // 等效用 DB 快照补齐缺条 (绝对态/追加流一并覆盖, 语义正确且实现极简)。
    // 注意: 此处 rebuild 只刷新镜像 + 重设 W (reset_gate=false), 保留已推进的
    // last_applied — 否则后续帧 seq > 新 W 首帧又暴露 gap → 循环重建。
    // 触发频率低 (仅 gap 窗口), 可接受。
    if (gate_.take_pending_gap().has_value()) {
        rebuild(account, /*reset_gate=*/false);
    }
    return true;
}

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_DATA_SERVICE_H_
