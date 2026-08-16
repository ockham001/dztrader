#include "shm_writer.h"
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/platform/frame_codec.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace dztrader::webui {

ShmWriter::ShmWriter(const std::filesystem::path& shm_dir) {
    try {
        auto meta = std::make_shared<shm::ChannelMeta>(
            shm::ChannelMeta::open_only(shm::channel_name("dzevent"), shm_dir));
        event_writer_ = std::make_shared<shm::MultiWriter>(
            shm::MultiWriter::create(meta, "dzweb"));
        spdlog::info("shm writer initialized | channel={}", shm::channel_name("dzevent"));
    } catch (const std::exception& e) {
        spdlog::warn("shm writer init failed | channel={} error=\"{}\"", shm::channel_name("dzevent"), e.what());
    }
}

ShmWriter::ShmWriter(std::shared_ptr<shm::MultiWriter> event_writer)
    : event_writer_(std::move(event_writer)) {}

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

bool ShmWriter::write_process_control(platform::ProcessAction action,
                                      const std::string& target,
                                      std::optional<nlohmann::json> config) {
    if (!event_writer_ || target.empty()) {
        return false;
    }
    const platform::ProcessControlReq req{
        .action = action, .target = target, .config = std::move(config)};
    const bool ok =
        platform::write_ext_json(*event_writer_, DZ_FRAME_REQUEST_PROCESS_CONTROL, req);
    if (ok) {
        spdlog::info("process control written | action={} target={}",
                     magic_enum::enum_name(action), target);
    } else {
        spdlog::warn("write process control failed | action={} target={}",
                     magic_enum::enum_name(action), target);
    }
    return ok;
}

void ShmWriter::write_set_process_config(const std::string& target,
                                         const nlohmann::json& config) {
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
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_MD_CONFIG, source, op_req_json);
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
        platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_SET_MD_SHM_CONFIG, source, payload);
        spdlog::info("set md shm config written | source={}", source);
    } catch (const std::exception& e) {
        spdlog::error("write set md shm config failed | source={} error=\"{}\"", source, e.what());
    }
}

} // namespace dztrader::webui
