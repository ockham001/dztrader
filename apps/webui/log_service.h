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
    /// tail 游标：字节偏移 + 行号基线（跨调用保持，支持增量读与轮换检测）
    struct TailCursor {
        long long byte_offset = 0;  // 已消费到的字节位置（下一条新行起点）
        int line_no = 0;            // 已消费到的行号（1-based；0 = 未读过）
    };

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

    /// 订阅基线：扫描文件当前末尾，返回"只追新增"的游标。
    /// 分块数换行（不逐行 parse，远快于 read_content 全量计数）；
    /// 末尾无换行的半行不计入（偏移停在最后一个 '\n' 之后），留给 read_tail 补齐。
    /// 文件不存在/路径不安全/打不开 → 返回 {0,0}（从文件头开始 tail）。
    TailCursor tail_baseline(const std::string& filename);

    /// Tail 专用增量读：从 cursor.byte_offset 起只读新增字节，解析为行。
    /// - 文件大小 < cursor.byte_offset（轮换/截断）→ 自动重置游标从头读（行号重新累计）
    /// - 末尾无换行的半行不消费（游标不推进），下次调用补齐
    /// - 达到 limit 停止读取，剩余行下次再读
    /// @param cursor  入参=上次游标；出参=本次推进后的游标（send 失败时调用方可回退）
    /// @return LogContent.total = 推进后的 cursor.line_no（建议的下次行号基线）
    LogContent read_tail(const std::string& filename, TailCursor& cursor, int limit);

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
