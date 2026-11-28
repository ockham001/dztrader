#ifndef DZTRADER_WEBUI_TD_GATEWAY_CONTROLLER_H_
#define DZTRADER_WEBUI_TD_GATEWAY_CONTROLLER_H_

#include <drogon/HttpController.h>

#include <filesystem>
#include <memory>
#include <string>

#include "controller_base.h"
#include "process_mirror.h"
#include "repository.h"
#include "shm_writer.h"
#include "td_account_registry.h"

namespace dztrader::webui {

/// 交易接口（td 网关，`dztd_*`）生命周期端点的 REST 实现（契约 rest §2.10、ADR 0053）。
///
/// 与账户端点（`TdAccountCtrl`）的分工：接口进程可**零账户**存在并运行，账户是接口内部
/// 配置项；本控制器只管接口进程的可见性与生命周期，不碰账户配置。
///
/// - `GET /api/td/gateways/available`：扫描 App Root 的 `dztd_*` 可执行文件，叠加进程配置
///   镜像的 `added` 与进程状态的 `running`（**任意已认证用户**，与全部交易读端点一致）。
/// - `POST /api/td/gateways/{name}/start`：添加/启动接口（admin）→
///   `REQUEST_PROCESS_CONTROL{Start}`；未注册时 master 动态注册并持久化（契约 process），
///   可选 body `{"display_name":...}` 作为 Start 携带的配置 patch。
/// - `POST /api/td/gateways/{name}/stop`：停止接口（admin）→ `REQUEST_PROCESS_CONTROL{Stop}`
///   （瞬态，不是持久期望态：master 启动时按进程配置无差别拉起）。
/// - `DELETE /api/td/gateways/{name}`：移除接口（admin）→ `REQUEST_PROCESS_CONTROL{Remove}`
///   （停止 + 删进程配置条目）；有账户时拒绝（409 `gateway_has_accounts`）。
///
/// 响应 `200 {"ok":true}` 只表示已写入事件通道，生效信号是 WS `process_config` /
/// `process_status`（契约 process）——与账户端点同语义（契约 rest §1 设置类请求）。
///
/// 预检顺序（契约 rest §2.10，失败时不写帧）：
/// 1. admin；否则 403 `admin_required`。
/// 2. `name` 不匹配 `dztd_*` → 400 `invalid_gateway`。
/// 3. start：目标既未注册、App Root 下也扫描不到同名 exe → 400 `invalid_gateway`
///    （master 的动态注册只接受可扫描到的网关 exe）。
/// 4. stop / DELETE：目标未注册 → 404 `gateway_not_found`（Remove 不幂等）。
/// 5. DELETE guard：账户注册表里该接口仍有任何账户（含已禁用/离线）→ 409
///    `gateway_has_accounts`；注册表降级（`available:false`）→ 503
///    `{"error":"account registry unavailable"}`（不带 code，不得在无法确认时移除接口）。
/// 6. 写通道不可用 / 写失败 → 503（不带 code，同 §2.9.7）。
class TdGatewayCtrl : public drogon::HttpController<TdGatewayCtrl, false>, public ControllerBase {
public:
    /// @param repo 用户数据源（admin 判定在 ControllerBase）
    /// @param registry 账户注册表（DELETE 的「有无账户」guard，契约 rest §2.10 预检 5）
    /// @param process_mirror 进程镜像（是否已注册 / 是否 Running / display_name）
    /// @param shm_writer 事件通道写入
    /// @param scan_root 可用接口扫描根；空路径 = `this_process::app_root()`（生产默认）。
    ///        测试注入临时目录，避免单测依赖测试二进制所在目录里恰好有 `dztd_*` exe。
    TdGatewayCtrl(std::shared_ptr<Repository> repo,
                  std::shared_ptr<TdAccountRegistry> registry,
                  std::shared_ptr<ProcessMirror> process_mirror,
                  std::shared_ptr<ShmWriter> shm_writer,
                  std::filesystem::path scan_root = {});

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TdGatewayCtrl::list_available, "/api/td/gateways/available", drogon::Get);
    ADD_METHOD_TO(TdGatewayCtrl::start, "/api/td/gateways/{1}/start", drogon::Post);
    ADD_METHOD_TO(TdGatewayCtrl::stop, "/api/td/gateways/{1}/stop", drogon::Post);
    ADD_METHOD_TO(TdGatewayCtrl::remove, "/api/td/gateways/{1}", drogon::Delete);
    ADD_METHOD_TO(TdGatewayCtrl::get_auto_login, "/api/td/gateways/{1}/auto-login", drogon::Get);
    ADD_METHOD_TO(TdGatewayCtrl::set_auto_login, "/api/td/gateways/{1}/auto-login", drogon::Put);
    METHOD_LIST_END

