#ifndef DZTRADER_CORE_JSON_SECTION_H_
#define DZTRADER_CORE_JSON_SECTION_H_

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace dztrader::core {

/// 从 JSON 文件的指定 section 加载 T。
/// 文件不存在或 section 缺失时返回默认构造的 T。
/// JSON 解析错误时抛 std::runtime_error。
/// sub_section 非空时读 section.sub_section (嵌套子段)。
template<typename T>
T load_json_section(const std::filesystem::path& path, const std::string& section,
                    const std::string& sub_section = "") {
    if (!std::filesystem::exists(path)) {
        return T{};
    }
    std::ifstream ifs(path);
    if (!ifs) {
        return T{};
    }
    nlohmann::json full;
    try {
        ifs >> full;
    } catch (const std::exception& e) {
        throw std::runtime_error("JSON parse failed | path=" + path.string() + " error=\"" + e.what() + "\"");
    }
    if (!full.is_object() || !full.contains(section)) {
        return T{};
    }
    auto node = full[section];
    if (!sub_section.empty()) {
        if (!node.is_object() || !node.contains(sub_section)) {
            return T{};
        }
        node = node[sub_section];
    }
    return node.get<T>();
}

/// 原子写入 JSON 文件的指定 section (load-modify-save)。
/// 读现有文件 (不存在或解析失败则空 object, 解析失败时备份原文件为 <path>.corrupt.<ts>),
/// 更新 section, tmp+rename 原子写回。
/// 显式 close + 检查流状态, 防止磁盘满/配额耗尽时静默产生截断文件。
/// sub_section 非空时写 section.sub_section (嵌套子段, 保留 section 中其他字段)。
/// @throws std::runtime_error 文件打开/写入/close/rename 失败时抛出。
template<typename T>
void save_json_section(const std::filesystem::path& path, const std::string& section, const T& data,
                       const std::string& sub_section = "") {
    nlohmann::json full = nlohmann::json::object();
    if (std::filesystem::exists(path)) {
        std::ifstream ifs(path);
        if (ifs) {
            try {
                ifs >> full;
            } catch (const std::exception&) {
                // 旧文件 JSON 损坏: 不能静默清空其他 section, 否则单 section 写入会丢失全部配置。
                // 备份损坏文件后用空 object 起步, 让用户能从备份恢复。
                full = nlohmann::json::object();
                std::error_code ec;
                auto backup = path;
                backup += ".corrupt." + std::to_string(std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now()));
                std::filesystem::rename(path, backup, ec);
                // rename 失败不阻断 save (可能 permission denied), 但至少尝试备份
            }
        }
    }
    if (sub_section.empty()) {
        full[section] = data;
    } else {
        if (!full.contains(section) || !full[section].is_object()) {
            full[section] = nlohmann::json::object();
        }
        full[section][sub_section] = data;
    }

    auto tmp = path;
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
            // 显式 close 并检查: ofstream 析构调用的 close() 会丢弃错误,
            // 磁盘满/配额耗尽时可能写入不完整, rename 会把截断文件提升为正式配置。
            ofs.close();
            if (!ofs) {
                throw std::runtime_error("close failed | path=" + tmp.string());
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            throw std::runtime_error("rename failed | from=" + tmp.string() + " to=" + path.string());
        }
    } catch (const std::exception&) {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        throw;
    }
}

}  // namespace dztrader::core

#endif  // DZTRADER_CORE_JSON_SECTION_H_
