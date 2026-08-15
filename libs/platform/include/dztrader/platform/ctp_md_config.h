#ifndef DZTRADER_PLATFORM_CTP_MD_CONFIG_H_
#define DZTRADER_PLATFORM_CTP_MD_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <dztrader/core/json_enum.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

// ===== 枚举 =====

/// CTP 行情网关配置操作类型（契约 08 CtpMdConfigOp 表）
enum class CtpMdConfigOp {
    AddBroker,
    RemoveBroker,
    UpdateBroker,
    SetFrontends,
    SetCurrentBroker,
    SetSubscribeParams,
};

DZ_JSON_ENUM(CtpMdConfigOp)

/// 判断 op 是否涉及连接参数变更（非 Idle 时拒绝）。
/// switch 覆盖所有枚举值，-Wswitch 保证新增 op 时编译器告警。
inline bool is_ctp_connection_op(CtpMdConfigOp op) {
    switch (op) {
        case CtpMdConfigOp::RemoveBroker:
        case CtpMdConfigOp::UpdateBroker:
        case CtpMdConfigOp::SetFrontends:
        case CtpMdConfigOp::SetCurrentBroker:
            return true;
        case CtpMdConfigOp::AddBroker:
        case CtpMdConfigOp::SetSubscribeParams:
            return false;
    }
    return false;
}

// ===== 数据结构 =====

/// SET payload 结构（契约 08 DZ_FRAME_SET_MD_CONFIG）
struct CtpMdConfigOpReq {
    CtpMdConfigOp op;
    nlohmann::json params;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpMdConfigOpReq, op, params)
};

/// 前置地址条目（契约 08 CtpBrokerFrontend）
struct CtpBrokerFrontend {
    std::string address;
    std::string label;
    bool enabled = true;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpBrokerFrontend, address, label, enabled)
};

/// 经纪商条目（契约 08 CtpBrokerEntry）
struct CtpBrokerEntry {
    std::string name;
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string product_info;
    std::vector<CtpBrokerFrontend> frontends;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        CtpBrokerEntry, name, broker_id, user_id, password, product_info, frontends)
};

/// MD 网关配置（契约 08 CtpMdConfigData，RTN payload 用）
struct CtpMdConfigData {
    std::vector<CtpBrokerEntry> brokers;
    std::string current_broker_name;
    int subscribe_batch_size = 1000;
    int subscribe_batch_delay_ms = 1000;
    int sub_check_interval_ms = 3000;
    int sub_max_retry = 3;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CtpMdConfigData,
                                                brokers,
                                                current_broker_name,
                                                subscribe_batch_size,
                                                subscribe_batch_delay_ms,
                                                sub_check_interval_ms,
                                                sub_max_retry)
};

// ===== 纯函数声明 =====

/// SET payload 级校验：检查 op/params/null/必填/目标存在性（契约 08 校验失败场景）
std::optional<std::string> validate_ctp_op_req(const CtpMdConfigOpReq& req, const CtpMdConfigData& cfg);

/// op 应用：switch(req.op) 修改 cfg，无副作用无 I/O
/// 调用前应已通过 validate_ctp_op_req 校验
void apply_ctp_config_op(CtpMdConfigData& cfg, const CtpMdConfigOpReq& req);

/// 集中校验：校验 CtpMdConfigData 完整性（字段值约束/一致性）
std::optional<std::string> validate_ctp_config(const CtpMdConfigData& cfg);

/// 脱敏 JSON：password -> "****"
nlohmann::json ctp_config_to_safe_json(const CtpMdConfigData& cfg);

// ===== CtpMdConfig 配置类 =====

/// CTP 行情网关配置类，封装 SET_MD_CONFIG / RTN_MD_CONFIG 两帧的处理。
///
/// 职责边界（与 LogConfig/AutoLoginConfig 一致）：
/// - 类负责配置的加载/修改/持久化/发送 RTN
/// - rtn 的时机和次数由外部控制--类内部不自动调 rtn_md_config()
/// - SET 失败的日志 + NOTIFY_UI 由外部 catch 块处理
class CtpMdConfig {
public:
    CtpMdConfig(std::string instance_id,
                std::filesystem::path cfg_path,
                shm::MultiWriter& event_writer,
                nlohmann::json::json_pointer section = nlohmann::json::json_pointer("/md"));

    CtpMdConfig(const CtpMdConfig&) = delete;
    CtpMdConfig& operator=(const CtpMdConfig&) = delete;
    CtpMdConfig(CtpMdConfig&&) = delete;
    CtpMdConfig& operator=(CtpMdConfig&&) = delete;
    ~CtpMdConfig() = default;

    /// 启动时加载配置。不抛、自愈、不发 RTN。
    void load();

    /// 接收处理 SET_MD_CONFIG。强保证（失败 cfg_ 不变），不发 RTN。
    /// @throws std::runtime_error 校验失败/持久化失败
    void set_md_config(const CtpMdConfigOpReq& req);

    /// 发送 RTN_MD_CONFIG 帧（全量脱敏，不抛）。
    void rtn_md_config();

    /// 只读访问当前配置（供 MdApi 运行时读取）
    const CtpMdConfigData& config() const noexcept { return cfg_; }

private:
    std::string instance_id_;
    std::filesystem::path cfg_path_;
    shm::MultiWriter& event_writer_;
    nlohmann::json::json_pointer section_;
    CtpMdConfigData cfg_;

    // 持久化：load-modify-save 保留其他 section
    CtpMdConfigData load_cfg_from_file(bool& degraded) const;
    void save_cfg_to_file(const CtpMdConfigData& cfg) const;
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_CTP_MD_CONFIG_H_