    /// GET `/api/td/gateways/available`（契约 rest §2.10）：任意已认证用户。
    /// 200，按 `name` 升序：
    /// `[{name, display_name, added, running}]`——`added` = 进程配置镜像有条目；
    /// `running` = 进程状态为 Running；`display_name` = 镜像配置值，缺失回退 `name`。
    void list_available(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    /// POST `/api/td/gateways/{name}/start`（admin）：body 可选 `{"display_name":...}`，
    /// 非空时作为 Start 帧的配置 patch（与行情源 start 同款调用）。未注册但 App Root 下
    /// 可扫描到同名 exe 时由 master 动态注册（契约 process）。
    void start(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& name);

    /// POST `/api/td/gateways/{name}/stop`（admin）：→ `REQUEST_PROCESS_CONTROL{Stop}`。
    void stop(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
              const std::string& name);

    /// DELETE `/api/td/gateways/{name}`（admin）：→ `REQUEST_PROCESS_CONTROL{Remove}`。
    /// 前置 guard：该接口仍有任何账户时 409，注册表降级时 503（见类注释预检 5）。
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& name);

    /// GET `/api/td/gateways/{name}/auto-login`（任意已认证用户，契约 rest §2.10 /
    /// 契约 auto-login）：读取该接口的自动登录/登出排程。
    ///
    /// 200 `{enabled, schedules, source}`：
    /// - `enabled`/`schedules`：镜像 `auto_login` 域的当前值；
    /// - `source`：`"mirror"` = 来自网关实时镜像；`"unavailable"` = 网关未运行 / 镜像尚无
    ///   该域，`enabled`/`schedules` 是**占位值、不代表真实生效配置**。这里**不编造**网关
    ///   内置默认排程（默认表是网关知识，dzweb 复制一份必然漂移；契约 rest §2.10）。
    ///
    /// 排程是**进程级**（一个网关一份），不是账户级——登出会断开该网关全部 session。
    void get_auto_login(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        const std::string& name);

    /// PUT `/api/td/gateways/{name}/auto-login`（admin，契约 rest §2.10 / 契约 auto-login）：
    /// RFC 7386 增量设置排程 → `SET_AUTO_LOGIN` 帧。body `{enabled?, schedules?}`；
    /// UI 侧发全量。200 `{"ok":true}` 只表示**已写入事件通道**；生效信号是 WS `auto_login` 推送。
    /// 校验复用 `platform::AutoLoginConfig::validate`（校验规则唯一真相源），非法 400；
    /// body 任何位置出现 `null` 均按契约判非法（400 `invalid_auto_login`）。
    void set_auto_login(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        const std::string& name);

private:
    /// `name` 是否为合法的交易接口进程名（`dztd_*`，契约 process 命名，预检 2）。
    static bool is_valid_gateway_name(const std::string& name);

    /// 该进程名是否已在 master 进程配置镜像中（契约 process；预检 3/4）。
    [[nodiscard]] bool is_registered(const std::string& name) const;

    /// 写通道就绪检查：未就绪返回 503（不带 code）；返回 nullptr = 可用。
    drogon::HttpResponsePtr check_writer_ready() const;

    /// 扫描根：注入值优先，否则 `this_process::app_root()`（契约 process：dzweb 扫描
    /// dzmd/dztd 必须用 App Root，不得用自身 exe 目录或 cwd）。
    [[nodiscard]] std::filesystem::path scan_root() const;

    std::shared_ptr<TdAccountRegistry> registry_;
    std::shared_ptr<ProcessMirror> process_mirror_;
    std::shared_ptr<ShmWriter> shm_writer_;
    std::filesystem::path scan_root_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_GATEWAY_CONTROLLER_H_
