#include <dztrader/platform/ctp_md_config.h>

#include <algorithm>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>

namespace dztrader::platform {

// ===== 工具函数 =====

namespace {

/// 检查 params 中是否有任何 null 值（递归）
bool has_null(const nlohmann::json& j) {
    if (j.is_null()) return true;
    if (j.is_object()) {
        for (auto& [k, v] : j.items()) {
            if (has_null(v)) return true;
        }
    }
    if (j.is_array()) {
        for (auto& v : j) {
            if (has_null(v)) return true;
        }
    }
    return false;
}

/// 在 brokers 中查找 name
const CtpBrokerEntry* find_broker(const CtpMdConfigData& cfg, const std::string& name) {
    for (const auto& b : cfg.brokers) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

/// 校验一组可选字符串字段：若存在则必须为字符串
std::optional<std::string> check_optional_strings(const nlohmann::json& params,
                                                  std::initializer_list<const char*> fields) {
    for (const char* f : fields) {
        if (params.contains(f) && !params[f].is_string()) {
            return std::string("field must be a string: ") + f;
        }
    }
    return std::nullopt;
}

}  // namespace

// ===== validate_ctp_op_req =====

std::optional<std::string> validate_ctp_op_req(const CtpMdConfigOpReq& req, const CtpMdConfigData& cfg) {
    // 2. params 必须为 object
    if (!req.params.is_object()) {
        return "params must be a JSON object";
    }
    // 3. 任何位置 null 为校验失败
    if (has_null(req.params)) {
        return "params contains null";
    }

    switch (req.op) {
        case CtpMdConfigOp::AddBroker: {
            if (!req.params.contains("name") || !req.params["name"].is_string()) {
                return "AddBroker: name is required and must be a string";
            }
            auto name = req.params["name"].get<std::string>();
            if (name.empty()) {
                return "AddBroker: name must not be empty";
            }
            if (auto err = check_optional_strings(req.params,
                                                  {"broker_id", "user_id", "password", "product_info"})) {
                return err;
            }
            if (find_broker(cfg, name) != nullptr) {
                return "AddBroker: broker already exists: " + name;
            }
            return std::nullopt;
        }
        case CtpMdConfigOp::RemoveBroker: {
            if (!req.params.contains("name") || !req.params["name"].is_string()) {
                return "RemoveBroker: name is required and must be a string";
            }
            auto name = req.params["name"].get<std::string>();
            if (name.empty()) {
                return "RemoveBroker: name must not be empty";
            }
            if (find_broker(cfg, name) == nullptr) {
                return "RemoveBroker: broker not found: " + name;
            }
            return std::nullopt;
        }
        case CtpMdConfigOp::UpdateBroker: {
            if (!req.params.contains("name") || !req.params["name"].is_string()) {
                return "UpdateBroker: name is required and must be a string";
            }
            auto name = req.params["name"].get<std::string>();
            if (name.empty()) {
                return "UpdateBroker: name must not be empty";
            }
            if (auto err = check_optional_strings(req.params,
                                                  {"broker_id", "user_id", "password", "product_info"})) {
                return err;
            }
            if (find_broker(cfg, name) == nullptr) {
                return "UpdateBroker: broker not found: " + name;
            }
            return std::nullopt;
        }
        case CtpMdConfigOp::SetFrontends: {
            if (!req.params.contains("name") || !req.params["name"].is_string()) {
                return "SetFrontends: name is required and must be a string";
            }
            auto broker_name = req.params["name"].get<std::string>();
            if (broker_name.empty()) {
                return "SetFrontends: name must not be empty";
            }
            if (find_broker(cfg, broker_name) == nullptr) {
                return "SetFrontends: broker not found: " + broker_name;
            }
            if (!req.params.contains("frontends") || !req.params["frontends"].is_array()) {
                return "SetFrontends: frontends is required and must be an array";
            }
            for (const auto& f : req.params["frontends"]) {
                if (!f.is_object() || !f.contains("address") || !f["address"].is_string() ||
                    f["address"].get<std::string>().empty()) {
                    return "SetFrontends: each frontend must have a non-empty address";
                }
                if (f.contains("label") && !f["label"].is_string()) {
                    return "SetFrontends: frontend label must be a string";
                }
                if (f.contains("enabled") && !f["enabled"].is_boolean()) {
                    return "SetFrontends: frontend enabled must be a boolean";
                }
            }
            return std::nullopt;
        }
        case CtpMdConfigOp::SetCurrentBroker: {
            // name 可选：缺失或空 = 清空选中
            if (req.params.contains("name")) {
                if (!req.params["name"].is_string()) {
                    return "SetCurrentBroker: name must be a string";
                }
                auto name = req.params["name"].get<std::string>();
                if (!name.empty() && find_broker(cfg, name) == nullptr) {
                    return "SetCurrentBroker: broker not found: " + name;
                }
            }
            return std::nullopt;
        }
        case CtpMdConfigOp::SetSubscribeParams: {
            if (req.params.contains("subscribe_batch_size")) {
                if (!req.params["subscribe_batch_size"].is_number_integer() ||
                    req.params["subscribe_batch_size"].get<int>() <= 0) {
                    return "subscribe_batch_size must be a positive integer";
                }
            }
            if (req.params.contains("subscribe_batch_delay_ms")) {
                if (!req.params["subscribe_batch_delay_ms"].is_number_integer() ||
                    req.params["subscribe_batch_delay_ms"].get<int>() < 0) {
                    return "subscribe_batch_delay_ms must be a non-negative integer";
                }
            }
            if (req.params.contains("sub_check_interval_ms")) {
                if (!req.params["sub_check_interval_ms"].is_number_integer() ||
                    req.params["sub_check_interval_ms"].get<int>() <= 0) {
                    return "sub_check_interval_ms must be a positive integer";
                }
            }
            if (req.params.contains("sub_max_retry")) {
                if (!req.params["sub_max_retry"].is_number_integer() ||
                    req.params["sub_max_retry"].get<int>() < 0) {
                    return "sub_max_retry must be a non-negative integer";
                }
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// ===== apply_ctp_config_op =====

void apply_ctp_config_op(CtpMdConfigData& cfg, const CtpMdConfigOpReq& req) {
    switch (req.op) {
        case CtpMdConfigOp::AddBroker: {
            CtpBrokerEntry b;
            b.name = req.params.value("name", "");
            b.broker_id = req.params.value("broker_id", "");
            b.user_id = req.params.value("user_id", "");
            b.password = req.params.value("password", "");
            b.product_info = req.params.value("product_info", "");
            cfg.brokers.push_back(std::move(b));
            break;
        }
        case CtpMdConfigOp::RemoveBroker: {
            auto name = req.params.value("name", "");
            auto& brokers = cfg.brokers;
            brokers.erase(std::remove_if(brokers.begin(), brokers.end(),
                                         [&](const CtpBrokerEntry& b) { return b.name == name; }),
                          brokers.end());
            // 删除后若 current_broker_name 指向被删经纪商，自动清空
            if (cfg.current_broker_name == name) {
                cfg.current_broker_name.clear();
            }
            break;
        }
        case CtpMdConfigOp::UpdateBroker: {
            auto name = req.params.value("name", "");
            for (auto& b : cfg.brokers) {
                if (b.name == name) {
                    b.broker_id = req.params.value("broker_id", b.broker_id);
                    b.user_id = req.params.value("user_id", b.user_id);
                    b.product_info = req.params.value("product_info", b.product_info);
                    auto pwd = req.params.value("password", "");
                    if (pwd != "****" && !pwd.empty()) {
                        b.password = pwd;
                    }
                    break;
                }
            }
            break;
        }
        case CtpMdConfigOp::SetFrontends: {
            auto broker_name = req.params.value("name", "");
            if (!req.params.contains("frontends") || !req.params["frontends"].is_array()) {
                break;
            }
            for (auto& b : cfg.brokers) {
                if (b.name == broker_name) {
                    b.frontends = req.params["frontends"].get<std::vector<CtpBrokerFrontend>>();
                    break;
                }
            }
            break;
        }
        case CtpMdConfigOp::SetCurrentBroker: {
            cfg.current_broker_name = req.params.value("name", "");
            break;
        }
        case CtpMdConfigOp::SetSubscribeParams: {
            if (req.params.contains("subscribe_batch_size")) {
                cfg.subscribe_batch_size = req.params["subscribe_batch_size"].get<int>();
            }
            if (req.params.contains("subscribe_batch_delay_ms")) {
                cfg.subscribe_batch_delay_ms = req.params["subscribe_batch_delay_ms"].get<int>();
            }
            if (req.params.contains("sub_check_interval_ms")) {
                cfg.sub_check_interval_ms = req.params["sub_check_interval_ms"].get<int>();
            }
            if (req.params.contains("sub_max_retry")) {
                cfg.sub_max_retry = req.params["sub_max_retry"].get<int>();
            }
            break;
        }
    }
}

// ===== validate_ctp_config =====

std::optional<std::string> validate_ctp_config(const CtpMdConfigData& cfg) {
    // broker name 非空、唯一
    for (const auto& b : cfg.brokers) {
        if (b.name.empty()) {
            return "broker name must not be empty";
        }
    }
    for (size_t i = 0; i < cfg.brokers.size(); ++i) {
        for (size_t j = i + 1; j < cfg.brokers.size(); ++j) {
            if (cfg.brokers[i].name == cfg.brokers[j].name) {
                return "duplicate broker name: " + cfg.brokers[i].name;
            }
        }
    }
    // current_broker_name 为空或指向已存在 broker
    if (!cfg.current_broker_name.empty()) {
        if (find_broker(cfg, cfg.current_broker_name) == nullptr) {
            return "current_broker_name not found: " + cfg.current_broker_name;
        }
    }
    // 每个 broker 的 frontends 中 address 非空
    for (const auto& b : cfg.brokers) {
        for (const auto& f : b.frontends) {
            if (f.address.empty()) {
                return "frontend address must not be empty for broker: " + b.name;
            }
        }
    }
    // 订阅参数值约束
    if (cfg.subscribe_batch_size <= 0) {
        return "subscribe_batch_size must be > 0";
    }
    if (cfg.subscribe_batch_delay_ms < 0) {
        return "subscribe_batch_delay_ms must be >= 0";
    }
    if (cfg.sub_check_interval_ms <= 0) {
        return "sub_check_interval_ms must be > 0";
    }
    if (cfg.sub_max_retry < 0) {
        return "sub_max_retry must be >= 0";
    }
    return std::nullopt;
}

// ===== ctp_config_to_safe_json =====

nlohmann::json ctp_config_to_safe_json(const CtpMdConfigData& cfg) {
    nlohmann::json j = cfg;
    for (auto& b : j["brokers"]) {
        b["password"] = "****";
    }
    return j;
}

// ===== CtpMdConfig 类 =====

CtpMdConfig::CtpMdConfig(std::string instance_id,
                         std::filesystem::path cfg_path,
                         shm::MultiWriter& event_writer,
                         nlohmann::json::json_pointer section)
    : instance_id_(std::move(instance_id)),
      cfg_path_(std::move(cfg_path)),
      event_writer_(event_writer),
      section_(std::move(section)),
      cfg_() {}  // 默认构造，load() 前也有合理默认值

CtpMdConfigData CtpMdConfig::load_cfg_from_file(bool& degraded) const {
    nlohmann::json raw;
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            nlohmann::json full;
            ifs >> full;
            if (section_.empty()) {
                raw = full.is_object() ? full : nlohmann::json::object();
                if (!full.is_object()) degraded = true;
            } else if (full.is_object() && full.contains(section_)) {
                raw = full.at(section_);
                if (!raw.is_object()) degraded = true;
            }
        } else {
            degraded = true;
        }
    }
    // 反序列化为 CtpMdConfigData，校验后返回
    CtpMdConfigData result{};  // 默认值
    if (raw.is_object()) {
        try {
            CtpMdConfigData candidate = raw.get<CtpMdConfigData>();
            // 字段级自愈：current_broker_name 指向不存在的经纪商时清空
            bool field_degraded = false;
            if (!candidate.current_broker_name.empty()) {
                bool found = false;
                for (const auto& b : candidate.brokers) {
                    if (b.name == candidate.current_broker_name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    candidate.current_broker_name.clear();
                    field_degraded = true;
                }
            }
            if (auto err = validate_ctp_config(candidate)) {
                field_degraded = true;
            } else {
                result = std::move(candidate);
            }
            degraded = degraded || field_degraded;
        } catch (const std::exception&) {
            degraded = true;
        }
    }
    return result;
}

void CtpMdConfig::save_cfg_to_file(const CtpMdConfigData& cfg) const {
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            auto backup_corrupt = [this]() {
                std::error_code ec;
                auto backup = cfg_path_;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                                            std::chrono::system_clock::now()));
                std::filesystem::rename(cfg_path_, backup, ec);
            };
            try {
                ifs >> full;
            } catch (const std::exception&) {
                full = nlohmann::json::object();
                backup_corrupt();
            }
            if (!section_.empty() && !full.is_object()) {
                full = nlohmann::json::object();
                backup_corrupt();
            }
        }
    }
    if (section_.empty()) {
        full = nlohmann::json(cfg);
    } else {
        full[section_] = cfg;
    }
    auto tmp = cfg_path_;
    tmp += ".tmp";
    try {
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("open failed | path=" + tmp.string());
            ofs << full.dump(2);
            ofs.flush();
            if (!ofs) throw std::runtime_error("write failed | path=" + tmp.string());
            ofs.close();
            if (!ofs) throw std::runtime_error("close failed | path=" + tmp.string());
        }
        std::error_code ec;
        std::filesystem::rename(tmp, cfg_path_, ec);
        if (ec) {
            throw std::runtime_error("rename failed | from=" + tmp.string() +
                                     " to=" + cfg_path_.string());
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

void CtpMdConfig::load() {
    bool degraded = false;
    std::string reason;
    try {
        cfg_ = load_cfg_from_file(degraded);
    } catch (const std::exception& e) {
        degraded = true;
        reason = e.what();
        cfg_ = CtpMdConfigData{};
    }
    if (degraded) {
        if (reason.empty()) reason = "invalid values, using defaults";
        spdlog::error("ctp md config load degraded, using defaults | error=\"{}\"", reason);
        try {
            save_cfg_to_file(cfg_);
        } catch (const std::exception& se) {
            spdlog::error("ctp md config repair save failed | error=\"{}\"", se.what());
        }
    }
}

void CtpMdConfig::set_md_config(const CtpMdConfigOpReq& req) {
    // 1. validate_ctp_op_req（payload 级校验）
    if (auto err = validate_ctp_op_req(req, cfg_)) {
        throw std::runtime_error(*err);
    }
    // 2. 副本上 apply
    CtpMdConfigData copy = cfg_;
    apply_ctp_config_op(copy, req);
    // 3. validate_ctp_config（完整性校验）
    if (auto err = validate_ctp_config(copy)) {
        throw std::runtime_error(*err);
    }
    // 4. 持久化
    save_cfg_to_file(copy);
    // 5. 提交
    cfg_ = std::move(copy);
}

void CtpMdConfig::rtn_md_config() {
    write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_MD_CONFIG,
                            instance_id_, ctp_config_to_safe_json(cfg_));
}

}  // namespace dztrader::platform