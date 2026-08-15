#include "log_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <regex>

namespace dztrader::webui {

namespace {
// spdlog FILE_PATTERN: "%Y-%m-%dT%T.%F%z %l %n [func=%! file=%s:%# pid=%P tid=%t] %v"
// 捕获组：1=ts 2=level 3=logger 4=func 5=file 6=line 7=pid 8=tid 9=msg
// 注意：func 名称可能包含空格（如 C++ 的 operator()、lambda），所以不能用 \S+
//       使用 [^\]]+? 非贪婪匹配到 ] 之前的字符，然后必须匹配空格
const std::regex SPDLOG_PATTERN(
    R"(^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{1,9}[+-]\d{2}:\d{2})\s+)"
    R"((\w+)\s+(\S+)\s+\[func=([^\]]*?)\s+file=([^:\s]*):(\d*)\s+pid=(\d+)\s+tid=(\d+)\]\s+(.*)$)"
);

/// 日志级别严重程度排序：trace < debug < info < warning < error < critical < off
/// 返回 -1 表示未知级别（不会匹配任何阈值）
int level_severity(const std::string& level) {
    static const std::vector<std::string> ORDERED = {
        "trace", "debug", "info", "warning", "error", "critical", "off"
    };
    auto it = std::find(ORDERED.begin(), ORDERED.end(), level);
    return it != ORDERED.end() ? static_cast<int>(std::distance(ORDERED.begin(), it)) : -1;
}
}  // namespace

LogService::LogService(std::filesystem::path log_dir)
    : log_dir_(std::move(log_dir)) {}

std::string LogService::extract_logger(const std::string& filename) {
    // Strip .log suffix
    std::string base = filename;
    if (base.ends_with(".log")) {
        base = base.substr(0, base.size() - 4);
    }
    // Strip _YYYY-MM-DD date suffix if present
    if (base.size() >= 11 && base[base.size() - 11] == '_') {
        const std::string maybe_date = base.substr(base.size() - 10);
        if (std::regex_match(maybe_date, std::regex(R"(\d{4}-\d{2}-\d{2})"))) {
            return base.substr(0, base.size() - 11);
        }
    }
    return base;
}

std::vector<LogFileInfo> LogService::list_files(const std::string& logger_filter,
                                                 const std::string& date_filter,
                                                 int limit, int offset) {
    std::vector<LogFileInfo> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(log_dir_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto name = entry.path().filename().string();
        if (!name.ends_with(".log")) {
            continue;
        }

        const std::string logger = extract_logger(name);
        if (!logger_filter.empty() && logger != logger_filter) {
            continue;
        }

        // 日期筛选：从文件名提取 _YYYY-MM-DD 后缀
        if (!date_filter.empty()) {
            std::string base = name.substr(0, name.size() - 4); // strip .log
            bool date_matched = false;
            if (base.size() >= 11 && base[base.size() - 11] == '_') {
                const std::string maybe_date = base.substr(base.size() - 10);
                if (maybe_date == date_filter) {
                    date_matched = true;
                }
            }
            if (!date_matched) {
                continue;
            }
        }

        LogFileInfo info;
        info.name = name;
        info.logger = logger;
        info.size = entry.file_size(ec);
        info.path = std::filesystem::relative(entry.path(), log_dir_, ec).generic_string();
        if (ec) {
            info.path = name;
        }

        // Format mtime as ISO 8601
        auto ftime = entry.last_write_time(ec);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        info.mtime = std::format("{:%Y-%m-%dT%T}", sctp);

        files.push_back(std::move(info));
    }

    // Sort by mtime descending (most recent first)
    std::sort(files.begin(), files.end(),
              [](const LogFileInfo& a, const LogFileInfo& b) {
                  return a.mtime > b.mtime;
              });

    // Paginate
    offset = (std::max)(offset, 0);
    if (offset >= static_cast<int>(files.size())) {
        return {};
    }
    int end = (std::min)(offset + limit, static_cast<int>(files.size()));
    if (limit <= 0) {
        end = static_cast<int>(files.size());
    }

    return std::vector<LogFileInfo>(files.begin() + offset, files.begin() + end);
}

bool LogService::is_path_safe(const std::string& path) const {
    if (path.find("..") != std::string::npos) {
        return false;
    }

    const std::filesystem::path p(path);
    if (p.is_absolute()) {
        return false;
    }

    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(log_dir_ / p, ec);
    if (ec) {
        return false;
    }

    auto log_dir_canonical = std::filesystem::weakly_canonical(log_dir_, ec);
    if (ec) {
        return false;
    }

    auto rel = std::filesystem::relative(resolved, log_dir_canonical, ec);
    if (ec) {
        return false;
    }

    auto rel_str = rel.string();
    return !rel_str.empty() && !rel_str.starts_with("..");
}

std::optional<LogLine> LogService::parse_line(const std::string& raw, int line_num) {
    std::smatch match;
    if (!std::regex_match(raw, match, SPDLOG_PATTERN)) {
        return std::nullopt;
    }

    LogLine line;
    line.n = line_num;
    line.ts = match[1].str();
    line.level = match[2].str();
    line.logger = match[3].str();
    line.func = match[4].str();
    line.file = match[5].str();
    line.line_no = match[6].length() > 0 ? std::stoi(match[6].str()) : 0;
    line.pid = match[7].str();
    line.tid = match[8].str();
    line.msg = match[9].str();
    line.raw = raw;
    line.parsed = true;
    return line;
}

LogContent LogService::read_content(const std::string& filename,
                                    int offset, int limit,
                                    const std::string& level_filter,
                                    const std::string& keyword,
                                    const std::string& from_ts,
                                    const std::string& to_ts,
                                    bool from_end) {
    LogContent result;

    if (!is_path_safe(filename)) {
        result.total = 0;
        return result;
    }

    auto filepath = log_dir_ / filename;
    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        result.total = 0;
        return result;
    }

