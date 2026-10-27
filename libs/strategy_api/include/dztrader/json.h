/**
 * @file json.h
 * @brief dztrader 策略接口 — JSON 后端的唯一入口（内置单头 / 外部接入两种模式）
 *
 * 策略 SDK 的 JSON 消费者（如 BarStrategy 框架的配置层）**只经本头**取得 JSON，
 * 不直接包含任何第三方路径：把「JSON 从哪来」收敛为一个头、两种模式（ADR 0026）。
 *
 * 两种模式：
 *   - 默认（未定义 DZTRADER_JSON_EXTERNAL）：使用随 SDK 头文件目录分发的内置单头
 *     `dztrader/third_party/nlohmann/json.hpp`。策略作者把 SDK 的 include 目录加进
 *     编译路径即可解析 JSON，不需要 Conan、也不需要额外安装。
 *   - 定义 DZTRADER_JSON_EXTERNAL：改为包含 `<nlohmann/json.hpp>`，使用策略作者
 *     自己提供（Conan / vendored / 系统包）的版本与来源。
 *
 * 版本与升级纪律：内置单头版本与 `conanfile.py` 固定的 `nlohmann_json` 版本一致，
 * 权威值、来源与整文件替换步骤见同目录 `VENDORED.md`。升级必须**同一次提交**改两边
 * （`conanfile.py` + 内置单头），由 `scripts/check_json_vendor_version.py` 在
 * preflight / CI 中机器校验，改漏一边即红。
 *
 * 模式一致性：DZTRADER_JSON_EXTERNAL 是编译期开关，必须在同一进程内所有包含本头的
 * 翻译单元上取值一致。两种模式可能解析到不同版本的 nlohmann（ABI inline namespace
 * 不同），混用会让同名 `nlohmann::json` 在不同 TU 中成为不同类型（ODR 违规，症状
 * 在链接期或运行期才暴露）。外部模式须选用满足框架所用 API 子集的版本，版本兼容
 * 说明进策略开发者指南。
 *
 * 本头**不**被聚合头 `<dztrader.h>` 包含：纯 C 与轻量 C++ 消费者不应为内置单头承担
 * 编译成本（issue #154 验收项）。需要 JSON 能力的头显式包含本头。
 */
#ifndef DZTRADER_JSON_H_
#define DZTRADER_JSON_H_

#ifdef DZTRADER_JSON_EXTERNAL
#include <nlohmann/json.hpp>
#else
#include "third_party/nlohmann/json.hpp"
#endif

#endif /* DZTRADER_JSON_H_ */
