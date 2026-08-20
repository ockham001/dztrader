#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <dztrader/shm/writer.h>
#include <dztrader/platform/process.h>

namespace dztrader::webui {

/// 封装 SHM 事件通道写入，提供业务语义化接口
/// 返回值约定分两组：
///   - bool 返回的 write_* (write_md_connect / write_md_disconnect / write_process_control)：
///     true = 已写入事件通道 (契约 rest §1)，调用方必须检查返回值，false 时映射 503
///   - void 返回的 write_* (write_set_process_config / write_query_all / write_md_set_config /
///     write_set_auto_login / write_set_md_shm_config)：fire-and-forget，写入失败由内部
///     catch 记日志，真正失败由 RTN_* 异步推送纠正 (guard 前置拦截 is_ready 保证就绪性)
class ShmWriter {
public:
    /// 按 shm_dir 打开事件通道并自建 writer（测试/独立使用；通道不存在时降级）
    explicit ShmWriter(const std::filesystem::path& shm_dir);

    /// 注入外部共享 writer（生产路径）：与 ControlDomainService 刷新的是同一实例，
    /// 保证 UPDATE_SHM_EVENT_SUBSCRIBER 后写帧能 notify 到新订阅者
    explicit ShmWriter(std::shared_ptr<shm::MultiWriter> event_writer);

    /// 写入 md_connect 控制帧。返回 true = 已写入事件通道（契约 rest：响应 ok 表示已受理，非业务结果）
    bool write_md_connect(const std::string& source);
    /// 写入 md_disconnect 控制帧。返回 true = 已写入事件通道
    bool write_md_disconnect(const std::string& source);

    /// 发送 REQUEST_PROCESS_CONTROL 帧 (115, 无 instance_id)
    /// action: platform::ProcessAction 枚举 (Start/Stop/Remove)
    /// target: 进程名 (如 "dzmd_ctp")
    /// config: 配置 patch, 仅 action=Start 有效 (如 {"display_name": ...}); nullopt 表示不携带
    /// 返回 true = 已写入事件通道
    bool write_process_control(platform::ProcessAction action,
                               const std::string& target,
                               std::optional<nlohmann::json> config = std::nullopt);

    /// 发送 SET_PROCESS_CONFIG 帧 (117, 无 instance_id)
    /// target: 进程名 (如 "dzmd_ctp")
    /// config: 配置 patch (RFC 7386 语义: 出现字段覆盖, 缺失字段保留)
    void write_set_process_config(const std::string& target, const nlohmann::json& config);

    /// 写 QUERY_FULL_SNAPSHOT 帧 (无 instance_id; master 响应配置+进程状态, 子进程响应各自配置)
    void write_query_all();

    /// 写 SET_MD_CONFIG 帧 (op-based 配置下发到 dzmd_ctp)
    /// source: 目标行情源名 (如 "dzmd_ctp")
    /// op_req_json: MdConfigOpReq 序列化后的 JSON ({op, params})
    void write_md_set_config(const std::string& source, const nlohmann::json& op_req_json);

    /// 写 SET_AUTO_LOGIN 帧 (契约 auto-login: 自动登录/登出排程, 定向到目标网关)
    /// source: 目标网关进程名 (如 "dzmd_ctp")
    /// payload: AutoLoginConfig JSON——REST 层为全量提交 {enabled, schedules};
    ///           帧层遵循契约 auto-login 增量 patch 语义（schedules 出现时整体覆盖）
    void write_set_auto_login(const std::string& source, const nlohmann::json& payload);

    /// 写 SET_MD_SHM_CONFIG 帧 (契约 shm: SHM 行情通道配置, 定向到目标行情进程)
    /// source: 目标行情源名 (如 "dzmd_ctp")
    /// payload: ShmConfig 子集 (RFC 7386 递归合并; preload_points 内 key 值 null=删除;
    ///           page_size_mb 网关端跳过——透传无害, 由网关忽略)
    void write_set_md_shm_config(const std::string& source, const nlohmann::json& payload);

    /// 写 SET_EVENT_SHM_CONFIG 帧 (契约 shm: 事件通道 SHM 配置, 无 instance_id,
    /// 定向 master; payload 为 RFC 7386 合并 patch, page_size_mb 由 master 跳过)
    /// 最终状态由 WS event_shm_config 推送 (RTN_EVENT_SHM_CONFIG) 决定
    void write_set_event_shm_config(const nlohmann::json& payload);

    bool is_ready() const { return event_writer_ != nullptr; }

private:
    std::shared_ptr<shm::MultiWriter> event_writer_;
};

} // namespace dztrader::webui
