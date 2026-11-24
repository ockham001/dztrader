#include "td_account_controller.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/account_identity.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <utility>

#include "td_session_guard.h"

namespace dztrader::webui {

namespace {

/// `DzAccountId` 是 `char[32]`，可用长度 = 31 字节（末字节留给 NUL）。
/// 契约 rest §2.9.7 预检 4 的「≤ 31 字节」即此容量，不用另写魔数。
/// （CTP 侧的 12 字节上限不在 dzweb：分层见 ADR 0056 决策 6。）
constexpr size_t kMaxAccountIdBytes = sizeof(DzAccountId) - 1;

/// 账户名的**有效比较键**（契约 td-config §5/§7 的「展平判定」）：`NFC(trim(name))`；
/// 留空或非法（手改配置文件可能带来）按 `account_id` 参与判定——与展示回落同一口径。
std::string effective_name_key(const TdAccountRegistryEntry& entry) {
    const auto name = dztrader::platform::validate_account_name(entry.account_name);
    return name.unset() ? entry.account_id : name.key;
}

}  // namespace

bool TdAccountCtrl::is_registered_td_gateway(const std::string& gateway) const {
    // 契约 rest §2.9.7 预检 2 / §2.9.10 预检 3：与 td_session_guard 共用同一实现，
    // 避免配置写端点与会话控制端点各写一份「已注册 dztd_*」判定。
    return process_mirror_ && td_gateway_registered(*process_mirror_, gateway);
}

bool TdAccountCtrl::is_gateway_running(const std::string& gateway) const {
    return process_mirror_ && td_gateway_running(*process_mirror_, gateway);
}

drogon::HttpResponsePtr TdAccountCtrl::check_gateway_ready(const std::string& gateway) const {
    if (!is_registered_td_gateway(gateway)) {
        SPDLOG_WARN("td account config rejected: invalid gateway | gateway={}", gateway);
        return error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway");
    }
    if (!is_gateway_running(gateway)) {
        SPDLOG_WARN("td account config rejected: gateway not running | gateway={}", gateway);
        return error_with_code(drogon::k409Conflict, "td gateway not running",
                               "gateway_not_running");
    }
    return nullptr;
}

bool TdAccountCtrl::is_registered_process_name(const std::string& name) const {
    // 契约 td-config §7 / general §5：账户级帧的 `instance_id` 就是 `account_id`，与进程级
    // `instance_id` 共用 MirrorStore 的一维键空间——同名会让 `progress` 域互相覆盖。
    // `dztraderd`（master）不在自己的 process_config 里，但它的实例 key 恒存在于镜像
    // （`kMasterInstance`），因此单独判定；其余进程（`dzweb`/`dzsysmon`/`dzmd_*`/`dztd_*`/
    // 策略）都在进程配置镜像里。
    if (name == kMasterInstance) {
        return true;
    }
    return process_mirror_ && process_mirror_->get_process_config(name).has_value();
}

drogon::HttpResponsePtr TdAccountCtrl::check_account_id(const std::string& account_id) const {
    // 契约 rest §2.9.7 预检 4（规则真相源：td-config §2.2 / §7）。CTP 的 12 字节上限不在
    // 这里——dzweb 不解析 `ext`，超限由网关走四件套报（ADR 0056 决策 6）。
    const bool invalid = account_id.empty() || account_id.size() > kMaxAccountIdBytes ||
                         !dztrader::platform::account_id_charset_valid(account_id) ||
                         is_registered_process_name(account_id);
    if (!invalid) {
        return nullptr;
    }
    SPDLOG_WARN("td account config rejected: invalid account id | account={} bytes={}", account_id,
                account_id.size());
    return error_with_code(drogon::k400BadRequest, "invalid account id", "invalid_account_id");
}

drogon::HttpResponsePtr TdAccountCtrl::check_account_name(const std::string& account_name) const {
    // 契约 rest §2.9.7 预检 4：`trim` 后 ≤64 字节 UTF-8、无控制字符（共享实现见
    // `platform::validate_account_name`，与 td 网关自检同一函数）。空串 = 未设置，合法。
    const auto result = dztrader::platform::validate_account_name(account_name);
    if (result.valid()) {
        return nullptr;
    }
    SPDLOG_WARN("td account config rejected: invalid account name | bytes={}",
                dztrader::platform::trim_account_name(account_name).size());
    return error_with_code(drogon::k400BadRequest, "invalid account name", "invalid_account_name");
}

drogon::HttpResponsePtr TdAccountCtrl::check_account_name_conflict(
    const std::string& account_name,
    const std::string& account_id,
    const std::string& exclude_account_id,
    bool check_candidate_id_against_names) const {
    if (!registry_) {
        return nullptr;  // 未装配注册表（测试/异常装配）：交给目标网关
    }
    // 候选的有效名键：留空按候选 account_id 参与判定（契约 §2.9.7 预检 5）。
    const auto name = dztrader::platform::validate_account_name(account_name);
    const std::string candidate_key = name.unset() ? account_id : name.key;
    for (const auto& entry : registry_->entries()) {
        if (!exclude_account_id.empty() && entry.account_id == exclude_account_id) {
            continue;  // PUT 改名排除自身（契约 §2.9.7 预检 6）
        }
        const std::string existing_key = effective_name_key(entry);
        // 展平判定（契约 td-config §5 / ADR 0056 决策 4 的四组合）：候选名与别人的 id /
        // 名冲突；候选 id 与别人的名字冲突（仅新增时——PUT 的 id 未变，既有冲突与本次
        // 请求无关，不得因此拒绝改名）。
        const bool conflict = candidate_key == entry.account_id || candidate_key == existing_key ||
                              (check_candidate_id_against_names && account_id == existing_key);
        if (conflict) {
            SPDLOG_WARN(
                "td account config rejected: account name conflict | account={} "
                "conflict_with={}",
                account_id, entry.account_id);
            return error_with_code(drogon::k409Conflict, "account name already exists",
                                   "account_name_exists");
        }
    }
    return nullptr;
}

drogon::HttpResponsePtr TdAccountCtrl::dispatch(const std::string& gateway,
                                                const std::string& op,
                                                const nlohmann::json& params) {
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        // 事件通道不可用：请求未被受理（基础设施问题），不是预检失败，故不带 code。
        SPDLOG_ERROR(
            "td account config dispatch failed: shm writer not available | gateway={} op={}",
            gateway, op);
        return error_response(drogon::k503ServiceUnavailable, "shm writer not available");
    }
    const nlohmann::json op_req = {{"op", op}, {"params", params}};
    if (!shm_writer_->write_td_modify_config(gateway, op_req)) {
        SPDLOG_ERROR("td account config dispatch failed | gateway={} op={}", gateway, op);
        return error_response(drogon::k503ServiceUnavailable, "shm write failed");
    }
    SPDLOG_INFO("td account config dispatched | gateway={} op={}", gateway, op);
    return nullptr;
}

