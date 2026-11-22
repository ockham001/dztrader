#include "td_gateway_controller.h"

#include <dztrader/core/this_process.h>
#include <dztrader/platform/process.h>
#include <dztrader/process/exe_scanner.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 交易接口进程名前缀（契约 process 的 `dztd_*` 命名，预检 2 的唯一判据）。
constexpr std::string_view kTdGatewayPrefix = "dztd_";

/// `GET /api/td/gateways/available` 的单行结果（排序后序列化）。
struct AvailableGateway {
    std::string name;
    std::string display_name;
    bool added = false;
    bool running = false;
};

/// 从进程配置镜像条目取 display_name：缺失 / 非字符串 / 空串都回退进程名
/// （契约 rest §2.10：`display_name` = 已注册时取配置值，未注册时回退 `name`）。
std::string display_name_or_name(const nlohmann::json& config, const std::string& name) {
    if (!config.is_object()) {
        return name;
    }
    const auto it = config.find("display_name");
    if (it == config.end() || !it->is_string()) {
        return name;
    }
    std::string display_name = it->get<std::string>();
    return display_name.empty() ? name : display_name;
}

}  // namespace

TdGatewayCtrl::TdGatewayCtrl(std::shared_ptr<Repository> repo,
                             std::shared_ptr<TdAccountRegistry> registry,
                             std::shared_ptr<ProcessMirror> process_mirror,
                             std::shared_ptr<ShmWriter> shm_writer,
                             std::filesystem::path scan_root)
    : ControllerBase(std::move(repo)),
      registry_(std::move(registry)),
      process_mirror_(std::move(process_mirror)),
      shm_writer_(std::move(shm_writer)),
      scan_root_(std::move(scan_root)) {}

bool TdGatewayCtrl::is_valid_gateway_name(const std::string& name) {
    return name.size() > kTdGatewayPrefix.size() && name.starts_with(kTdGatewayPrefix);
}

bool TdGatewayCtrl::is_registered(const std::string& name) const {
    return process_mirror_ && process_mirror_->get_process_config(name).has_value();
}

drogon::HttpResponsePtr TdGatewayCtrl::check_writer_ready() const {
    if (!shm_writer_ || !shm_writer_->is_ready()) {
        // 事件通道不可用：请求未被受理（基础设施问题），不是预检失败，故不带 code。
        SPDLOG_ERROR("td gateway control rejected | reason=writer_not_ready");
        return error_response(drogon::k503ServiceUnavailable, "shm writer not available");
    }
    return nullptr;
}

std::filesystem::path TdGatewayCtrl::scan_root() const {
    return scan_root_.empty() ? dztrader::this_process::app_root() : scan_root_;
}

// ---------------------------------------------------------------------------
// GET /api/td/gateways/available
// ---------------------------------------------------------------------------
void TdGatewayCtrl::list_available(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // 认证由 JWT advice 处理；本端点权限是「任意已认证用户」（契约 rest §2.10），
    // 与全部交易读端点一致——**不是** admin 专属。
    (void)req;
    const std::filesystem::path root = scan_root();
    const auto scanned = dztrader::process::scan_all_exes(root);

    std::vector<AvailableGateway> gateways;
    gateways.reserve(scanned.size());
    for (const auto& info : scanned) {
        if (info.kind != dztrader::process::ProcessKind::GatewayTd) {
            continue;  // `dzmd_*` / dzweb / dzsysmon 不在交易接口列表内
        }
        AvailableGateway gateway{.name = info.name, .display_name = info.name};
        if (process_mirror_) {
            if (const auto config = process_mirror_->get_process_config(info.name);
                config.has_value()) {
                gateway.added = true;
                gateway.display_name = display_name_or_name(*config, info.name);
            }
            if (const auto status = process_mirror_->get_status(info.name); status.has_value()) {
                gateway.running = status->state == platform::ChildState::Running;
            }
        }
        gateways.push_back(std::move(gateway));
    }
    // 契约 rest §2.10：按 name 升序（扫描器本身有序，这里显式排序把接口契约钉在端点侧）。
    std::sort(gateways.begin(), gateways.end(),
              [](const AvailableGateway& lhs, const AvailableGateway& rhs) {
                  return lhs.name < rhs.name;
              });

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& gateway : gateways) {
        arr.push_back(nlohmann::json{{"name", gateway.name},
                                     {"display_name", gateway.display_name},
                                     {"added", gateway.added},
                                     {"running", gateway.running}});
    }
    SPDLOG_INFO("available td gateways scanned | count={} dir={}", arr.size(), root.string());
    callback(json_response(drogon::k200OK, arr));
}

