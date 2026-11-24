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
/// - `POST /api/td/accounts/{account_id}/login`：登录交易账户（admin）→ `TD_CONNECT`；
/// - `POST /api/td/accounts/{account_id}/logout`：登出交易账户（admin，任意非未登录状态可发
///   = 强制中断）→ `TD_DISCONNECT`（契约 rest §2.9.10 / 契约 td-connection）。
///
/// 响应 `200 {"ok":true}` **只表示已写入事件通道**，不代表业务已生效（契约 rest §1 设置类
/// 请求语义）：配置写（POST/PUT/DELETE）以 `RTN_TD_CONFIG` 为准（成功推新值、失败推回滚
/// 旧值）；会话控制（login/logout）以 `progress` / `td_rtn_status` 为准（契约 td-connection）。
///
/// 预检顺序（契约 rest §2.9.7）：
/// - POST：admin(403 `admin_required`) → 网关是已注册的 `dztd_*`(400 `invalid_gateway`) →
///   网关 Running(409 `gateway_not_running`) → 通用层字段校验（`account_id` 非空 / ≤31 字节/
///   字符集/进程名冲突 → 400 `invalid_account_id`；`account_name` 长度/控制字符/UTF-8 →
///   400 `invalid_account_name`）→ 注册表查重（409 `account_exists`；展平名冲突
///   409 `account_name_exists`）。
/// - PUT/DELETE：admin(403) → 注册表解析 `account_id → gateway`(404 `account_not_found`)
///   → 网关是 `dztd_*`(400 `invalid_gateway`) → Running(409 `gateway_not_running`) → PUT 的
///   字段校验与唯一性（排除自身）。
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
    ADD_METHOD_TO(TdAccountCtrl::login, "/api/td/accounts/{1}/login", drogon::Post);
    ADD_METHOD_TO(TdAccountCtrl::logout, "/api/td/accounts/{1}/logout", drogon::Post);
    METHOD_LIST_END

    /// GET `/api/td/accounts`（契约 rest §2.9.9）：任意已认证用户。
    /// 可用 → 200 `{available:true, accounts:[{account_id, account_name, gateway, enabled}]}`
    /// （按 account_id 升序；`account_name` 空串 = 未设置，展示回落 account_id）；
    /// 尚未收到任何配置帧且未从 platform.db 恢复出条目 → 200
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

    /// POST `/api/td/accounts/{account_id}/login`（admin，契约 rest §2.9.10 / td-connection）：
    /// 登录该账户（建立会话并重走完整登录链）→ `TD_CONNECT`。响应只表示已写事件通道，
    /// 生效信号 = 该账户 `progress` / `td_rtn_status`。
    void login(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& account_id);

    /// POST `/api/td/accounts/{account_id}/logout`（admin）：登出该账户（任意非未登录状态可发，
    /// 即强制中断）→ `TD_DISCONNECT`。生效信号同上。
    void logout(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& account_id);

private:
    /// 网关是否为已注册的交易网关进程（进程配置中的 `dztd_*`，契约 rest §2.9.7 预检 2）。
    bool is_registered_td_gateway(const std::string& gateway) const;

    /// 网关当前是否 Running（`process_status`，契约 process；预检 3）。
    bool is_gateway_running(const std::string& gateway) const;

    /// 预检 2 + 3（PUT/DELETE 在注册表解析之后共用）。返回 nullptr = 通过。
    drogon::HttpResponsePtr check_gateway_ready(const std::string& gateway) const;

    /// `account_id` 是否等于任何已注册进程名（契约 td-config §7 / general §5：账户级与进程级
    /// `instance_id` 共用镜像键空间）。`dztraderd` 恒占位，其余查进程配置镜像。
    bool is_registered_process_name(const std::string& name) const;

    /// 预检 4：`account_id` 通用层校验（非空 / ≤31 字节 / ASCII 可见字符集 / 进程名冲突）。
    /// 返回 nullptr = 通过；否则 400 `invalid_account_id`。
    drogon::HttpResponsePtr check_account_id(const std::string& account_id) const;

    /// 预检 4：`account_name` 通用层校验（trim 后 ≤64 字节 / 无控制字符 / UTF-8）。
    /// 返回 nullptr = 通过；否则 400 `invalid_account_name`。
    drogon::HttpResponsePtr check_account_name(const std::string& account_name) const;

    /// 预检 5：`account_name` 与 `account_id` 的**展平唯一性**（契约 td-config §5/§7）。
    /// @param exclude_account_id PUT 改名时排除自身（POST 传空）
    /// @param check_candidate_id_against_names POST 新增 id 时还要判「新 id 与别人的名字冲突」
    ///        （PUT 的 id 未变，既有冲突与本次改名无关）
    /// 返回 nullptr = 通过；否则 409 `account_name_exists`。
    drogon::HttpResponsePtr check_account_name_conflict(
        const std::string& account_name,
        const std::string& account_id,
        const std::string& exclude_account_id,
        bool check_candidate_id_against_names) const;

    /// 写 `TD_REQ_MODIFY_CONFIG` 帧；失败 → 503（不带 code，见类注释）。返回 nullptr = 成功。
    drogon::HttpResponsePtr dispatch(const std::string& gateway,
                                     const std::string& op,
                                     const nlohmann::json& params);

    /// login/logout 共用：统一守卫（td_session_guard.h）+ 写 TD_CONNECT/TD_DISCONNECT + 映射
    /// （契约 rest §2.9.10：admin 403 / 账户未登记 404 / 注册表降级 503 / 网关非 Running 409 /
    /// 通道不可用 503）。
    void handle_session_action(const drogon::HttpRequestPtr& req,
                               const std::string& account_id,
                               bool is_connect,
                               std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    std::shared_ptr<TdAccountRegistry> registry_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_CONTROLLER_H_
