/**
 * @file main.cpp
 * @brief dzsysmon 进程入口（薄壳）：进程主体在 `app.cpp`。
 *
 * 拆成两层的理由：进程级 e2e 需要一个「同一份进程主体、只替换 NTP 传输」的可执行文件
 * （`tests/clock_e2e_runner.cpp` 注入假传输），因此 `run` 必须对测试可链接；生产入口只保留
 * `int main`，避免测试专用可执行文件与生产可执行文件各带一份 `main` 符号。
 */

#include "app.h"

int main(int argc, char* argv[]) { return dztrader::sysmon::run(argc, argv); }
