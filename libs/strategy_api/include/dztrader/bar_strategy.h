/**
 * @file bar_strategy.h
 * @brief BarStrategy 框架公开入口：`run_bar_strategy<Strategy>()`（ADR 0027，组件文档 §2）
 *
 * 用户策略（B）只写策略逻辑；框架管理器 A（`BarStrategyManager<Strategy>`）负责配置、
 * 订阅、tick→bar 聚合、窗口推进与回调分发。对引擎而言 A 是唯一策略对象，本入口内部以既有
 * `run_strategy` 驱动，因此不修改 `StrategyBase` / 引擎头 / C API。
 *
 * 最小用法：
 * @code
 *   #include <dztrader/bar_strategy.h>
 *
 *   struct MyStrategy {
 *       void on_init(dztrader::InstrumentContext& ctx) {}
 *       void on_bar_open(dztrader::InstrumentContext& ctx) {}
 *       void on_bar_update(dztrader::InstrumentContext& ctx) {
 *           const double price = ctx.close[0];
 *           (void)price;
 *       }
 *       void on_bar_close(dztrader::InstrumentContext& ctx) {}
 *       void on_destroy(dztrader::InstrumentContext& ctx) {}
 *   };
 *
 *   int main() { return dztrader::run_bar_strategy<MyStrategy>(); }
 * @endcode
 *
 * 配置文件：`$dz_strategy_home()/<strategy_id>.json`（`dz_strategy_home()` = 可执行文件
 * 目录、`strategy_id` = exe stem）。缺失时框架生成最小默认配置、打印提示并以非 0 退出，
 * 等作者编辑后重启；非法配置同样打印错误并非 0 退出，不静默继续（组件文档 §7）。
 *
 * 本头是 SDK 公开头：只依赖标准库与 SDK 公开头（含 `dztrader/json.h` 的 JSON 取用规则，
 * ADR 0026），不依赖 spdlog 或任何内部库（ADR 0020/0021）。**不要**把它加进聚合头
 * `<dztrader.h>`：配置层会引入内置 JSON 单头，#154 已锁定聚合头不得包含它。
 */
#ifndef DZTRADER_BAR_STRATEGY_H_
#define DZTRADER_BAR_STRATEGY_H_

#include <dztrader/bar/types.h>
#include <dztrader/bar_strategy/context.h>
#include <dztrader/bar_strategy/manager.h>
#include <dztrader/strategy_engine.h>

#include <concepts>
#include <cstdint>

namespace dztrader {

/**
 * @brief BarStrategy 用户策略（B）的编译期约束
 *
 * 六个回调**全部可选**（缺省视为空操作，与引擎的可选回调探测同风格）；B 不继承
 * `StrategyBase`、不接触引擎回调。约束只要求**可默认构造**：实例在节点内就地构造，构造后
 * 不再搬移。
 */
template <typename T>
concept BarStrategy = std::default_initializable<T>;

/**
 * @brief `run_bar_strategy` 的可选注入（#161 的接缝：交易小节表数据源）
 *
 * v1 默认用内置退化表（组件文档 §5.5）；platform.db 数据源落地后由调用方传入真实表，
 * 框架代码不改。显式传入表时 `degraded` 应置 false，启动诊断随之改变口径。
 */
struct BarStrategyRunOptions {
    /// @brief 交易小节与竞价窗口表（省略 = 内置退化表单小节 `[0, 86399]`、无竞价窗口）
    bar::TradingSessionTable session_table = degraded_trading_session_table();

    /// @brief 是否使用内置退化表（true = 启动诊断声明退化口径）
    bool degraded_session_table = true;
};

/**
 * @brief 运行 bar 策略直至收到停止信号（DZ_FRAME_SHUTDOWN）或框架自主退出
 *
 * 内部构造 `BarStrategyManager<Strategy>`（A）并交给既有 `dztrader::run_strategy`；返回值
 * 即引擎退出码：正常停止 0、启动失败（配置缺失/非法、订阅失败）1、`dz_init` 失败返回其
 * 错误码。
 *
 * @tparam Strategy 用户策略类型（默认构造；6 个回调任意子集）
 * @param options   可选注入（交易小节表；省略 = 内置退化表）
 * @return 进程退出码
 */
template <BarStrategy Strategy>
std::int32_t run_bar_strategy(const BarStrategyRunOptions& options = {}) {
    BarStrategyManager<Strategy> manager(options.session_table, options.degraded_session_table);
    return run_strategy(manager);
}

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_H_
