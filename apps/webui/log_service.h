#ifndef DZTRADER_WEBUI_LOG_SERVICE_H_
#define DZTRADER_WEBUI_LOG_SERVICE_H_

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {

struct LogFileInfo {
    std::string name;     // e.g. "master_2026-07-13.log"
    std::string logger;   // e.g. "dztraderd" (parsed from filename)
    uintmax_t size;       // bytes
    std::string mtime;    // ISO 8601 last-modified time
    std::string path;     // full path
};

struct LogLine {
    int n;                // 1-based line number
    std::string ts;       // timestamp (raw from log)
    std::string level;    // info/warning/error/critical/...（spdlog 默认小写显示名）
    std::string logger;   // logger name
    std::string func;     // function name
    std::string file;     // source file
    int line_no;          // source line number
    std::string pid;
    std::string tid;
    std::string msg;      // message content
    std::string raw;      // raw line text
    bool parsed;          // false if regex didn't match (strategy logs)
};

struct LogContent {
    std::vector<LogLine> lines;
    int total;            // estimated total lines in file
};

class LogService {
public:
    explicit LogService(std::filesystem::path log_dir);

    /// List log files (*.log), optionally filtered by logger name and/or date.
    /// @param logger_filter  empty = all loggers
    /// @param date_filter    "YYYY-MM-DD" to match filename date suffix; empty = all dates
    /// Sorted by mtime descending. Pagination via limit/offset.
    std::vector<LogFileInfo> list_files(const std::string& logger_filter,
                                        const std::string& date_filter,
                                        int limit, int offset);

    /// Read file content with optional filtering.
    /// @param filename  just the filename (not full path), e.g. "master.log"
    /// @param offset    0-based line offset (after filtering)
    /// @param limit     max lines to return
    /// @param level_filter  lowercase spdlog display level name (info/warning/error/...), empty = all
    /// @param keyword   substring match on msg, empty = all
    /// @param from_ts   ISO timestamp lower bound (inclusive), empty = no bound
    /// @param to_ts     ISO timestamp upper bound (inclusive), empty = no bound
    LogContent read_content(const std::string& filename,
                            int offset, int limit,
                            const std::string& level_filter,
                            const std::string& keyword,
                            const std::string& from_ts,
                            const std::string& to_ts,
                            bool from_end = false);

    /// Tail 专用：从 from_line 之后（不含）开始读取，最多返回 limit 行。
    /// 比 read_content(limit=0) 全量读快得多，适合 50ms 轮询场景。
    /// @param from_line  上次读到的最后一行行号（1-based）；0 表示从文件开头读
    /// @param limit      最多返回的行数（0 = 不限）
    /// @return LogContent.total = 建议的下次 from_line（见 ws_controller 用法）
    ///         - 正常读 N 行：total = from_line + N
    ///         - 达到 limit：total = from_line + limit
    ///         - 文件被截断（实际行数 < from_line）：total = 实际行数（重置 baseline）
    LogContent read_tail(const std::string& filename, int from_line, int limit);

    struct LogStats {
        std::map<std::string, int> by_level;
        int total;
        std::string timespan;  // "first_ts to last_ts"
    };

    struct ErrorAggregate {
        std::string msg_pattern;
        int count;
        std::string first_ts;
        std::string last_ts;
        std::vector<std::string> samples;
    };

    struct TimelineBucket {
        std::string ts;               // bucket timestamp (truncated to bucket precision)
        std::map<std::string, int> counts;  // level → count
    };

    /// Statistics for a log file.
    LogStats get_stats(const std::string& filename,
                       const std::string& from_ts,
                       const std::string& to_ts,
                       const std::string& logger_filter);

    /// Error aggregation: groups error/critical lines by message template.
    /// @param level  target level (default "error"), also includes critical if present
    /// @param limit  max number of aggregate groups
    std::vector<ErrorAggregate> get_aggregate(const std::string& filename,
                                              const std::string& level,
                                              int limit);

    /// Timeline data: buckets log lines by time period.
    /// @param bucket  "minute", "hour", or "day"
    std::vector<TimelineBucket> get_timeline(const std::string& filename,
                                             const std::string& bucket);

    /// Extract logger name from filename: "dztraderd.log" → "dztraderd", "dztraderd_2026-07-13.log" → "dztraderd"
    static std::string extract_logger(const std::string& filename);

private:
    std::filesystem::path log_dir_;

    /// Parse a single spdlog-formatted line. Returns std::nullopt if regex doesn't match.
    static std::optional<LogLine> parse_line(const std::string& raw, int line_num);

    /// Validate that a relative path is safe (no .. traversal, within log_dir_).
    bool is_path_safe(const std::string& path) const;

    /// Seek-optimized read of last N lines from a large file.
    LogContent read_from_end_seek(const std::filesystem::path& filepath,
                                  std::uintmax_t file_size, int limit);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_LOG_SERVICE_H_
