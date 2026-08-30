/**
 * @file strategy_base.h
 * @brief 策略空默认实现 - 各回调签名速查与可选基类
 *
 * 仅实现部分回调的策略可继承它补齐默认空实现; 引擎
 * (dztrader/strategy_engine.h) 按鸭子类型接入, 继承与否不影响任何行为。
 * 注意: 继承本类的策略, 引擎启动自检行的可选回调探测恒为 yes
 * (基类提供了全部空实现), 拼写错误的 override 由编译器 override 检查兜底。
 */
#ifndef DZTRADER_STRATEGY_BASE_H_
#define DZTRADER_STRATEGY_BASE_H_

#include <dztrader/api.h>
#include <dztrader/data_type.h>
#include <dztrader/error.h>
#include <dztrader/struct.h>

#include <cstdint>
#include <string_view>

namespace dztrader {

/// 用户输入回调视图 (DZ_FRAME_STG_USER_INPUT, 变长 ext_inst 帧的解析结果)。
/// 非共享内存布局结构: data 指向 SHM 帧 payload, 仅在本次回调返回前有效,
/// 需留存请拷贝; 内容为 UTF-8 文本或 JSON, 无固定 schema (契约 strategy)。
/// instance_id 为目标裸策略名——SDK 已按 instance_id == 本策略名 定向过滤,
/// 回调收到的必为定向本策略的输入。
struct DzUserInput {
    std::string_view instance_id;  ///< 目标策略名 (帧扩展头 instance_id, == 本策略名)
    const char* data;              ///< 变长 payload (UTF-8 文本/JSON), 回调内有效
    uint32_t data_size;            ///< payload 字节数 (不含结尾 0, 不保证 0 结尾)
};

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    virtual void on_start(DzContext* ctx) { (void)ctx; }
    virtual void on_stop() {}
    virtual void on_tick(const DzTick& tick) { (void)tick; }
    virtual void on_trade_report(const DzTradeReport& trade_report) { (void)trade_report; }
    virtual void on_order_report(const DzOrderReport& order_report) { (void)order_report; }
    virtual void on_schedule(const DzScheduleEvent& timer_event) { (void)timer_event; }
    virtual void on_user_input(const DzUserInput& user_input) { (void)user_input; }
    virtual void on_error(DzErrorCode errcode, std::string_view message) {
        (void)errcode;
        (void)message;
    }
};

}  // namespace dztrader

#endif /* DZTRADER_STRATEGY_BASE_H_ */