// ---------------------------------------------------------------------------
// POST /api/td/gateways/{name}/start
// ---------------------------------------------------------------------------
void TdGatewayCtrl::start(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                          const std::string& name) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    if (!is_valid_gateway_name(name)) {
        SPDLOG_WARN("td gateway start rejected | reason=invalid_gateway name={}", name);
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    // 预检 3：未注册时 master 的动态注册只接受 App Root 下可扫描到的网关 exe。
    if (!is_registered(name) &&
        !dztrader::process::find_exe_by_stem(name, scan_root()).has_value()) {
        SPDLOG_WARN("td gateway start rejected | reason=invalid_gateway name={} registered=false",
                    name);
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    // 可选 body `{"display_name":...}`：仅此字段作为 Start 的配置 patch（契约 rest §2.10）。
    // 其余字段不透传（master 的 patch 校验只认已知字段），body 为空也允许。
    std::string display_name;
    if (const auto& body_text = req->getBody(); !body_text.empty()) {
        try {
            const auto body = nlohmann::json::parse(body_text);
            if (!body.is_object()) {
                callback(error_response(drogon::k400BadRequest, "bad request"));
                return;
            }
            const auto it = body.find("display_name");
            if (it != body.end() && it->is_string()) {
                display_name = it->get<std::string>();
            }
        } catch (const std::exception&) {
            callback(error_response(drogon::k400BadRequest, "bad request"));
            return;
        }
    }
    if (auto err = check_writer_ready(); err != nullptr) {
        callback(err);
        return;
    }
    // 与 market_source_controller 的 start 同款：display_name 非空才携带 patch，
    // 空串 = 不下发配置（master 侧空串语义是清空，不能把「未提供」误当清空）。
    std::optional<nlohmann::json> start_config;
    if (!display_name.empty()) {
        start_config = nlohmann::json{{"display_name", display_name}};
    }
    if (!shm_writer_->write_process_control(platform::ProcessAction::Start, name,
                                            std::move(start_config))) {
        SPDLOG_ERROR("td gateway start write failed | name={}", name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td gateway start dispatched | name={} display_name={}", name, display_name);
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

// ---------------------------------------------------------------------------
// POST /api/td/gateways/{name}/stop
// ---------------------------------------------------------------------------
void TdGatewayCtrl::stop(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                         const std::string& name) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    if (!is_valid_gateway_name(name)) {
        SPDLOG_WARN("td gateway stop rejected | reason=invalid_gateway name={}", name);
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    // 预检 4：未注册 → 404（Stop 对不存在目标是失败，不是幂等成功）。
    if (!is_registered(name)) {
        SPDLOG_WARN("td gateway stop rejected | reason=gateway_not_found name={}", name);
        callback(error_with_code(drogon::k404NotFound, "gateway not found", "gateway_not_found"));
        return;
    }
    if (auto err = check_writer_ready(); err != nullptr) {
        callback(err);
        return;
    }
    if (!shm_writer_->write_process_control(platform::ProcessAction::Stop, name)) {
        SPDLOG_ERROR("td gateway stop write failed | name={}", name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td gateway stop dispatched | name={}", name);
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

// ---------------------------------------------------------------------------
// DELETE /api/td/gateways/{name}
// ---------------------------------------------------------------------------
void TdGatewayCtrl::remove(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           const std::string& name) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    if (!is_valid_gateway_name(name)) {
        SPDLOG_WARN("td gateway remove rejected | reason=invalid_gateway name={}", name);
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    // 预检 4：Remove 不幂等，已移除视为失败（契约 rest §2.10）。
    if (!is_registered(name)) {
        SPDLOG_WARN("td gateway remove rejected | reason=gateway_not_found name={}", name);
        callback(error_with_code(drogon::k404NotFound, "gateway not found", "gateway_not_found"));
        return;
    }
    // 预检 5：账户注册表 guard。注册表降级时「无账户」不可信（与「确实没有账户」不可
    // 区分）→ 503 且不写帧，不得在无法确认时移除接口（ADR 0053 决策 3）。
    if (!registry_ || !registry_->available()) {
        SPDLOG_WARN("td gateway remove rejected | reason=account_registry_unavailable name={}",
                    name);
        callback(error_response(drogon::k503ServiceUnavailable, "account registry unavailable"));
        return;
    }
    if (registry_->gateway_has_accounts(name)) {
        // 账户配置归属接口自身文件（契约 td-config §6）：带账户移除会让账户在重新添加
        // 时复活，且期间 account_id 全局唯一校验出现窗口（ADR 0045 / 0053）。
        SPDLOG_WARN("td gateway remove rejected | reason=gateway_has_accounts name={}", name);
        callback(
            error_with_code(drogon::k409Conflict, "gateway has accounts", "gateway_has_accounts"));
        return;
    }
    if (auto err = check_writer_ready(); err != nullptr) {
        callback(err);
        return;
    }
    if (!shm_writer_->write_process_control(platform::ProcessAction::Remove, name)) {
        SPDLOG_ERROR("td gateway remove write failed | name={}", name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td gateway remove dispatched | name={}", name);
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