    const bool has_filter =
        !level_filter.empty() || !keyword.empty() || !from_ts.empty() || !to_ts.empty();

    // Seek-optimized path: no filter, from_end, large file (>= 1MB)
    if (from_end && !has_filter) {
        auto file_size = std::filesystem::file_size(filepath, ec);
        if (file_size >= 1048576) {
            return read_from_end_seek(filepath, file_size, limit);
        }
        // Small file: fall through to full read, take last `limit` below
    }

    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        result.total = 0;
        return result;
    }

    // Read and filter
    std::vector<LogLine> filtered;
    std::string raw_line;
    int line_num = 0;
    while (std::getline(ifs, raw_line)) {
        ++line_num;

        auto parsed = parse_line(raw_line, line_num);

        // Apply filters
        if (!level_filter.empty()) {
            const int threshold = level_severity(level_filter);
            if (!parsed || level_severity(parsed->level) < threshold) {
                continue;
            }
        }
        if (!keyword.empty()) {
            const std::string msg = parsed ? parsed->msg : raw_line;
            if (msg.find(keyword) == std::string::npos) {
                continue;
            }
        }
        if (!from_ts.empty() && parsed) {
            if (parsed->ts < from_ts) {
                continue;
            }
        }
        if (!to_ts.empty() && parsed) {
            if (parsed->ts > to_ts) {
                continue;
            }
        }

        if (parsed) {
            filtered.push_back(*parsed);
        } else {
            LogLine unparsed;
            unparsed.n = line_num;
            unparsed.raw = raw_line;
            unparsed.parsed = false;
            filtered.push_back(std::move(unparsed));
        }
    }

    result.total = static_cast<int>(filtered.size());

    if (from_end) {
        // Take last `limit` lines
        const int start = (limit > 0) ? (std::max)(0, static_cast<int>(filtered.size()) - limit) : 0;
        result.lines = std::vector<LogLine>(filtered.begin() + start, filtered.end());
    } else {
        // Offset-based pagination (existing behavior)
        offset = (std::max)(offset, 0);
        if (offset >= static_cast<int>(filtered.size())) {
            result.lines = {};
            return result;
        }
        int end = (std::min)(offset + limit, static_cast<int>(filtered.size()));
        if (limit <= 0) {
            end = static_cast<int>(filtered.size());
        }
        result.lines = std::vector<LogLine>(filtered.begin() + offset,
                                            filtered.begin() + end);
    }

    return result;
}

