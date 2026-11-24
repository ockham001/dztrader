#include "shm_writer.h"
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/sysmon.h>  // SYSMON_INSTANCE_NAME（契约 sysmon §5 的固定实例名）
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace dztrader::webui {

ShmWriter::ShmWriter(const std::filesystem::path& shm_dir) {
    try {
        auto meta = std::make_shared<shm::ChannelMeta>(
            shm::ChannelMeta::open_only(shm::channel_name("dzevent"), shm_dir));
        event_writer_ = std::make_shared<shm::MultiWriter>(shm::MultiWriter::create(meta, "dzweb"));
        spdlog::info("shm writer initialized | channel={}", shm::channel_name("dzevent"));
    } catch (const std::exception& e) {
        spdlog::warn("shm writer init failed | channel={} error=\"{}\"",
                     shm::channel_name("dzevent"), e.what());
    }
}

ShmWriter::ShmWriter(std::shared_ptr<shm::MultiWriter> event_writer)
    : event_writer_(std::move(event_writer)) {}

bool ShmWriter::write_td_modify_config(const std::string& gateway,
                                       const nlohmann::json& op_req_json) {
    if (!event_writer_ || gateway.empty()) {
        return false;
    }
    // TD_REQ_MODIFY_CONFIG 定向到目标交易网关 (契约 td-config §3): payload 为
    // {op, params}，dzweb 透传（不解析柜台特有字段）。与 write_md_set_config 不同，
    // 本方法返回真实写入结果——写端点据此映射 503（契约 rest §2.9.7 语义：
    // 响应 ok 只表示已写入事件通道，业务结果由 RTN_TD_CONFIG 表达）。
    const bool ok = platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_TD_REQ_MODIFY_CONFIG,
                                                      gateway, op_req_json);
    if (ok) {
        spdlog::debug("td modify config written | gateway={}", gateway);
    } else {
        spdlog::error("write td modify config failed | gateway={}", gateway);
    }
    return ok;
}

bool ShmWriter::write_md_connect(const std::string& source) {
    if (!event_writer_ || source.empty()) {
        return false;
    }
    // platform 层吞异常并返回真实写入结果（写 + notify 成功 = true）
    return platform::write_ext_inst_raw(*event_writer_, DZ_FRAME_REQUEST_MD_CONNECT, source);
}

bool ShmWriter::write_md_disconnect(const std::string& source) {
    if (!event_writer_ || source.empty()) {
        return false;
    }
    return platform::write_ext_inst_raw(*event_writer_, DZ_FRAME_REQUEST_MD_DISCONNECT, source);
}

bool ShmWriter::write_td_connect(const std::string& gateway, const std::string& account_id) {
    if (!event_writer_ || gateway.empty() || account_id.empty()) {
        return false;
    }
    // 契约 td-connection：定向到网关进程（instance_id = 网关名），账户在 payload。
    const bool ok = platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_TD_CONNECT, gateway,
                                                      nlohmann::json{{"account_id", account_id}});
    if (ok) {
        spdlog::debug("td connect written | gateway={} account={}", gateway, account_id);
    } else {
        spdlog::error("write td connect failed | gateway={} account={}", gateway, account_id);
    }
    return ok;
}

bool ShmWriter::write_td_disconnect(const std::string& gateway, const std::string& account_id) {
    if (!event_writer_ || gateway.empty() || account_id.empty()) {
        return false;
    }
    const bool ok =
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_TD_DISCONNECT, gateway,
                                          nlohmann::json{{"account_id", account_id}});
    if (ok) {
        spdlog::debug("td disconnect written | gateway={} account={}", gateway, account_id);
    } else {
        spdlog::error("write td disconnect failed | gateway={} account={}", gateway, account_id);
    }
    return ok;
}

bool ShmWriter::write_td_refresh_req(const std::string& account_id, int64_t request_id) {
    if (!event_writer_ || account_id.empty()) {
        return false;
    }
    // 契约 td-refresh：JSON ext 帧（无 instance_id，事件通道广播），td 侧按 payload
    // account_id 归属过滤。
    const bool ok = platform::write_ext_json(
        *event_writer_, DZ_FRAME_TD_REFRESH_REQ,
        nlohmann::json{{"account_id", account_id}, {"request_id", request_id}});
    if (ok) {
        spdlog::debug("td refresh req written | account={} request_id={}", account_id, request_id);
    } else {
        spdlog::error("write td refresh req failed | account={}", account_id);
    }
    return ok;
}

bool ShmWriter::write_process_control(platform::ProcessAction action,
                                      const std::string& target,
                                      std::optional<nlohmann::json> config) {
    if (!event_writer_ || target.empty()) {
        return false;
    }
    const platform::ProcessControlReq req{
        .action = action, .target = target, .config = std::move(config)};
    const bool ok = platform::write_ext_json(*event_writer_, DZ_FRAME_REQUEST_PROCESS_CONTROL, req);
    if (ok) {
        spdlog::info("process control written | action={} target={}", magic_enum::enum_name(action),
                     target);
    } else {
        spdlog::warn("write process control failed | action={} target={}",
                     magic_enum::enum_name(action), target);
    }
    return ok;
}

