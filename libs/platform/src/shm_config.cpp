#include <dztrader/platform/shm_config.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace dztrader::platform::detail {

namespace {
// ===== 内部工具函数(仅本 TU 使用) =====

bool is_integer_value(const nlohmann::json& j) {
    // 契约 shm:整数字段不区分 int32/int64/uint64,JSON 数字只要值在目标类型范围内即可
    // 故 5 与 5.0 均接受,5.5 拒绝
    if (j.is_number_integer()) {
        return true;
    }
    if (j.is_number_float()) {
        const auto d = j.get<double>();
        // 超出 int64 可安全转换范围的整数 double 转 int64_t 是 UB。
        // 允许范围 [-2^63, 2^63):下界 -2^63 可精确表示;double(INT64_MAX) 舍入为
        // 2^63,故上界取 2^63 且必须严格小于
        return std::isfinite(d) && d == std::floor(d) &&
               d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
               d < std::ldexp(1.0, 63);
    }
    return false;
}

bool is_hhmm(const std::string& s) {
    // "HH:MM",00:00-23:59
    if (s.size() != 5 || s[2] != ':') {
        return false;
    }
    for (const char c : {s[0], s[1], s[3], s[4]}) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    const int hh = ((s[0] - '0') * 10) + (s[1] - '0');
    const int mm = ((s[3] - '0') * 10) + (s[4] - '0');
    return hh <= 23 && mm <= 59;
}

}  // namespace

ShmConfigBase::ShmConfigBase(nlohmann::json default_cfg,
                             std::filesystem::path cfg_path,
                             shm::MultiWriter& writer,
                             nlohmann::json::json_pointer section)
    : writer_(writer),
      default_cfg_(std::move(default_cfg)),
      cfg_path_(std::move(cfg_path)),
      section_(std::move(section)),
      cfg_(default_cfg_) {  // 构造后即使不调 load() 也有合理默认值
}

std::vector<ShmConfigBase::PreloadPointView> ShmConfigBase::preload_points() const {
    std::vector<PreloadPointView> result;
    for (auto it = cfg_["preload_points"].begin(); it != cfg_["preload_points"].end(); ++it) {
        result.push_back({.time = it.key(),
                          .pages = it.value()["pages"].get<uint32_t>(),
                          .bytes = it.value()["bytes"].get<uint64_t>()});
    }
    return result;
}

std::optional<std::string> ShmConfigBase::validate(const nlohmann::json& cfg) {
    auto check_int = [](const nlohmann::json& c, const char* key, int64_t lo, int64_t hi) -> bool {
        if (!c.contains(key) || !is_integer_value(c[key])) {
            return false;
        }
        const int64_t v = get_int_value(c[key]);
        return v >= lo && v <= hi;
    };
    // page_size_mb:契约未给上界,只要求 >= 1(0/负数无意义)
    if (!cfg.contains("page_size_mb") || !is_integer_value(cfg["page_size_mb"])) {
        return "invalid page_size_mb";
    }
    if (get_int_value(cfg["page_size_mb"]) < 1) {
        return "page_size_mb must be > 0";
    }
    if (!check_int(cfg, "check_interval_min", 0, 1440)) {
        return "check_interval_min out of range [0,1440]";
    }
    if (!check_int(cfg, "check_pages", 0, 8)) {
        return "check_pages out of range [0,8]";
    }
    if (!check_int(cfg, "check_bytes", 0, SHM_BYTES_MAX)) {
        return "check_bytes out of range";
    }
    if (!cfg.contains("preload_points") || !cfg["preload_points"].is_object()) {
        return "invalid preload_points";
    }
    for (auto it = cfg["preload_points"].begin(); it != cfg["preload_points"].end(); ++it) {
        if (!is_hhmm(it.key())) {
            return "invalid preload_points key format: " + it.key();
        }
        const nlohmann::json& v = it.value();
        if (!v.is_object()) {
            return "invalid preload_points value: " + it.key();
        }
        if (!check_int(v, "pages", 0, 8)) {
            return "pages out of range: " + it.key();
        }
        if (!check_int(v, "bytes", 0, SHM_BYTES_MAX)) {
            return "bytes out of range: " + it.key();
        }
    }
    return std::nullopt;
}

