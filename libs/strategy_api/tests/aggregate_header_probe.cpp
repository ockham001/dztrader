/**
 * @file aggregate_header_probe.cpp
 * @brief 聚合头 `<dztrader.h>` 不得引入内置 JSON 单头（issue #154 验收项）
 *
 * 聚合头是全部策略消费者的默认入口（含纯 C 与轻量 C++ 消费者）。内置 nlohmann 单头
 * 两万余行、单个 TU 约 4s 编译成本，不能让它成为「只用 C 接口」的消费者无条件代价。
 * 本探针在包含聚合头后检查 nlohmann 的版本宏**未**被定义；一旦有人把
 * `dztrader/json.h`（或内置单头路径）加进聚合头，本 TU 立刻编译失败。
 *
 * 只链接 `strategy_api_headers`，只证明头文件包含面，不涉及任何运行时。
 */
#include <dztrader.h>

#if defined(NLOHMANN_JSON_VERSION_MAJOR) || defined(NLOHMANN_JSON_VERSION_MINOR) || \
    defined(NLOHMANN_JSON_VERSION_PATCH)
#error "dztrader.h must not include the vendored JSON single header (see issue #154)"
#endif

int main() { return 0; }
