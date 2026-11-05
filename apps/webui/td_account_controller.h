#ifndef DZTRADER_WEBUI_TD_ACCOUNT_CONTROLLER_H_
#define DZTRADER_WEBUI_TD_ACCOUNT_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <memory>
#include <string>

#include "controller_base.h"
#include "process_mirror.h"
#include "repository.h"
#include "shm_writer.h"
#include "td_account_registry.h"

namespace dztrader::webui {

/// 交易账户注册表与账户配置写端点的 REST 实现（契约 rest §2.9.7 / §2.9.9）。
///
/// - `GET /api/td/accounts`：账户注册表全集（**任意已认证用户**，不是 admin——与全部交易
///   读端点权限一致）。这是账户配置页的**唯一账户列表来源**：跨网关的账户全集 + 唯一性
///   预检需要的 `account_id → gateway` 投影，单网关的配置镜像拼不出来。
/// - `POST /api/td/accounts`：添加账户（admin）→ `TD_REQ_MODIFY_CONFIG{AddAccount}`。
/// - `PUT /api/td/accounts/{account_id}`：修改账户（admin，字段级 merge）→
///   `TD_REQ_MODIFY_CONFIG{UpdateAccount}`。
/// - `DELETE /api/td/accounts/{account_id}`：删除账户（admin；不删交易数据，ADR 0045）→
///   `TD_REQ_MODIFY_CONFIG{RemoveAccount}`。
///
/// 响应 `200 {"ok":true}` **只表示已写入事件通道**，不代表业务已生效（契约 rest §1 设置类
/// 请求语义）：生效/失败以 `RTN_TD_CONFIG` 为准（成功推新值、失败推回滚旧值）。
///
/// 预检顺序（契约 rest §2.9.7）：
/// - POST：admin(403) → 网关是已注册的 `dztd_*`(400 `invalid_gateway`) → 网关 Running
///   (409 `gateway_not_running`) → `account_id` 非空且 ≤31 字节(400) → 注册表查重
///   (409 `account_exists`)。
/// - PUT/DELETE：admin(403) → 注册表解析 `account_id → gateway`(404 `account_not_found`)
///   → 网关是 `dztd_*`(400 `invalid_gateway`) → Running(409 `gateway_not_running`)。
///   **顺序与 POST 不同是有意的**：PUT/DELETE 的 gateway 只能从注册表解析出来，契约
///   §2.9.7 的字面顺序（先校验网关）在这两个方法上不可机械照搬。
///
/// 错误体带机器码 `code`（契约 rest §1「需要机器判别的失败」的形状），前端只认 `code`。
/// 唯一不带 code 的失败是事件通道写失败（503）——那属"请求未被受理"（基础设施不可用），
/// 不是预检失败，也不在契约 §2.9.7 登记的 code 族里。
class TdAccountCtrl : public drogon::HttpController<TdAccountCtrl, false>, public ControllerBase {
public:
    /// @param repo 用户数据源（admin 判定在 ControllerBase）
    /// @param registry 账户注册表（读端点数据源 + 写端点唯一性预检）
    /// @param process_mirror 进程镜像（网关是否已注册 / 是否 Running，契约 process）
    /// @param shm_writer 事件通道写入
    TdAccountCtrl(std::shared_ptr<Repository> repo,
                  std::shared_ptr<TdAccountRegistry> registry,
                  std::shared_ptr<ProcessMirror> process_mirror,
                  std::shared_ptr<ShmWriter> shm_writer)
        : ControllerBase(std::move(repo)),
          registry_(std::move(registry)),
          process_mirror_(std::move(process_mirror)),
          shm_writer_(std::move(shm_writer)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TdAccountCtrl::list, "/api/td/accounts", drogon::Get);
    ADD_METHOD_TO(TdAccountCtrl::create, "/api/td/accounts", drogon::Post);
    ADD_METHOD_TO(TdAccountCtrl::update, "/api/td/accounts/{1}", drogon::Put);
    ADD_METHOD_TO(TdAccountCtrl::remove, "/api/td/accounts/{1}", drogon::Delete);
    METHOD_LIST_END

    /// GET `/api/td/accounts`（契约 rest §2.9.9）：任意已认证用户。
    /// 可用 → 200 `{available:true, accounts:[{account_id, gateway, enabled}]}`（按
    /// account_id 升序）；尚未收到任何配置帧且未从 platform.db 恢复出条目 → 200
    /// `{available:false, reason:"no_gateway_snapshot"}`（"无账户"不可信，前端不得当空列表）。
    void list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// POST `/api/td/accounts`（admin）：body `{gateway, account:{...}}`，account 为契约
    /// td-config §2.2 的账户配置（`account_id` 必填；`ext` 由目标网关校验）。
    void create(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// PUT `/api/td/accounts/{account_id}`（admin）：body 为 `UpdateAccount` 的 params 子集
    /// （字段级 merge，**不得包含 account_id**，路径是唯一来源；允许空对象 `{}`）。
    void update(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& account_id);

    /// DELETE `/api/td/accounts/{account_id}`（admin）：无 body（忽略请求体）。
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& account_id);

private:
    /// 带机器码的错误体 `{"error": <human>, "code": <snake_case>}`（契约 rest §1）。
    static drogon::HttpResponsePtr error_with_code(drogon::HttpStatusCode status,
                                                   const std::string& message,
                                                   const std::string& code) {
        return json_response(status, nlohmann::json{{"error", message}, {"code", code}});
    }

    /// 网关是否为已注册的交易网关进程（进程配置中的 `dztd_*`，契约 rest §2.9.7 预检 2）。
    bool is_registered_td_gateway(const std::string& gateway) const;

    /// 网关当前是否 Running（`process_status`，契约 process；预检 3）。
    bool is_gateway_running(const std::string& gateway) const;

    /// 预检 2 + 3（PUT/DELETE 在注册表解析之后共用）。返回 nullptr = 通过。
    drogon::HttpResponsePtr check_gateway_ready(const std::string& gateway) const;

    /// 写 `TD_REQ_MODIFY_CONFIG` 帧；失败 → 503（不带 code，见类注释）。返回 nullptr = 成功。
    drogon::HttpResponsePtr dispatch(const std::string& gateway,
                                     const std::string& op,
                                     const nlohmann::json& params);

    std::shared_ptr<TdAccountRegistry> registry_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_CONTROLLER_H_
