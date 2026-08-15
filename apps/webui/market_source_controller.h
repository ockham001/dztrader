#ifndef DZTRADER_WEBUI_MARKET_SOURCE_CONTROLLER_H_
#define DZTRADER_WEBUI_MARKET_SOURCE_CONTROLLER_H_

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "repository.h"
#include "shm_writer.h"
#include "process_mirror.h"
#include "controller_base.h"

namespace dztrader::webui {

// 单实例模式下, source_type → process.name 映射 ("CTP"→"dzmd_ctp")
// 与 master 的 ProcessRegistry::scan_gateways() 发现的 exe stem 一致
std::string process_name_for_source_type(const std::string& source_type);

// 提取 ui_card 大类 (契约 08 接口类型识别): 去掉 dzmd_/dztd_ 前缀, 取第一个 _ 之前的部分
// dzmd_ctp -> ctp, dzmd_ctp_1234 -> ctp, 未知前缀 -> ""
std::string extract_ui_card(const std::string& process_name);

class MarketSourceCtrl : public drogon::HttpController<MarketSourceCtrl, false>, public ControllerBase {
public:
    MarketSourceCtrl(std::shared_ptr<Repository> repo,
                     std::shared_ptr<ShmWriter> shm_writer,
                     std::shared_ptr<ProcessMirror> process_mirror)
        : ControllerBase(std::move(repo)),
          shm_writer_(std::move(shm_writer)),
          process_mirror_(std::move(process_mirror)) {}

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(MarketSourceCtrl::list_available, "/api/market-sources/available", drogon::Get);
    ADD_METHOD_TO(MarketSourceCtrl::list, "/api/market-sources", drogon::Get);
    ADD_METHOD_TO(MarketSourceCtrl::get, "/api/market-sources/{1}", drogon::Get);
    ADD_METHOD_TO(MarketSourceCtrl::create, "/api/market-sources", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::update, "/api/market-sources/{1}", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::remove, "/api/market-sources/{1}", drogon::Delete);
    ADD_METHOD_TO(MarketSourceCtrl::login, "/api/market-sources/{1}/login", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::logout, "/api/market-sources/{1}/logout", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::set_auto_login, "/api/market-sources/{1}/auto-login", drogon::Put);
    // Broker CRUD (Wave 2C): 走 SHM 配置变更流程, 不再使用 DB 持久化
    ADD_METHOD_TO(MarketSourceCtrl::add_broker,
                  "/api/market-sources/{1}/brokers", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::remove_broker,
                  "/api/market-sources/{1}/brokers/{2}", drogon::Delete);
    ADD_METHOD_TO(MarketSourceCtrl::update_broker,
                  "/api/market-sources/{1}/brokers/{2}", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::update_broker_frontends,
                  "/api/market-sources/{1}/brokers/{2}/frontends", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::set_current_broker,
                  "/api/market-sources/{1}/current-broker", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::set_subscribe_params,
                  "/api/market-sources/{1}/subscribe-params", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::set_shm_config,
                  "/api/market-sources/{1}/shm-config", drogon::Put);
    ADD_METHOD_TO(MarketSourceCtrl::start, "/api/market-sources/{1}/start", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::stop, "/api/market-sources/{1}/stop", drogon::Post);
    ADD_METHOD_TO(MarketSourceCtrl::get_config, "/api/market-sources/{1}/config", drogon::Get);
    ADD_METHOD_TO(MarketSourceCtrl::refresh, "/api/market-sources/refresh", drogon::Post);
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    /// GET /api/market-sources/available - 扫描 dzweb exe 同目录下所有 dzmd_* 行情源 exe
    /// 返回真实可用进程列表 [{name, display_name, added}]
    void list_available(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void get(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void create(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    void update(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void remove(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void login(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    void logout(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    /// PUT /api/market-sources/{id}/auto-login - 全量设置自动登录/登出排程
    /// Body: {enabled: bool, schedules: [{login_time, logout_time}, ...]}（契约 04）
    /// 直发 SET_AUTO_LOGIN 帧, 最终状态由 WS auto_login 推送决定
    void set_auto_login(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        int64_t id);
    /// POST /api/market-sources/{id}/brokers - 添加经纪商 (走 SHM 配置变更流程)
    /// Body: {name, broker_id, user_id, password, product_info}
    /// frontends 默认空数组, 子进程校验名称唯一性 (重复走失败路径 D)
    void add_broker(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                    int64_t id);
    /// DELETE /api/market-sources/{id}/brokers/{broker_name} - 删除经纪商
    /// 若删除的是当前选中, 子进程自动置空 current_broker_name (spec D-C4)
    void remove_broker(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       int64_t id, const std::string& broker_name);
    /// PUT /api/market-sources/{id}/brokers/{broker_name} - 编辑经纪商字段
    /// Body: 完整 BrokerEntry (name 必须与 URL 中一致, 其余字段可改)
    void update_broker(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       int64_t id, const std::string& broker_name);
    /// PUT /api/market-sources/{id}/brokers/{broker_name}/frontends - 替换前置地址列表
    /// Body: {frontends: [{address, label, enabled}, ...]}
    void update_broker_frontends(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                                 int64_t id, const std::string& broker_name);
    /// PUT /api/market-sources/{id}/current-broker - 切换当前选中经纪商
    /// Body: {name: "broker_name"} 或 {name: ""} 清空选中
    void set_current_broker(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                            int64_t id);
    /// PUT /api/market-sources/{id}/subscribe-params - 修改订阅参数 (SetSubscribeParams, 无状态保护)
    /// Body: 4 个可选字段 {subscribe_batch_size?, subscribe_batch_delay_ms?,
    ///        sub_check_interval_ms?, sub_max_retry?}，缺失=保留旧值 (契约 08)
    /// dzweb 透传不解析; 类型/范围校验由子进程负责; 最终状态由 WS md_rtn_config 推送
    void set_subscribe_params(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              int64_t id);
    /// PUT /api/market-sources/{id}/shm-config - 修改 SHM 行情通道配置 (契约 02)
    /// Body: ShmConfig 子集 (RFC 7386 递归合并)。preload_points 内 key 的 value 为 null
    /// 表示删除该时间点 (契约 02 唯一合法 null 位置); page_size_mb 网关端跳过。
    /// dzweb 透传不解析; 范围校验由网关负责; 最终状态由 WS md_shm_config 推送
    void set_shm_config(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        int64_t id);
    /// POST /api/market-sources/{id}/start - 启动行情源进程 (PROCESS_CONTROL action=start)
    void start(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    /// POST /api/market-sources/{id}/stop - 停止行情源进程 (PROCESS_CONTROL action=stop)
    void stop(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    /// GET /api/market-sources/{id}/config - 读取行情源配置 (从 ProcessMirror 读取 dzmd_ctp 上报的运行时配置)
    void get_config(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback, int64_t id);
    /// POST /api/market-sources/refresh - 触发 SHM 全量查询 (WS 断连恢复后补拉镜像)
    /// 写 QUERY_ALL 帧, master 响应配置+进程状态, 子进程响应各自配置, 通过 WS 推送
    void refresh(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<ShmWriter> shm_writer_;
    std::shared_ptr<ProcessMirror> process_mirror_;

    nlohmann::json market_source_detail(const MarketSource& s);
    static nlohmann::json market_source_to_json(const MarketSource& s);

    /// 从镜像读取指定 source 对应进程的完整 config JSON (MdConfig 序列化)
    /// 进程不存在或镜像为空时返回 std::nullopt
    std::optional<nlohmann::json> get_config_from_mirror(int64_t source_id) const;

    /// 检查指定 source 对应进程在镜像中是否为 running 状态
    /// 用于 SHM 下发前确认 dzmd_ctp 在线
    /// B-C3: 读 statuses_ (PROCESS_STATUS 先到, 避免误判为离线)
    bool is_process_running_in_mirror(int64_t source_id) const;

    /// op-based 配置下发辅助: 检查 source 存在 + 进程在镜像中 running + write_md_set_config
    /// op_name: MdConfigOp 序列化后的字符串 (如 "AddBroker"), 子进程用 DZ_JSON_ENUM 宏 (magic_enum) 反序列化
    /// params: op 携带的参数 JSON (结构随 op 变化, 由子进程 switch(op) 解析)
    /// 成功: 返回 nullopt (caller 应返回 200 ok=dispatched)
    /// 失败: 返回 HTTP 错误响应 (404 source 不存在 / 503 进程未运行 / 500 SHM 写入失败)
    std::optional<drogon::HttpResponsePtr> dispatch_op(
        int64_t source_id,
        const std::string& log_label,
        const std::string& op_name,
        const nlohmann::json& params);

    /// 下发守卫（dispatch_op 与 set_auto_login 共用）: source 存在 (404) +
    /// 进程在镜像中 running (503) + shm_writer 就绪 (503)
    /// 成功: 返回 nullopt, process_name 输出目标进程名; 失败: 返回 HTTP 错误响应
    std::optional<drogon::HttpResponsePtr> guard_process_dispatch(
        int64_t source_id,
        const std::string& log_label,
        std::string& process_name);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MARKET_SOURCE_CONTROLLER_H_
