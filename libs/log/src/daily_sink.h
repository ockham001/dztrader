#ifndef DZTRADER_LOG_SRC_DAILY_SINK_H_
#define DZTRADER_LOG_SRC_DAILY_SINK_H_

#include <spdlog/sinks/daily_file_sink.h>

namespace dztrader::log {

struct DailyFilenameCalculator {
    static spdlog::filename_t calc_filename(const spdlog::filename_t& filename, const tm& now_tm) {
        spdlog::filename_t basename, ext;
        std::tie(basename, ext) = spdlog::details::file_helper::split_by_extension(filename);
        // 文件名格式: <basename>_YYYY-MM-DD<ext>
        // 与 log_service.cpp extract_logger 期望的 _YYYY-MM-DD 后缀匹配
        return spdlog::fmt_lib::format(
            SPDLOG_FMT_STRING(SPDLOG_FILENAME_T("{}_{:04d}-{:02d}-{:02d}{}")), basename,
            now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday, ext);
    }
};

using DailyFileSinkMt = spdlog::sinks::daily_file_sink<std::mutex, DailyFilenameCalculator>;

}  // namespace dztrader::log

#endif  // DZTRADER_LOG_SRC_DAILY_SINK_H_
