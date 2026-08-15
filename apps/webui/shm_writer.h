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
/// 所有 write_* 方法均为 fire-and-forget (与其他库一致):
///   - SHM 写入失败 (如通道满) 由内部 catch 记录 WARN 日志, 不抛出
///   - 调用方不应检查返回值, 真正失败由 RTN_* 异步推送纠正
class ShmWriter {
public:
    /// 按 shm_dir 打开事件通道并自建 writer（测试/独立使用；通道不存在时降级）
    explicit ShmWriter(const std::filesystem::path& shm_dir);

    /// 注入外部共享 writer（生产路径）：与 ControlDomainService 刷新的是同一实例，
    /// 保证 UPDATE_SHM_EVENT_SUBSCRIBER 后写帧能 notify 到新订阅者
    explicit ShmWriter(std::shared_ptr<shm::MultiWriter> event_writer);

    /// 写入 md_connect 控制帧。返回 true = 已写入事件通道（契约 11：响应 ok 表示已受理，非业务结果）
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

    /// 写 SET_AUTO_LOGIN 帧 (契约 04: 自动登录/登出排程, 定向到目标网关)
    /// source: 目标网关进程名 (如 "dzmd_ctp")
    /// payload: AutoLoginConfig JSON——REST 层为全量提交 {enabled, schedules};
    ///           帧层遵循契约 04 增量 patch 语义（schedules 出现时整体覆盖）
    void write_set_auto_login(const std::string& source, const nlohmann::json& payload);

    bool is_ready() const { return event_writer_ != nullptr; }

private:
    std::shared_ptr<shm::MultiWriter> event_writer_;
};

} // namespace dztrader::webui
