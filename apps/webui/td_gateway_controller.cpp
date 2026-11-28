#include "td_gateway_controller.h"

#include <dztrader/core/this_process.h>
#include <dztrader/platform/auto_login.h>  // AutoLoginConfig::validate（契约 auto-login 校验真相源）
#include <dztrader/platform/process.h>
#include <dztrader/process/exe_scanner.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "td_session_guard.h"

namespace dztrader::webui {

namespace {

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
    // 前缀谓词由 td_session_guard.h 单一实现；这里额外要求 `dztd_` 后非空（退化名不是
    // 合法进程身份，且 master 动态注册依赖可扫描 exe stem，契约 process 命名）。
    return is_td_gateway_name(name) && name.size() > kTdGatewayNamePrefix.size();
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
    SPDLOG_INFO("available td gateways scanned | count={} dir=\"{}\"", arr.size(), root.string());
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
    // 与 market_source_controller 的 start 同一条规则（唯一实现见 controller_base.h 的
    // process_start_config_patch）：display_name 非空才携带 patch，空串 = 未提供（不误当清空）。
    const std::optional<nlohmann::json> start_config = process_start_config_patch(display_name);
    if (!shm_writer_->write_process_control(platform::ProcessAction::Start, name, start_config)) {
        SPDLOG_ERROR("td gateway start write failed | name={}", name);
        callback(error_response(drogon::k503ServiceUnavailable, "shm write failed"));
        return;
    }
    SPDLOG_INFO("td gateway start dispatched | name={} display_name=\"{}\"", name, display_name);
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

// ---------------------------------------------------------------------------
// GET /api/td/gateways/{name}/auto-login（契约 rest §2.10 / 契约 auto-login）
// ---------------------------------------------------------------------------
void TdGatewayCtrl::get_auto_login(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                   const std::string& name) {
    (void)req;  // 认证由 JWT advice；本端点权限「任意已认证用户」（与全部交易读端点一致）
    if (!is_valid_gateway_name(name)) {
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    // 镜像里的 auto_login 域由网关进程的 RTN_AUTO_LOGIN 填充（契约 auto-login）。
    // 缺失有两种情况：网关从未启动过 / 刚启动还没回 RTN。
    // **不在这里编造内置默认排程**：默认表是网关的知识（apps/ctp/common），
    // dzweb 复制一份必然漂移。用 source 字段如实告诉前端「当前拿不到实时值」。
    if (process_mirror_) {
        if (auto it = process_mirror_->get_auto_login(name); it.has_value()) {
            nlohmann::json out = *it;
            out["source"] = "mirror";
            callback(json_response(drogon::k200OK, out));
            return;
        }
    }
    SPDLOG_INFO("td auto-login read | name={} source=unavailable", name);
    callback(json_response(drogon::k200OK, nlohmann::json{{"source", "unavailable"},
                                                          {"enabled", false},
                                                          {"schedules", nlohmann::json::array()}}));
}

// ---------------------------------------------------------------------------
// PUT /api/td/gateways/{name}/auto-login（admin，契约 rest §2.10 / 契约 auto-login）
// ---------------------------------------------------------------------------
void TdGatewayCtrl::set_auto_login(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                   const std::string& name) {
    if (!is_admin(req)) {
        callback(error_with_code(drogon::k403Forbidden, "admin required", "admin_required"));
        return;
    }
    if (!is_valid_gateway_name(name)) {
        callback(error_with_code(drogon::k400BadRequest, "invalid gateway", "invalid_gateway"));
        return;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        callback(error_with_code(drogon::k400BadRequest, "invalid json", "invalid_json"));
        return;
    }
    if (!body.is_object()) {
        callback(error_with_code(drogon::k400BadRequest, "body must be an object", "invalid_body"));
        return;
    }
    // 校验复用 platform::AutoLoginConfig（契约 auto-login 的校验唯一真相源），
    // 不做第二套规则。注意 validate 校验的是**合并后的完整配置**，因此这里先按
    // RFC 7386 语义把它与镜像里的当前值合并再校验——只发 {enabled:false} 这类增量
    // 也是合法的。
    nlohmann::json merged =
        nlohmann::json{{"enabled", false}, {"schedules", nlohmann::json::array()}};
    if (process_mirror_) {
        if (auto current = process_mirror_->get_auto_login(name); current.has_value()) {
            merged = *current;
        }
    }
    // 契约 auto-login：`null` 在本帧**任何位置**都是校验失败（不是 RFC 7386 的删键语义）。
    // 若把 null 抹掉再校验，校验的是 merged、下发的仍是含 null 的 body —— 网关必然拒绝，
    // 而这里已返回 200（#530 M4）。递归检查后统一 400 invalid_auto_login。
    bool has_null = false;
    const std::function<void(const nlohmann::json&)> find_null = [&](const nlohmann::json& node) {
        if (has_null) return;
        if (node.is_null()) {
            has_null = true;
            return;
        }
        if (node.is_object() || node.is_array()) {
            for (const auto& child : node) find_null(child);
        }
    };
    find_null(body);
    if (has_null) {
        SPDLOG_WARN("td auto-login set rejected | name={} reason=\"patch contains null\"", name);
        callback(error_with_code(drogon::k400BadRequest, "null field is not allowed",
                                 "invalid_auto_login"));
        return;
    }
    for (auto it = body.begin(); it != body.end(); ++it) {
        merged[it.key()] = it.value();
    }
    if (auto err = platform::AutoLoginConfig::validate(merged); err.has_value()) {
        // 400 带具体原因：契约要求非法输入可见，不静默吞。
        SPDLOG_WARN("td auto-login set rejected | name={} reason=\"{}\"", name, *err);
        callback(error_with_code(drogon::k400BadRequest, *err, "invalid_auto_login"));
        return;
    }
    if (!is_registered(name)) {
        // 网关未注册 = 没有进程能接收 SET_AUTO_LOGIN 帧；此时写帧会悬空到对端超时。
        callback(error_with_code(drogon::k404NotFound, "gateway not found", "gateway_not_found"));
        return;
    }
    if (auto err = check_writer_ready(); err != nullptr) {
        callback(err);
        return;
    }
    // write_set_auto_login 是 fire-and-forget（无返回值；内部失败只记日志），
    // 就绪性已由上方 check_writer_ready() 拦截。真正生效由 WS auto_login 推送确认。
    shm_writer_->write_set_auto_login(name, body);
    SPDLOG_INFO("td auto-login set dispatched | name={}", name);
    // 关闭自动登录是安全敏感操作（网关不再自动登录/登出），下发成功后打 WARN 审计日志
    // ——与行情侧 set_auto_login 同一口径。
    if (body.contains("enabled") && body["enabled"].is_boolean() && !body["enabled"].get<bool>()) {
        SPDLOG_WARN("td auto-login disable dispatched | name={}", name);
    }
    callback(json_response(drogon::k200OK, nlohmann::json{{"ok", true}}));
}

}  // namespace dztrader::webui