nlohmann::json ShmConfigBase::merge_patch(const nlohmann::json& patch) const {
    // 契约 shm:payload 非 object 为校验失败
    if (!patch.is_object()) {
        throw std::runtime_error("shm config patch must be a JSON object");
    }
    nlohmann::json merged = cfg_;

    // page_size_mb 不可变:SET 中完全跳过(不解析、不校验、不报错),即使为 null 也跳过
    // (契约 shm)

    auto apply_int_field = [&patch](nlohmann::json& m, const char* key) {
        if (!patch.contains(key)) {
            return;  // 缺失=不修改(RFC 7386)
        }
        const nlohmann::json& v = patch[key];
        // 契约 shm:除 preload_points 内部 key 的 value 外,null 均校验失败
        if (v.is_null()) {
            throw std::runtime_error(std::string(key) + " must not be null");
        }
        if (!is_integer_value(v)) {
            throw std::runtime_error(std::string(key) + " must be an integer");
        }
        m[key] = v;  // 范围由 validate 检查
    };
    apply_int_field(merged, "check_interval_min");
    apply_int_field(merged, "check_pages");
    apply_int_field(merged, "check_bytes");

    if (patch.contains("preload_points")) {
        const nlohmann::json& pp = patch["preload_points"];
        // preload_points 本身不允许 null(契约 shm)
        if (pp.is_null()) {
            throw std::runtime_error("preload_points must not be null");
        }
        if (!pp.is_object()) {
            throw std::runtime_error("preload_points must be an object");
        }
        if (!merged["preload_points"].is_object()) {
            merged["preload_points"] = nlohmann::json::object();
        }
        // 纯 RFC 7386:pp 为 {} 时递归合并无 key 改动 = 无操作(契约 shm)
        for (auto it = pp.begin(); it != pp.end(); ++it) {
            const std::string& key = it.key();
            const nlohmann::json& val = it.value();
            // 契约 shm:key 必须为 "HH:MM"
            if (!is_hhmm(key)) {
                throw std::runtime_error("invalid preload_points key format: " + key);
            }
            if (val.is_null()) {
                // 契约 shm:null 在此唯一合法位置 = 删除该 key
                merged["preload_points"].erase(key);
            } else if (val.is_object()) {
                // 递归合并:从现有 point(规整为 {pages,bytes})起步,再叠加 val 的 pages/bytes
                // 契约 shm:新增 key 时缺失 pages/bytes 补默认值 0
                nlohmann::json point = {{"pages", 0}, {"bytes", 0}};
                if (merged["preload_points"].contains(key) &&
                    merged["preload_points"][key].is_object()) {
                    const nlohmann::json& existing = merged["preload_points"][key];
                    if (existing.contains("pages") && is_integer_value(existing["pages"])) {
                        point["pages"] = existing["pages"];
                    }
                    if (existing.contains("bytes") && is_integer_value(existing["bytes"])) {
                        point["bytes"] = existing["bytes"];
                    }
                }
                if (val.contains("pages")) {
                    if (val["pages"].is_null()) {
                        throw std::runtime_error("pages must not be null");
                    }
                    if (!is_integer_value(val["pages"])) {
                        throw std::runtime_error("pages must be an integer");
                    }
                    point["pages"] = val["pages"];
                }
                if (val.contains("bytes")) {
                    if (val["bytes"].is_null()) {
                        throw std::runtime_error("bytes must not be null");
                    }
                    if (!is_integer_value(val["bytes"])) {
                        throw std::runtime_error("bytes must be an integer");
                    }
                    point["bytes"] = val["bytes"];
                }
                // val 中的额外字段忽略(不污染 cfg_)
                merged["preload_points"][key] = std::move(point);
            } else {
                // 契约 shm:value 非 object 且非 null 为校验失败
                throw std::runtime_error("preload_points value must be object or null: " + key);
            }
        }
    }
    return merged;
}

nlohmann::json ShmConfigBase::load_cfg_from_file(bool& degraded) const {
    nlohmann::json raw;
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            nlohmann::json full;
            ifs >> full;  // 解析失败抛 std::exception
            if (section_.empty()) {
                raw = full.is_object() ? full : nlohmann::json::object();
                if (!full.is_object()) {
                    degraded = true;
                }
            } else if (full.contains(section_)) {
                raw = full.at(section_);
            }
        } else {
            degraded = true;  // 文件存在但打不开:回退默认并尝试修复
        }
    }
    // 补齐缺失字段为默认值,只接受正确类型,再校验值有效性
    // (契约 RTN 始终全量,文件被手改成非法值时用默认值覆盖)
    nlohmann::json result = default_cfg_;
    if (raw.is_object()) {
        nlohmann::json candidate = default_cfg_;
        bool field_degraded = false;
        // 类型合法才采用,类型错则降级该字段为默认值并标记修复
        auto take_int = [&](const char* key) {
            if (!raw.contains(key)) {
                return;
            }
            if (is_integer_value(raw[key])) {
                candidate[key] = raw[key];
            } else {
                field_degraded = true;
            }
        };
        take_int("page_size_mb");
        take_int("check_interval_min");
        take_int("check_pages");
        take_int("check_bytes");
        if (raw.contains("preload_points")) {
            if (raw["preload_points"].is_object()) {
                nlohmann::json pp = nlohmann::json::object();
                for (auto it = raw["preload_points"].begin(); it != raw["preload_points"].end();
                     ++it) {
                    if (!is_hhmm(it.key()) || !it.value().is_object()) {
                        field_degraded = true;  // 跳过非法 entry 并标记修复
                        continue;
                    }
                    const nlohmann::json& v = it.value();
                    nlohmann::json point = {{"pages", 0}, {"bytes", 0}};
                    if (v.contains("pages") && is_integer_value(v["pages"])) {
                        point["pages"] = v["pages"];
                    } else if (v.contains("pages")) {
                        field_degraded = true;
                    }
                    if (v.contains("bytes") && is_integer_value(v["bytes"])) {
                        point["bytes"] = v["bytes"];
                    } else if (v.contains("bytes")) {
                        field_degraded = true;
                    }
                    pp[it.key()] = std::move(point);
                }
                candidate["preload_points"] = std::move(pp);
            } else {
                field_degraded = true;  // 非 object:整段降级默认
            }
        }
        if (auto err = validate(candidate)) {
            field_degraded = true;  // 值超范围:保留 result = default_cfg_
        } else {
            result = std::move(candidate);  // 校验通过,采用文件值
        }
        degraded = degraded || field_degraded;
    }
    return result;
}