void TdAccountCtrl::list(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；本端点权限是「任意已认证用户」（契约 rest §2.9.9），
    // 与全部交易读端点一致——**不是** admin 专属。
    (void)req;
    if (!registry_) {
        SPDLOG_WARN("td account registry not wired");
        callback(json_response(drogon::k200OK, nlohmann::json{{"available", false},
                                                              {"reason", "no_gateway_snapshot"}}));
        return;
    }
    if (!registry_->available()) {
        // 契约 rest §2.9.9：既未收到任何配置帧、platform.db 也未恢复出条目——此时
        // 「无账户」不可信（与「确实没有账户」不可区分），必须降级而不是回空数组，
        // 否则用户会以为账户被删了。
        callback(json_response(drogon::k200OK, nlohmann::json{{"available", false},
                                                              {"reason", "no_gateway_snapshot"}}));
        return;
    }
    nlohmann::json accounts = nlohmann::json::array();
    for (const auto& entry : registry_->entries()) {
        // 契约 rest §2.9.9：`account_name` 空串 = 未设置（不在服务端规范化，展示回落由
        // 前端解析层负责）；它同时是本端点作为「账户显示名唯一来源」的字段（ADR 0056）。
        accounts.push_back(nlohmann::json{{"account_id", entry.account_id},
                                          {"account_name", entry.account_name},
                                          {"gateway", entry.gateway},
                                          {"enabled", entry.enabled}});
    }
    callback(
        json_response(drogon::k200OK, nlohmann::json{{"available", true}, {"accounts", accounts}}));
}

