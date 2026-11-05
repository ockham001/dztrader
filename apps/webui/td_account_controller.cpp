#include "td_account_controller.h"

#include <dztrader/core/core_data_type.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <utility>

namespace dztrader::webui {

namespace {

/// `DzAccountId` 是 `char[32]`，可用长度 = 31 字节（末字节留给 NUL）。
/// 契约 rest §2.9.7 预检 4 的「≤ 31 字节」即此容量，不用另写魔数。
constexpr size_t kMaxAccountIdBytes = sizeof(DzAccountId) - 1;

constexpr const char* kTdGatewayPrefix = "dztd_";

}  // namespace

bool TdAccountCtrl::is_registered_td_gateway(const std::string& gateway) const {
    if (gateway.empty() || !gateway.starts_with(kTdGatewayPrefix)) {
        return false;
    }
    // 契约 rest §2.9.7 预检 2：必须是「进程配置中的 dztd_*」——仅进程名形如 dztd_ 不够，
    // 未注册的进程不该收到配置写请求（总则 §7 兜底会把运维的误操作变成超时）。
    return process_mirror_ && process_mirror_->get_process_config(gateway).has_value();
}

bool TdAccountCtrl::is_gateway_running(const std::string& gateway) const {
    if (!process_mirror_) {
        return false;
    }
    const auto status = process_mirror_->get_status(gateway);
    return status.has_value() && status->state == platform::ChildState::Running;
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
        accounts.push_back(nlohmann::json{{"account_id", entry.account_id},
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

    // 预检 4：account_id 非空且 ≤ 31 字节（DzAccountId 容量，契约 rest §2.9.7）。
    auto account_id_it = account_it->find("account_id");
    if (account_id_it == account_it->end() || !account_id_it->is_string()) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }
    const std::string account_id = account_id_it->get<std::string>();
    if (account_id.empty() || account_id.size() > kMaxAccountIdBytes) {
        callback(error_response(drogon::k400BadRequest, "bad request"));
        return;
    }

    // 预检 5：account_id 已在注册表中（**任何**网关，含已禁用/离线）→ 409。
    if (registry_ && registry_->contains(account_id)) {
        SPDLOG_WARN("td account config rejected: account exists | account={} gateway={}",
                    account_id, gateway);
        callback(error_with_code(drogon::k409Conflict, "account already exists", "account_exists"));
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

}  // namespace dztrader::webui
