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
 * 与引擎的可选回调探测同风格：六个回调**各自可选**（未实现视为空操作），B 不继承
 * `StrategyBase`、不接触引擎回调。除**可默认构造**（实例在节点内就地构造、之后不再搬移）
 * 外，还要求至少实现一个 B 回调——否则 `run_bar_strategy<T>()` 会启动一个什么都不做的
 * 进程，而这几乎总是「把策略类型写错了」的笔误，在编译期挡住比运行到线上再发现便宜。
 */
template <typename T>
concept BarStrategy = std::default_initializable<T> &&
                      (manager_internal::HasOnInit<T> || manager_internal::HasOnDestroy<T> ||
                       manager_internal::HasOnBarOpen<T> || manager_internal::HasOnBarUpdate<T> ||
                       manager_internal::HasOnBarClose<T>);

/// @brief 内部接缝（不进公开承诺面）：三个外部数据源的注入点（历史 bar / 小节表 / 元数据）
namespace bar_strategy_internal {

/**
 * @brief 用注入的内部依赖跑策略（内部接缝；生产入口恒用默认归档实现 + 内置退化表 + 18:00）
 *
 * 公开入口 `run_bar_strategy()` 不带依赖参数：按 #161 的约定，外部依赖（历史 bar 源 / 交易
 * 小节表 / 合约元数据）都是**内部接缝**，数据源落地时只改框架内部，不改公开头面。
 *
 * 参数 `dependencies` 聚合三个数据源与回卷时刻（定义见 `bar_strategy/dependencies.h`）：
 *
 * - **`session_table`**（接缝）：交易小节与竞价窗口表，数据源落地后由配置注入；
 * - **`history` / `metadata`**（接缝）：历史 bar 与合约元数据来源，空 = 默认归档实现
 *   （`dz_db_query_bar` / `dz_db_query_instruments`，归档域未实现时前者的默认实现恒返回
 *   Unavailable）；测试注入 fake 后不需要真实归档库或 platform.db 就能验证回放、冷启动降级
 *   与元数据失败路径（Seam B）；
 * - **`rollback_clock_ms`**（测试接缝）：18:00 回卷定时器按 wall clock 触发，真实用例无法
 *   等一天，因此把回卷时刻换成「当前时刻之后若干秒」，让 Seam A 集成测试能在秒级内确定性地
 *   走通定时器路径，并与推进前兜底路径对比 B 观察到的值。
 *
 * 合在一个入口里而不是各开一个函数：它们服务同一类需求（替换框架内的外部数据/固定取值），
 * 分开会让接缝面按参数个数膨胀。
 *
 * @tparam Strategy 用户策略类型
 * @param dependencies 内部依赖聚合（见 `bar_strategy/dependencies.h`）
 * @return 进程退出码
 */
template <BarStrategy Strategy>
std::int32_t run_bar_strategy_with(
    const bar_strategy_internal::FrameworkDependencies& dependencies) {
    BarStrategyManager<Strategy> manager(dependencies);
    return run_strategy(manager);
}

/**
 * @brief 用注入的交易小节表与回卷时刻跑策略（内部接缝的简化形态）
 *
 * 历史 bar 与合约元数据走默认归档实现；只需要替换小节表的测试（#163 的竞价事件序）用本
 * 重载，需要替换历史/元数据来源的测试（#161 的 Seam B）用完整形态。
 *
 * @tparam Strategy 用户策略类型
 * @param session_table     交易小节与竞价窗口表
 * @param rollback_clock_ms 每日回卷时刻（距午夜毫秒）；默认 18:00
 * @return 进程退出码
 */
template <BarStrategy Strategy>
std::int32_t run_bar_strategy_with(const bar::TradingSessionTable& session_table,
                                   std::int32_t rollback_clock_ms = DEFAULT_ROLLBACK_CLOCK_MS) {
    bar_strategy_internal::FrameworkDependencies dependencies;
    dependencies.session_table = session_table;
    dependencies.rollback_clock_ms = rollback_clock_ms;
    return run_bar_strategy_with<Strategy>(dependencies);
}

}  // namespace bar_strategy_internal

/**
 * @brief 运行 bar 策略直至收到停止信号（DZ_FRAME_SHUTDOWN）或框架自主退出
 *
 * 内部构造 `BarStrategyManager<Strategy>`（A）并交给既有 `dztrader::run_strategy`；返回值
 * 即引擎退出码：正常停止 0、启动失败（配置缺失/非法、订阅失败）1、`dz_init` 失败返回其
 * 错误码。
 *
 * @tparam Strategy 用户策略类型（默认构造；6 个回调任意子集，但至少一个）
 * @return 进程退出码
 */
template <BarStrategy Strategy>
std::int32_t run_bar_strategy() {
    return bar_strategy_internal::run_bar_strategy_with<Strategy>(degraded_trading_session_table());
}

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_H_