void ShmConfigBase::save_cfg_to_file(const nlohmann::json& cfg) const {
    // load-modify-save 保留其他 section,原子写(tmp + rename)
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(cfg_path_)) {
        std::ifstream ifs(cfg_path_);
        if (ifs) {
            try {
                ifs >> full;
            } catch (const std::exception&) {
                // 旧文件损坏:备份后用空 object 起步
                full = nlohmann::json::object();
                std::error_code ec;
                auto backup = cfg_path_;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                                            std::chrono::system_clock::now()));
                std::filesystem::rename(cfg_path_, backup, ec);
            }
        }
    }
    if (section_.empty()) {
        full = cfg;  // root:整个文件就是配置
    } else {
        full[section_] = cfg;
    }
    auto tmp = cfg_path_;
    tmp += ".tmp";
    try {
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                throw std::runtime_error("open failed | path=" + tmp.string());
            }
            ofs << full.dump(2);
            ofs.flush();
            if (!ofs) {
                throw std::runtime_error("write failed | path=" + tmp.string());
            }
            ofs.close();
            if (!ofs) {
                throw std::runtime_error("close failed | path=" + tmp.string());
            }
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

void ShmConfigBase::load() {
    bool degraded = false;
    std::string reason;
    try {
        cfg_ = load_cfg_from_file(degraded);
    } catch (const std::exception& e) {
        // 文件 JSON 损坏:用默认值修复,不中断启动
        degraded = true;
        reason = e.what();
        cfg_ = default_cfg_;
    }
    if (!degraded) {
        return;
    }
    if (reason.empty()) {
        reason = "invalid values, using defaults";
    }
    spdlog::error("shm config load degraded, using defaults | error=\"{}\"", reason);
    try {
        save_cfg_to_file(cfg_);  // 修复文件(内容与内存值同步)
    } catch (const std::exception& se) {
        // 修复失败:内存值已是默认/降级值,进程能跑,仅告警
        spdlog::error("shm config repair save failed | error=\"{}\"", se.what());
    }
}

void ShmConfigBase::set_shm_config(const nlohmann::json& patch) {
    nlohmann::json merged = merge_patch(patch);  // 类型/结构错误抛,cfg_ 不变
    if (auto err = validate(merged)) {
        throw std::runtime_error(*err);  // 范围错误,cfg_ 不变
    }
    // 契约 shm:空对象 {} 视为无操作。值无变化(含 {}、page_size_mb 单独提交、
    // preload_points:{})时跳过持久化,仍回 RTN(当前值)
    if (merged == cfg_) {
        return;
    }
    save_cfg_to_file(merged);  // 持久化失败抛,cfg_ 不变
    cfg_ = std::move(merged);  // 提交
}

}  // namespace dztrader::platform::detail

namespace dztrader::platform {

void EventShmConfig::send_rtn(const nlohmann::json& cfg) {
    // 事件通道无 instance_id 帧 (DzExtFrameHeader)
    // 内联"写帧 + notify"(detail::write_ext_json 模板版未
    // notify,且为不改动共享头,此处自包含)
    try {
        const auto str = cfg.dump();
        if (!writer_.write_ext_frame(DZ_FRAME_RTN_EVENT_SHM_CONFIG,
                                     reinterpret_cast<const std::byte*>(str.data()),
                                     static_cast<uint32_t>(str.size()))) {
            spdlog::error("event shm config rtn write failed");
            return;
        }
        writer_.notify_subscribers();
    } catch (const std::exception& e) {
        spdlog::error("event shm config rtn serialize failed | error=\"{}\"", e.what());
    }
}

nlohmann::json EventShmConfig::make_default() {
    return {
        {"page_size_mb", 64},      {"preload_points", nlohmann::json::object()},
        {"check_interval_min", 0}, {"check_pages", 0},
        {"check_bytes", 0},
    };
}

nlohmann::json MdShmConfig::make_default() {
    return {
        {"page_size_mb", 1024},    {"preload_points", nlohmann::json::object()},
        {"check_interval_min", 0}, {"check_pages", 0},
        {"check_bytes", 0},
    };
}

}  // namespace dztrader::platform
