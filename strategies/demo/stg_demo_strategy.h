/**
 * @file stg_demo_strategy.h
 * @brief stg_demo 常驻示例策略: UI_INPUT 文本命令驱动, 结果经 OUTPUT_UI 文本行输出
 *
 * 进程入口 (stg_demo.cpp) 只调用 dztrader::run_strategy; 本类不解析 argv、
 * 不做终端交互。master 托管启动后, UI 经 UI_INPUT 帧投递文本命令
 * (语法 `<命令> [参数...]`: 空白分词、命令小写、无引号), 策略把结果写成
 * OUTPUT_UI 帧文本行 —— 行首 ASCII 类别标签 + key=value + 中文正文;
 * md dump 为单帧多行 (逐 tick 一行), 其余命令单帧一行。
 *
 * 命令集: help / info / md add|del|list|dump / order / cancel / account / quit。
 * 设计意图: 给外部策略作者一个可照抄的常驻策略骨架 —— on_start 打就绪横幅、
 * on_ui_input 解析命令、各回调把异步回报转成 UI 文本行、should_exit 响应 quit。
 */
#ifndef DZTRADER_STRATEGIES_DEMO_STG_DEMO_STRATEGY_H_
#define DZTRADER_STRATEGIES_DEMO_STG_DEMO_STRATEGY_H_

#include <dztrader/api.h>
#include <dztrader/data_type.h>
#include <dztrader/error.h>
#include <dztrader/strategy_base.h>
#include <dztrader/struct.h>

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace dztrader::demo {

/// stg_demo 常驻示例策略。
///
/// 订阅集合以 tick 缓存的 key 集合表达 (key 存在 = 已订阅): md add 插入空缓存,
/// md del 退订并删除缓存; 未订阅合约的 tick 不缓存 (正常也不会送达)。
class DemoStrategy : public StrategyBase {
public:
    /// 每合约 tick 缓存上限 (环形: 新 tick 挤掉最旧一条)。
    static constexpr std::size_t kTickCacheCapacity = 100;

    void on_start(DzContext* ctx) override;
    void on_stop() override;
    void on_ui_input(std::string_view data) override;
    void on_tick(const DzTick& tick) override;
    void on_order_report(const DzOrderReport& order_report) override;
    void on_trade_report(const DzTradeReport& trade_report) override;
    void on_position_info(const DzPositionInfo& position_info) override;
    void on_trading_account(const DzTradingAccount& account) override;
    void on_account_status(const DzAccountStatus& status) override;
    void on_error(DzErrorCode errcode, std::string_view message) override;

    /// quit 命令置位; 引擎在 on_start 后与每批事件泵后探测, 命中即优雅退出 (exit 0)。
    bool should_exit() const noexcept { return quit_; }

private:
    /// 分词并分发一条 UI_INPUT 文本命令 (非法输入统一输出 error 行, 不抛异常)。
    void handle_command(std::string_view input);
    void cmd_help(std::span<const std::string_view> args);
    void cmd_info(std::span<const std::string_view> args);
    void cmd_md(std::span<const std::string_view> args);
    void cmd_order(std::span<const std::string_view> args);
    void cmd_cancel(std::span<const std::string_view> args);
    void cmd_account(std::span<const std::string_view> args);
    void cmd_quit(std::span<const std::string_view> args);

    /// md add/del 共用: 订阅 (add=true) 或退订 (add=false) 并维护 tick 缓存
    void md_subscribe(std::span<const std::string_view> instruments, bool add);
    /// 无需参数的命令拒绝多余参数; 返回 true 表示已输出 error 行。
    bool emit_error_on_extra_args(std::string_view cmd, std::span<const std::string_view> args);
    /// 统一 error 行: `[error] cmd=... reason=... <detail>` (含空格的 cmd 加双引号)
    void emit_error(std::string_view cmd, std::string_view reason, std::string_view detail = {});
    /// 写一个 OUTPUT_UI 帧 (文本行); ctx_ 未就绪时静默丢弃
    void emit_line(std::string_view line);
    void emit_md_list();
    /// md dump: 单帧多行, 每合约内按 (date,time,subseconds) 时间正序; limit=0 表示全部
    void emit_md_dump(std::size_t limit);

    DzContext* ctx_ = nullptr;
    bool quit_ = false;
    /// 合约 -> 最近 tick 缓存; 透明比较器让 on_tick 的 find 免构造临时 std::string
    std::map<std::string, std::deque<DzTick>, std::less<>> tick_cache_;
};

}  // namespace dztrader::demo

#endif /* DZTRADER_STRATEGIES_DEMO_STG_DEMO_STRATEGY_H_ */
