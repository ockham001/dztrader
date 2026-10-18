/**
 * @file stg_demo.cpp
 * @brief stg_demo 进程入口 — 常驻示例策略, 只调用 run_strategy
 *
 * 由 master 按 strategy[] 配置托管启动 (exe 名即策略 ID); 不解析命令行参数、
 * 不做终端交互。全部交互经帧通道: UI_INPUT 文本命令进, OUTPUT_UI 文本行出;
 * STDOUT 仅保留 SDK 引擎自检与诊断行 (由 master 捕获进日志)。
 *
 * 运行前提: master 已启动且注入 DZTRADER_MD_SOURCE (绑定行情源 ready 后策略才启动),
 * 策略名 = 可执行文件名 stg_demo。
 */
#include <dztrader/strategy_engine.h>
#include "stg_demo_strategy.h"

int main() {
    dztrader::demo::DemoStrategy strategy;
    return dztrader::run_strategy(strategy);
}