LogContent LogService::read_from_end_seek(const std::filesystem::path& filepath,
                                           std::uintmax_t file_size, int limit) {
    LogContent result;

    // Estimate chunk size: max(limit * 256, 64KB)
    auto chunk_size = std::max<std::uintmax_t>(
        static_cast<std::uintmax_t>(limit) * 256, 65536);
    chunk_size = (std::min)(chunk_size, file_size);

    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        result.total = 0;
        return result;
    }

    auto seek_pos = file_size - chunk_size;

    // Count newlines in the skipped portion (before seek point)
    int skipped_newlines = 0;
    {
        constexpr size_t buf_size = 65536;
        std::vector<char> buf(buf_size);
        std::uintmax_t read_so_far = 0;
        while (read_so_far < seek_pos) {
            auto to_read = std::min<std::uintmax_t>(buf_size, seek_pos - read_so_far);
            ifs.read(buf.data(), static_cast<std::streamsize>(to_read));
            auto gcount = ifs.gcount();
            if (gcount <= 0) {
                break;
            }
            skipped_newlines += static_cast<int>(
                std::count(buf.begin(), buf.begin() + gcount, '\n'));
            read_so_far += static_cast<std::uintmax_t>(gcount);
        }
    }

    // Seek to chunk start
    ifs.clear();
    ifs.seekg(static_cast<std::streamoff>(seek_pos), std::ios::beg);

    // Skip first line (likely partial — spdlog may be mid-write)
    std::string dummy;
    std::getline(ifs, dummy);

    // Read complete lines from seek point to EOF
    std::vector<LogLine> lines;
    std::string raw_line;
    int line_num = skipped_newlines + 1;  // partial line number
    while (std::getline(ifs, raw_line)) {
        if (ifs.eof()) {
            break;  // Last line without newline — spdlog may be mid-write
        }
        // Binary mode on Windows leaves \r at end of line; strip it
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.pop_back();
        }
        ++line_num;
        auto parsed = parse_line(raw_line, line_num);
        if (parsed) {
            lines.push_back(*parsed);
        } else {
            LogLine unparsed;
            unparsed.n = line_num;
            unparsed.raw = raw_line;
            unparsed.parsed = false;
            lines.push_back(std::move(unparsed));
        }
    }

    // Keep only the last `limit` lines
    if (limit > 0 && static_cast<int>(lines.size()) > limit) {
        lines.erase(lines.begin(), lines.end() - limit);
    }

    result.lines = std::move(lines);
    result.total = line_num;  // actual total line count
    return result;
}

LogContent LogService::read_tail(const std::string& filename, int from_line, int limit) {
    LogContent result;
    result.total = from_line;  // 默认：无进展

    if (!is_path_safe(filename)) {
        return result;
    }

    auto filepath = log_dir_ / filename;
    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        return result;
    }

    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        return result;
    }

    std::string raw_line;
    int line_num = 0;  // 实际读到的最后一行行号
    while (std::getline(ifs, raw_line)) {
        // getline 返回 true 但 eofbit 已设置：文件末尾无换行符，
        // 说明该行可能尚未写完（spdlog 正在写入）。不推送、不递增 line_num、
        // 不推进 baseline，下次轮询从同一行重新读。
        if (ifs.eof()) {
            break;
        }
        ++line_num;
        if (line_num <= from_line) {
            continue;  // 跳过已读行
        }
        if (limit > 0 && static_cast<int>(result.lines.size()) >= limit) {
            break;  // 达到单次推送上限，停止读取（剩余行下次再读）
        }
        auto parsed = parse_line(raw_line, line_num);
        if (parsed) {
            result.lines.push_back(*parsed);
        } else {
            LogLine unparsed;
            unparsed.n = line_num;
            unparsed.raw = raw_line;
            unparsed.parsed = false;
            result.lines.push_back(std::move(unparsed));
        }
    }

    // 计算 baseline：
    // - 文件被截断（line_num < from_line）：重置为实际行数
    // - 正常：from_line + 本次读到的行数
    if (line_num < from_line) {
        result.total = line_num;
    } else {
        result.total = from_line + static_cast<int>(result.lines.size());
    }
    return result;
}

