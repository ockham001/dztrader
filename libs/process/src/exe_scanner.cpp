#include <dztrader/process/exe_scanner.h>
#include <dztrader/core/encoding.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace dztrader::process {

namespace {

/// 检查路径是否为可执行文件 (跨平台启发式)
/// Windows: .exe / .bat / .cmd 后缀
/// Linux: 常规文件 + owner_exec 权限位
bool is_executable(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
        return false;
    }
#ifdef _WIN32
    auto ext = p.extension().string();
    for (auto& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".exe" || ext == ".bat" || ext == ".cmd";
#else
    auto status = std::filesystem::status(p, ec);
    if (ec) {
        return false;
    }
    return std::filesystem::is_regular_file(status) &&
           (status.permissions() & std::filesystem::perms::owner_exec) !=
               std::filesystem::perms::none;
#endif
}

/// 解析可执行文件名, 识别进程类别
/// "dzmd_ctp" -> ("dzmd_ctp", GatewayMd)
/// "dztd_ctp" -> ("dztd_ctp", GatewayTd)
/// "dzweb"    -> ("dzweb",    WebUI)
/// 退化检查: dzmd_/dztd_ 后必须非空 (stem.size() > 5)
bool parse_gateway_binary(const std::string& filename, std::string& name, ProcessKind& kind) {
    // Windows 上剥离扩展名
    std::string stem = std::filesystem::path(filename).stem().string();

    if (stem.size() > 5 && stem.starts_with("dzmd_")) {
        name = stem;
        kind = ProcessKind::GatewayMd;
        return true;
    }
    if (stem.size() > 5 && stem.starts_with("dztd_")) {
        name = stem;
        kind = ProcessKind::GatewayTd;
        return true;
    }
    if (stem == "dzweb") {
        name = stem;
        kind = ProcessKind::WebUI;
        return true;
    }
    return false;
}

}  // namespace

std::vector<ProcessExeInfo> scan_all_exes(const std::filesystem::path& root) {
    std::vector<ProcessExeInfo> result;

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
        SPDLOG_WARN("scan_all_exes: root not found | path={}", root.string());
        return result;
    }

    // 已发现 stem 集合, 用于二级子目录去重 (根目录优先)
    std::unordered_set<std::string> seen_stems;

    // 第一层: 扫描 root 下所有可执行文件
    for (const auto& file_entry : std::filesystem::directory_iterator(root, ec)) {
        if (!file_entry.is_regular_file()) {
            continue;
        }
        std::string entry_name;
        ProcessKind kind{};
        if (!parse_gateway_binary(file_entry.path().filename().string(), entry_name, kind)) {
            continue;
        }
        if (!is_executable(file_entry.path())) {
            SPDLOG_WARN("scan_all_exes: binary not executable, skipping | path={}",
                        file_entry.path().string());
            continue;
        }
        std::string stem = file_entry.path().stem().string();
        seen_stems.insert(stem);
        result.push_back(ProcessExeInfo{
            .name = entry_name,
            .kind = kind,
            .exe = file_entry.path(),
            .start_dir = file_entry.path().parent_path(),
        });
    }

    if (ec) {
        SPDLOG_WARN("scan_all_exes: iterate root failed | error=\"{}\"",
                    dztrader::to_utf8_from_system(ec.message()));
        ec.clear();
    }

    // 第二层: 遍历 root 下每个子目录 (仅该子目录, 不递归)
    for (const auto& sub_entry : std::filesystem::directory_iterator(root, ec)) {
        if (!sub_entry.is_directory()) {
            continue;
        }
        std::error_code sub_ec;
        for (const auto& file_entry :
                std::filesystem::directory_iterator(sub_entry.path(), sub_ec)) {
            if (sub_ec) {
                // 失败路径 C: 子目录 IO 错误时跳过该子目录, 不整体失败
                SPDLOG_WARN("scan_all_exes: iterate subdir failed | path={} error=\"{}\"",
                            sub_entry.path().string(),
                            dztrader::to_utf8_from_system(sub_ec.message()));
                break;
            }
            if (!file_entry.is_regular_file()) {
                continue;
            }
            std::string entry_name;
            ProcessKind kind{};
            if (!parse_gateway_binary(file_entry.path().filename().string(), entry_name, kind)) {
                continue;
            }
            if (!is_executable(file_entry.path())) {
                SPDLOG_WARN("scan_all_exes: binary not executable, skipping | path={}",
                            file_entry.path().string());
                continue;
            }
            std::string stem = file_entry.path().stem().string();
            // 去重: 二级子目录出现与根目录同 stem 的进程时跳过
            if (seen_stems.contains(stem)) {
                continue;
            }
            seen_stems.insert(stem);
            result.push_back(ProcessExeInfo{
                .name = entry_name,
                .kind = kind,
                .exe = file_entry.path(),
                .start_dir = file_entry.path().parent_path(),
            });
        }
        if (sub_ec) {
            SPDLOG_WARN("scan_all_exes: scan subdir failed | path={} error=\"{}\"",
                        sub_entry.path().string(),
                        dztrader::to_utf8_from_system(sub_ec.message()));
            continue;
        }
    }

    if (ec) {
        SPDLOG_WARN("scan_all_exes: iterate root for subdirs failed | error=\"{}\"",
                    dztrader::to_utf8_from_system(ec.message()));
    }

    // 按 name 字典序排序, 保证调用方输出稳定
    std::sort(result.begin(), result.end(),
              [](const ProcessExeInfo& a, const ProcessExeInfo& b) { return a.name < b.name; });

    SPDLOG_INFO("scan_all_exes: scanned | count={} root={}", result.size(), root.string());
    return result;
}

std::optional<ProcessExeInfo> find_exe_by_stem(std::string_view name,
                                                const std::filesystem::path& root) {
    auto all = scan_all_exes(root);
    auto it = std::find_if(all.begin(), all.end(),
                           [&](const ProcessExeInfo& info) { return info.name == name; });
    if (it == all.end()) {
        SPDLOG_WARN("find_exe_by_stem: not found | name={}", name);
        return std::nullopt;
    }
    SPDLOG_INFO("find_exe_by_stem: found | name={} exe={}", name, it->exe.string());
    return *it;
}

}  // namespace dztrader::process