void TdAccountCtrl::create(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 预检 1：admin（契约 rest §2.9.7）。code 用契约登记的 admin_required，
    // 不用其他端点的 admin_only 遗留形态（rest §1 明确新代码不得以它们为先例）。
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    if (!body.is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    auto gateway_it = body.find("gateway");
    auto account_it = body.find("account");
    if (gateway_it == body.end() || !gateway_it->is_string() || account_it == body.end() ||
        !account_it->is_object()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    const std::string gateway = gateway_it->get<std::string>();

    // 预检 2 + 3：已注册的 dztd_* 网关 + Running。
    if (auto err = check_gateway_ready(gateway); err != nullptr) {
        callback(err);
        return;
    }

    // 预检 4：通用层字段校验（契约 rest §2.9.7 预检 4，规则真相源 td-config §2.2 / §7）。
    // 契约区分「JSON 形状错误」（不带 code 的 bad request）与「字段值非法」（带 code 的
    // 预检失败）：缺失 / 类型不符留给下面的通用失败体，值非法走 invalid_account_id /
    // invalid_account_name。
    auto account_id_it = account_it->find("account_id");
    if (account_id_it == account_it->end() || !account_id_it->is_string()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    const std::string account_id = account_id_it->get<std::string>();
    if (auto err = check_account_id(account_id); err != nullptr) {
        callback(err);
        return;
    }
    std::string account_name;
    auto account_name_it = account_it->find("account_name");
    if (account_name_it != account_it->end()) {
        if (!account_name_it->is_string()) {
            callback(error_response(drogon::k400BadRequest, "bad request"));
            return;
        }
        account_name = account_name_it->get<std::string>();
        if (auto err = check_account_name(account_name); err != nullptr) {
            callback(err);
            return;
        }
    }

    // 预检 5：account_id 已在注册表中（**任何**网关，含已禁用/离线）→ 409。
    if (registry_ && registry_->contains(account_id)) {
        SPDLOG_WARN("td account config rejected: account exists | account={} gateway={}",
                    account_id, gateway);
        callback(error_with_code(drogon::k409Conflict, "account already exists", "account_exists"));
        return;
    }
    // 预检 5（续）：account_name 展平唯一性（留空按 account_id 参与判定；NFC 后字节比较）。
    if (auto err = check_account_name_conflict(account_name, account_id,
                                               /*exclude_account_id=*/"",
                                               /*check_candidate_id_against_names=*/true);
        err != nullptr) {
        callback(err);
        return;
    }

    // 预检 6：其余字段（含 ext）dzweb 不校验，透传给目标网关。
    // params = 契约 td-config §2.1 AddAccount 的 params：account 配置对象原样透传
    // （account_id 已在其中，不重复注入——参数形状就是账户配置本身）。
    if (auto err = dispatch(gateway, "AddAccount", *account_it); err != nullptr) {
        callback(err);
        return;
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

void TdAccountCtrl::update(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           const std::string& account_id) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    // PUT/DELETE 的 gateway 只能从注册表解析，故顺序：admin → 注册表解析 → dztd_* → Running
    // （契约 rest §2.9.7 的字面顺序在此不可机械照搬，见类注释）。
    const auto entry = registry_ ? registry_->find(account_id) : std::nullopt;
    if (!entry.has_value()) {
        SPDLOG_WARN("td account config rejected: account not found | account={}", account_id);
        callback(error_with_code(drogon::k404NotFound, "account not found", "account_not_found"));
        return;
    }
    if (auto err = check_gateway_ready(entry->gateway); err != nullptr) {
        callback(err);
        return;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    // 契约 rest §2.9.7：body 为 `UpdateAccount` 的 params 子集（字段级 merge），允许空对象
    // `{}`；**不得包含 account_id**（出现即 400，路径是唯一来源）。
    if (!body.is_object() || body.contains("account_id")) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    // 预检 4：通用层字段校验（PUT 也做，契约 rest §2.9.7；顺序在注册表解析 404 与网关
    // 检查之后——见类注释的 PUT 顺序 1→6→2→3→4）。路径上的 account_id 是既有账户的值，
    // 同样按同一规则判定（旧库/手改配置可能带非法值，不能放行到网关）。
    if (auto err = check_account_id(account_id); err != nullptr) {
        callback(err);
        return;
    }
    if (auto name_it = body.find("account_name"); name_it != body.end()) {
        if (!name_it->is_string()) {
            callback(error_response(drogon::k400BadRequest, "bad request"));
            return;
        }
        const std::string account_name = name_it->get<std::string>();
        if (auto err = check_account_name(account_name); err != nullptr) {
            callback(err);
            return;
        }
        // 预检 5（改名）：唯一性同 POST，排除自身（契约 rest §2.9.7 预检 6）。
        if (auto err = check_account_name_conflict(account_name, account_id,
                                                   /*exclude_account_id=*/account_id,
                                                   /*check_candidate_id_against_names=*/false);
            err != nullptr) {
            callback(err);
            return;
        }
    }

    // dzweb 转发前以路径值**注入** params.account_id（`{}` → `{"account_id": <路径>}`）。
    nlohmann::json params = body;
    params["account_id"] = account_id;
    if (auto err = dispatch(entry->gateway, "UpdateAccount", params); err != nullptr) {
        callback(err);
        return;
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

void TdAccountCtrl::remove(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           const std::string& account_id) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    const auto entry = registry_ ? registry_->find(account_id) : std::nullopt;
    if (!entry.has_value()) {
        SPDLOG_WARN("td account config rejected: account not found | account={}", account_id);
        callback(error_with_code(drogon::k404NotFound, "account not found", "account_not_found"));
        return;
    }
    if (auto err = check_gateway_ready(entry->gateway); err != nullptr) {
        callback(err);
        return;
    }
    // DELETE 无 body（忽略请求体）：契约 rest §2.9.7。
    (void)req;
    if (auto err = dispatch(entry->gateway, "RemoveAccount", {{"account_id", account_id}});
        err != nullptr) {
        callback(err);
        return;
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

// ============================================================================
// 账户会话控制（契约 rest §2.9.10 / 契约 td-connection）
// ============================================================================

void TdAccountCtrl::login(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::string& account_id) {
    handle_session_action(req, account_id, /*is_connect=*/true, std::move(callback));
}

void TdAccountCtrl::logout(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           const std::string& account_id) {
    handle_session_action(req, account_id, /*is_connect=*/false, std::move(callback));
}

void TdAccountCtrl::handle_session_action(
    const drogon::HttpRequestPtr& req,
    const std::string& account_id,
    bool is_connect,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 统一守卫（td_session_guard.h，REST/WS 共用）：admin → 账户已登记（解析网关）→
    // 网关 Running → 写通道可用。顺序即契约 rest §2.9.10 预检 1–4。
    const bool writer_ready = shm_writer_ != nullptr && shm_writer_->is_ready();
    const auto check = evaluate_td_session_guard(is_admin(req), registry_.get(),
                                                 process_mirror_.get(), writer_ready, account_id);
    switch (check.result) {
        case TdSessionGuard::NotAdmin:
            callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
            return;
        case TdSessionGuard::AccountInvalid:
            SPDLOG_WARN("td session action rejected | reason=account_not_found account={}",
                        account_id);
            callback(
                error_with_code(drogon::k404NotFound, "account not found", "account_not_found"));
            return;
        case TdSessionGuard::RegistryUnavailable:
            // 注册表降级：无法解析归属，属"请求未被受理"的基础设施失败（不带 code）。
            SPDLOG_WARN(
                "td session action rejected | reason=account_registry_unavailable "
                "account={}",
                account_id);
            callback(
                error_response(drogon::k503ServiceUnavailable, "account registry unavailable"));
            return;
        case TdSessionGuard::GatewayNotRunning:
            SPDLOG_WARN("td session action rejected | reason=gateway_not_running gateway={}",
                        check.gateway);
            callback(error_with_code(drogon::k409Conflict, "td gateway not running",
                                     "gateway_not_running"));
            return;
        case TdSessionGuard::ChannelUnavailable:
            SPDLOG_ERROR(
                "td session action rejected | reason=writer_not_ready account={} "
                "gateway={}",
                account_id, check.gateway);
            callback(error_response(drogon::k503ServiceUnavailable, "shm writer not available"));
            return;
        case TdSessionGuard::Ok:
            break;
    }
    const bool ok = is_connect ? shm_writer_->write_td_connect(check.gateway, account_id)
                               : shm_writer_->write_td_disconnect(check.gateway, account_id);
    if (!ok) {
        SPDLOG_ERROR("td session action write failed | action={} gateway={} account={}",
                     is_connect ? "connect" : "disconnect", check.gateway, account_id);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td session action dispatched | action={} gateway={} account={}",
                is_connect ? "connect" : "disconnect", check.gateway, account_id);
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