LogService::LogStats LogService::get_stats(const std::string& filename,
                                           const std::string& from_ts,
                                           const std::string& to_ts,
                                           const std::string& logger_filter) {
    LogStats stats;
    stats.total = 0;
    auto content = read_content(filename, 0, 0, "", "", from_ts, to_ts);
    // read_content with limit=0 returns all filtered lines

    std::string first_ts, last_ts;
    for (const auto& line : content.lines) {
        if (!line.parsed) {
            continue;
        }
        if (!logger_filter.empty() && line.logger != logger_filter) {
            continue;
        }

        stats.by_level[line.level]++;
        stats.total++;

        if (first_ts.empty() || line.ts < first_ts) {
            first_ts = line.ts;
        }
        if (last_ts.empty() || line.ts > last_ts) {
            last_ts = line.ts;
        }
    }

    stats.timespan = first_ts + " to " + last_ts;
    return stats;
}

namespace {
/// Extract a message template by replacing variable parts with *.
std::string extract_template(const std::string& msg) {
    std::string t = msg;
    // Replace numbers (including decimals)
    static const std::regex NUM_RE(R"(\d+\.?\d*)");
    t = std::regex_replace(t, NUM_RE, "*");
    // Replace IP addresses
    static const std::regex IP_RE(R"(\d+\.\d+\.\d+\.\d+)");
    t = std::regex_replace(t, IP_RE, "*");
    // Replace key=value where value is a number or quoted string
    static const std::regex KV_RE(R"((\w+)=("[^"]*"|\d+\.?\d*))");
    t = std::regex_replace(t, KV_RE, "$1=*");
    // Replace quoted strings
    static const std::regex STR_RE(R"("[^"]*")");
    t = std::regex_replace(t, STR_RE, "*");
    return t;
}

/// Truncate a timestamp to the given bucket precision.
std::string truncate_ts(const std::string& ts, const std::string& bucket) {
    // ts format: "2026-07-13T14:23:45.123456789+08:00"
    if (bucket == "minute") {
        return ts.substr(0, 16);  // "2026-07-13T14:23"
    }
    if (bucket == "hour") {
        return ts.substr(0, 13);  // "2026-07-13T14"
    }
    if (bucket == "day") {
        return ts.substr(0, 10);  // "2026-07-13"
    }
    return ts.substr(0, 16);  // default minute
}
}  // namespace

std::vector<LogService::ErrorAggregate> LogService::get_aggregate(
    const std::string& filename, const std::string& level, int limit) {
    // Read all lines at the target level and above (e.g. "error" includes critical)
    auto content = read_content(filename, 0, 0, level, "", "", "");

    // Group by template
    std::map<std::string, ErrorAggregate> groups;
    for (const auto& line : content.lines) {
        if (!line.parsed) {
            continue;
        }
        const std::string tmpl = extract_template(line.msg);
        auto& agg = groups[tmpl];
        if (agg.msg_pattern.empty()) {
            agg.msg_pattern = tmpl;
        }
        agg.count++;
        if (agg.first_ts.empty() || line.ts < agg.first_ts) {
            agg.first_ts = line.ts;
        }
        if (agg.last_ts.empty() || line.ts > agg.last_ts) {
            agg.last_ts = line.ts;
        }
        if (static_cast<int>(agg.samples.size()) < 3) {
            agg.samples.push_back(line.raw);
        }
    }

    // Sort by count descending, take top N
    std::vector<ErrorAggregate> result;
    for (auto& [_, agg] : groups) {
        result.push_back(std::move(agg));
    }
    std::sort(result.begin(), result.end(),
              [](const ErrorAggregate& a, const ErrorAggregate& b) {
                  return a.count > b.count;
              });

    if (limit > 0 && static_cast<int>(result.size()) > limit) {
        result.resize(limit);
    }
    return result;
}

std::vector<LogService::TimelineBucket> LogService::get_timeline(
    const std::string& filename, const std::string& bucket) {
    auto content = read_content(filename, 0, 0, "", "", "", "");

    std::map<std::string, TimelineBucket> buckets;
    for (const auto& line : content.lines) {
        if (!line.parsed) {
            continue;
        }
        const std::string key = truncate_ts(line.ts, bucket);
        auto& b = buckets[key];
        if (b.ts.empty()) {
            b.ts = key;
        }
        b.counts[line.level]++;
    }

    std::vector<TimelineBucket> result;
    for (auto& [_, b] : buckets) {
        result.push_back(std::move(b));
    }
    // Sort by timestamp ascending
    std::sort(result.begin(), result.end(),
              [](const TimelineBucket& a, const TimelineBucket& b) {
                  return a.ts < b.ts;
              });
    return result;
}

}  // namespace dztrader::webui