void ShmWriter::write_set_process_config(const std::string& target, const nlohmann::json& config) {
    if (!event_writer_ || target.empty()) {
        return;
    }
    try {
        const platform::SetProcessConfigReq req{.target = target, .config = config};
        platform::write_ext_json(*event_writer_, DZ_FRAME_SET_PROCESS_CONFIG, req);
        spdlog::info("set process config written | target={}", target);
    } catch (const std::exception& e) {
        spdlog::warn("write set process config failed | error=\"{}\"", e.what());
    }
}

void ShmWriter::write_query_all() {
    if (!event_writer_) {
        return;
    }
    try {
        // QUERY_FULL_SNAPSHOT (无 instance_id): master 响应配置+进程状态, 子进程响应各自配置
        platform::write_ext_raw(*event_writer_, DZ_FRAME_QUERY_FULL_SNAPSHOT);
    } catch (const std::exception& e) {
        spdlog::warn("write query all failed | error=\"{}\"", e.what());
    }
}

void ShmWriter::write_md_set_config(const std::string& source, const nlohmann::json& op_req_json) {
    if (!event_writer_) {
        spdlog::error("write md set config failed: event_writer null | source={}", source);
        return;
    }
    if (source.empty()) {
        spdlog::error("write md set config failed: source empty");
        return;
    }
    try {
        // SET_MD_CONFIG 的 payload 是 MdConfigOpReq 序列化 JSON ({op, params})
        // dzmd_ctp 端用 decode_ext_inst_json<MdConfigOpReq> 解析
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_MD_CONFIG, source,
                                          op_req_json);
        spdlog::debug("md set config written | source={}", source);
    } catch (const std::exception& e) {
        spdlog::error("write md set config failed | source={} error=\"{}\"", source, e.what());
    }
}

void ShmWriter::write_set_auto_login(const std::string& source, const nlohmann::json& payload) {
    if (!event_writer_) {
        spdlog::error("write set auto login failed: event_writer null | source={}", source);
        return;
    }
    if (source.empty()) {
        spdlog::error("write set auto login failed: source empty");
        return;
    }
    try {
        // SET_AUTO_LOGIN 定向到目标网关 (契约 auto-login): payload 为 AutoLoginConfig 增量 patch
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_AUTO_LOGIN, source, payload);
        spdlog::info("set auto login written | source={}", source);
    } catch (const std::exception& e) {
        spdlog::error("write set auto login failed | source={} error=\"{}\"", source, e.what());
    }
}

void ShmWriter::write_set_md_shm_config(const std::string& source, const nlohmann::json& payload) {
    if (!event_writer_) {
        spdlog::error("write set md shm config failed: event_writer null | source={}", source);
        return;
    }
    if (source.empty()) {
        spdlog::error("write set md shm config failed: source empty");
        return;
    }
    try {
        // SET_MD_SHM_CONFIG 定向到目标行情进程 (契约 shm): payload 为 ShmConfig 增量 patch
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_MD_SHM_CONFIG, source,
                                          payload);
        spdlog::info("set md shm config written | source={}", source);
    } catch (const std::exception& e) {
        spdlog::error("write set md shm config failed | source={} error=\"{}\"", source, e.what());
    }
}

void ShmWriter::write_set_event_shm_config(const nlohmann::json& payload) {
    if (!event_writer_) {
        spdlog::error("write set event shm config failed: event_writer null");
        return;
    }
    // 无 instance_id 帧: platform::write_ext_json 内部不抛异常, 返回 bool 标识写入结果
    // (对齐 write_set_md_shm_config 的日志模式)
    const bool ok =
        platform::write_ext_json(*event_writer_, DZ_FRAME_SET_EVENT_SHM_CONFIG, payload);
    if (ok) {
        spdlog::info("set event shm config written");
    } else {
        spdlog::error("write set event shm config failed");
    }
}

void ShmWriter::write_set_sysmon_config(const nlohmann::json& payload) {
    if (!event_writer_) {
        spdlog::error("write set sysmon config failed: event_writer null");
        return;
    }
    // SET_SYSMON_CONFIG 定向到内部进程 dzsysmon (契约 sysmon §3; instance_id 即进程名)。
    // 成功只表示已写入事件通道, 生效由 RTN_SYSMON_CONFIG 全量回推并经 WS sysmon_config 表达。
    const bool ok = platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_SYSMON_CONFIG,
                                                      platform::SYSMON_INSTANCE_NAME, payload);
    if (ok) {
        spdlog::info("set sysmon config written");
    } else {
        spdlog::error("write set sysmon config failed");
    }
}

void ShmWriter::write_query_sysmon_clock() {
    if (!event_writer_) {
        spdlog::error("write query sysmon clock failed: event_writer null");
        return;
    }
    // QUERY_SYSMON_CLOCK 是形态 3 单向下行动作帧 (契约 sysmon §1.1): 空 payload、无 RTN,
    // 语义与 FLUSH_LOG 同类。结果不单独回帧, 由随后的 RTN_SYSMON_SNAPSHOT 广播体现。
    const bool ok = platform::write_ext_inst_raw(*event_writer_, DZ_FRAME_QUERY_SYSMON_CLOCK,
                                                 platform::SYSMON_INSTANCE_NAME);
    if (ok) {
        spdlog::info("query sysmon clock written");
    } else {
        spdlog::error("write query sysmon clock failed");
    }
}

}  // namespace dztrader::webui
