#ifndef DZTRADER_CTP_TESTS_TD_CONFIG_FIXTURE_H_
#define DZTRADER_CTP_TESTS_TD_CONFIG_FIXTURE_H_

/**
 * @file td_config_fixture.h
 * @brief 测试用「构造 dztd_ctp.json 内容」的**唯一实现**。
 *
 * 为什么必须只有一份（`docs/unit-test-standard.md` §5.4）：`dztd_ctp.json` 的形态真相源是
 * `TdConfig` 的 `to_json`（`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT` 宏，见
 * `td/td_config.h`），`{"td": ...}` 只是文件外层 section。原来 `td_config_test.cpp` 与
 * `apps/master/tests/smoke/smoke_test.cpp` 各自手写一份同形 JSON：#251 把连接参数移入账户
 * `ext` 后只改了前者的形状，smoke 仍写旧形态——build 与 preflight 全绿，直到 smoke 运行时
 * `dztd_ctp` 启动即 CRITICAL 退出、等不到 Running 才超时暴露（#281 复盘）。
 *
 * 收益：`make_td_config_json` 先构造 `TdConfig` 实例，再交给宏生成的 `to_json` 产出 JSON
 * ——**不手写任何 JSON 键字面量**。因此 `TdConfig` 的字段增删/改名/类型变更会让本函数
 * 编译失败，**在构造夹具的测试 target 上就红**（如 `dztd_ctp_td_config_test`，编译期即
 * 暴露），而不是等到 smoke 运行时超时。
 *
 * **header-only**：`apps/ctp/CMakeLists.txt` 为 `tests/` 下每个测试源文件建一个 target，
 * 没有共享测试库；与 `apps/webui/tests/td_order_row_writer.h` 用 inline 头同型。
 */
#include "td/td_config.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace dztrader::test {

/// 构造 `dztd_ctp.json` 内容所需的全部可变量，默认值 = 常态形态（用例只改关心的字段）。
/// 分层与 `TdConfig` 一一对应：进程层字段 + 账户通用层（契约 td-config §2.2）+
/// 账户 `ext`（契约 td-config §2.3）。
struct TdConfigSeed {
    // --- 进程层 ---
    int qry_account_interval_s = 5;    ///< 资金查询间隔（秒）
    int qry_position_interval_s = 60;  ///< 持仓查询间隔（秒）
    int qry_flush_interval_ms = 1500;  ///< 流控队列间隔（毫秒）
    bool enable_lock_mode = true;      ///< 进程级 `ext`：开平仓转换 LOCK 模式

    bool with_account = true;  ///< false = `accounts` 为空数组（无账户形态）

    // --- 账户通用层（契约 td-config §2.2）---
    std::string account_id = "account_001";
    bool enabled = true;
    bool risk_control_enabled = false;
    std::string currency_id = "CNY";

    // --- 账户 `ext`（契约 td-config §2.3）---
    std::string broker_id = "9999";
    std::string user_id = "00001";
    std::string password = "123456";
    std::string product_info{};
    std::string auth_code{};
    std::string app_id{};
    std::string flow_dir{};  ///< 空 = 由网关按 DZTRADER_HOME 派生

    std::string frontend_address{};  ///< 空 = 不配置前置（`frontends` 为空数组）
    std::string frontend_label{};
    bool frontend_enabled = true;
};

/// 生成 `dztd_ctp.json` 的完整 JSON 内容（`{"td": <TdConfig>}`）。
/// 形态真相源是 `TdConfig` 的 `to_json`，本函数只负责把 seed 映射到配置对象。
/// @param seed 可变量；默认值即常态形态
/// @return 可直接 `dump()` 写文件的完整 JSON 文档
[[nodiscard]] inline nlohmann::json make_td_config_json(const TdConfigSeed& seed = {}) {
    ctp::TdConfig config;
    config.qry_account_interval_s = seed.qry_account_interval_s;
    config.qry_position_interval_s = seed.qry_position_interval_s;
    config.qry_flush_interval_ms = seed.qry_flush_interval_ms;
    config.ext.enable_lock_mode = seed.enable_lock_mode;

    if (seed.with_account) {
        ctp::AccountConfig account;
        account.account_id = seed.account_id;
        account.enabled = seed.enabled;
        account.risk_control_enabled = seed.risk_control_enabled;
        account.currency_id = seed.currency_id;
        account.ext.broker_id = seed.broker_id;
        account.ext.user_id = seed.user_id;
        account.ext.password = seed.password;
        account.ext.product_info = seed.product_info;
        account.ext.auth_code = seed.auth_code;
        account.ext.app_id = seed.app_id;
        account.ext.flow_dir = seed.flow_dir;
        if (!seed.frontend_address.empty()) {
            account.ext.frontends.push_back(
                {seed.frontend_address, seed.frontend_label, seed.frontend_enabled});
        }
        config.accounts.push_back(std::move(account));
    }

    return nlohmann::json{{"td", config}};
}

}  // namespace dztrader::test

#endif  // DZTRADER_CTP_TESTS_TD_CONFIG_FIXTURE_H_
