#include "child_process.h"

#include <optional>

#include <boost/asio/buffer.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/process/v2/start_dir.hpp>

#include <dztrader/core/encoding.h>
#include <dztrader/log/log.h>
#include <spdlog/spdlog.h>

namespace dztrader::master {

namespace {

/// 将 key_value_pair_view 转换为窄字符串 "KEY=VALUE"。
/// Windows 上环境条目为 wchar_t，使用 std::wcsrtombs 转换。
std::string env_to_narrow(const process_v2::environment::key_value_pair_view& var) {
#ifdef _WIN32
    const wchar_t* wide = var.c_str();
    if (!wide) {
        return "";
    }
    std::mbstate_t state = std::mbstate_t();
    const wchar_t* ptr = wide;
    auto len = std::wcsrtombs(nullptr, &ptr, 0, &state);
    if (len == static_cast<std::size_t>(-1)) {
        return "";
    }
    std::string result(len, '\0');
    ptr = wide;
    std::wcsrtombs(result.data(), &ptr, len + 1, &state);
    return result;
#else
    return std::string(var.c_str());
#endif
}

/// Windows 上环境变量键名大小写不敏感比较。
bool env_key_matches(std::string_view entry, std::string_view key) {
#ifdef _WIN32
    if (entry.size() <= key.size() + 1) {
        return false;
    }
    if (entry[key.size()] != '=') {
        return false;
    }
    auto entry_key = entry.substr(0, key.size());
    return _stricmp(std::string(entry_key).c_str(), std::string(key).c_str()) == 0;
#else
    return entry.starts_with(std::string(key) + "=");
#endif
}

/// 转义子进程输出中的特殊字符，使其在 k=v 双引号包裹中安全
/// - " → \"
/// - \ → \\（一个反斜杠转义为两个）
/// - 其他字符（包括 | = 换行等）不变
/// 注意：已按行读取，无换行符
std::string escape_for_kv(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
    return out;
}

/// 转发日志的占位符 source_loc：func=forwarded file=external:1
/// 明确标识这是转发的外部内容，非 master 自身代码位置
/// source_loc 参数顺序：(filename, line, funcname) 对应 pattern 的 %s / %# / %!
/// 注意：line 必须 >=1，因为 spdlog source_loc::empty() 在 line<=0 时返回 true，
/// 会导致 %s/%#/%! formatter 提前返回不输出任何字符（即 func/file 字段为空）
static constexpr spdlog::source_loc kForwardedLoc{"external", 1, "forwarded"};

}  // namespace

// ── 工厂函数 ─────────────────────────────────────────────────

std::shared_ptr<ChildProcess> ChildProcess::create(boost::asio::io_context& ioc,
                                                   ProcessEntry entry) {
    // shared_ptr + 私有构造函数 = enable_shared_from_this 安全
    return std::shared_ptr<ChildProcess>(new ChildProcess(ioc, std::move(entry)));
}

ChildProcess::ChildProcess(boost::asio::io_context& ioc, ProcessEntry entry)
    : ioc_(ioc),
      entry_(std::move(entry)) {}

// ── 启动 ─────────────────────────────────────────────────────

bool ChildProcess::start(boost::system::error_code& ec) {
    state_ = ChildState::Starting;

    // 创建 stdout/stderr 管道用于日志捕获
    stdout_pipe_ = std::make_unique<boost::asio::readable_pipe>(ioc_);
    stderr_pipe_ = std::make_unique<boost::asio::readable_pipe>(ioc_);

    // 构建合并的环境变量：当前环境 + ProcessEntry::env
    // process_environment 会替换整个环境，因此必须合并
    std::vector<std::string> full_env;
    auto current_env = process_v2::environment::current();
    for (const auto& var : current_env) {
        full_env.push_back(env_to_narrow(var));
    }
    for (const auto& [k, v] : entry_.env) {
        // 覆盖或追加（Windows 上键名大小写不敏感匹配）
        std::string entry_str = k + "=" + v;
        bool found = false;
        for (auto& existing : full_env) {
            if (env_key_matches(existing, k)) {
                existing = entry_str;
                found = true;
                break;
            }
        }
        if (!found) {
            full_env.push_back(entry_str);
        }
    }

    // 将 std::filesystem::path 转换为 boost::filesystem::path（process v2 要求）
    auto exe = boost::filesystem::path(entry_.exe.native());

    // 使用 default_process_launcher + error_code（非抛出）
    // 注意: start_dir 为空时不得传 process_start_dir — 空路径在 Linux 下被
    // posix_spawn addchdir("") 解析为 ENOENT (Windows 传 NULL 为"继承 cwd"语义,
    // 两平台行为不对称; master 正常拉起路径 start_dir 均非空, 此处为防御)
    process_v2::default_process_launcher launcher;
    std::optional<process_v2::process> proc_opt;
    if (entry_.start_dir.empty()) {
        proc_opt.emplace(launcher(ioc_.get_executor(), ec, exe, entry_.args,
                                  process_v2::process_stdio{nullptr, *stdout_pipe_, *stderr_pipe_},
                                  process_v2::process_environment(full_env)));
    } else {
        auto start_dir = boost::filesystem::path(entry_.start_dir.native());
        proc_opt.emplace(launcher(ioc_.get_executor(), ec, exe, entry_.args,
                                  process_v2::process_stdio{nullptr, *stdout_pipe_, *stderr_pipe_},
                                  process_v2::process_environment(full_env),
                                  process_v2::process_start_dir(start_dir)));
    }

    if (ec) {
        state_ = ChildState::Stopped;
        SPDLOG_ERROR("child start failed | name={} exe={} error=\"{}\"", entry_.name, entry_.exe.string(),
                     dztrader::to_utf8_from_system(ec.message()));
        return false;
    }

    proc_ = std::make_unique<process_v2::process>(std::move(*proc_opt));
    state_ = ChildState::Running;

    SPDLOG_INFO("child started | name={} pid={} category={}", entry_.name, proc_->id(),
                category_str(entry_.category));

    start_pipe_readers();
    return true;
}

// ── 终止 / 取消 ──────────────────────────────────────────────

void ChildProcess::terminate() {
    if (!proc_) {
        return;
    }
    if (state_ == ChildState::Stopped) return;

    state_ = ChildState::Stopping;
    cancel();  // 关闭管道加速清理

    boost::system::error_code ec;
    proc_->terminate(ec);
    if (ec) {
        SPDLOG_WARN("terminate failed | name={} error=\"{}\"", entry_.name, dztrader::to_utf8_from_system(ec.message()));
    } else {
        SPDLOG_INFO("child terminated | name={}", entry_.name);
    }
}

void ChildProcess::cancel() {
    boost::system::error_code ec;
    if (stdout_pipe_) {
        stdout_pipe_->close(ec);
    }
    if (stderr_pipe_) {
        stderr_pipe_->close(ec);
    }
}

// ── 查询 ─────────────────────────────────────────────────────

ChildState ChildProcess::state() const { return state_; }

process_v2::pid_type ChildProcess::pid() const {
    if (!proc_) {
        return 0;
    }
    return proc_->id();
}

const std::string& ChildProcess::name() const { return entry_.name; }

const ProcessEntry& ChildProcess::entry() const { return entry_; }

// ── 管道读取 ─────────────────────────────────────────────────

void ChildProcess::start_pipe_readers() {
    read_stdout_line();
    read_stderr_line();
}

void ChildProcess::read_stdout_line() {
    auto self = shared_from_this();
    boost::asio::async_read_until(
        *stdout_pipe_, boost::asio::dynamic_buffer(stdout_buf_), '\n',
        [self](boost::system::error_code ec, std::size_t bytes_transferred) {
            try {
                auto forward_stdout = [&self](std::string_view buf) {
                    constexpr auto level = spdlog::level::info;
                    constexpr auto prose = "forwarded child stdout";
                    auto logger = dztrader::log::forward_logger();
                    if (logger) {
                        logger->log(kForwardedLoc, level,
                            "{} | child_pid={} child_entry={} child_msg=\"{}\"",
                            prose,
                            self->proc_ ? self->proc_->id() : 0,
                            self->entry_.exe.filename().string(),
                            escape_for_kv(buf));
                    } else {
                        SPDLOG_INFO(
                            "{} | child_pid={} child_entry={} child_msg=\"{}\"",
                            prose,
                            self->proc_ ? self->proc_->id() : 0,
                            self->entry_.exe.filename().string(),
                            escape_for_kv(buf));
                    }
                };
                if (ec) {
                    if (ec != boost::asio::error::eof && ec != boost::asio::error::broken_pipe &&
                        ec != boost::asio::error::operation_aborted) {
                        SPDLOG_WARN("stdout read error | name={} error=\"{}\"", self->entry_.name,
                                    dztrader::to_utf8_from_system(ec.message()));
                    }
                    // 刷新剩余数据
                    if (!self->stdout_buf_.empty()) {
                        forward_stdout(self->stdout_buf_);
                        self->stdout_buf_.clear();
                    }
                    return;
                }

                auto line = self->stdout_buf_.substr(0, bytes_transferred);
                self->stdout_buf_.erase(0, bytes_transferred);

                // 去除尾部换行符
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }

                if (!line.empty()) {
                    forward_stdout(line);
                }

                self->read_stdout_line();
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("stdout read callback error | name={} error=\"{}\"",
                                self->entry_.name, e.what());
                self->stdout_buf_.clear();
            }
        });
}

void ChildProcess::read_stderr_line() {
    auto self = shared_from_this();
    boost::asio::async_read_until(
        *stderr_pipe_, boost::asio::dynamic_buffer(stderr_buf_), '\n',
        [self](boost::system::error_code ec, std::size_t bytes_transferred) {
            try {
                auto forward_stderr = [&self](std::string_view buf) {
                    constexpr auto level = spdlog::level::warn;
                    constexpr auto prose = "forwarded child stderr";
                    auto logger = dztrader::log::forward_logger();
                    if (logger) {
                        logger->log(kForwardedLoc, level,
                            "{} | child_pid={} child_entry={} child_msg=\"{}\"",
                            prose,
                            self->proc_ ? self->proc_->id() : 0,
                            self->entry_.exe.filename().string(),
                            escape_for_kv(buf));
                    } else {
                        SPDLOG_WARN(
                            "{} | child_pid={} child_entry={} child_msg=\"{}\"",
                            prose,
                            self->proc_ ? self->proc_->id() : 0,
                            self->entry_.exe.filename().string(),
                            escape_for_kv(buf));
                    }
                };
                if (ec) {
                    if (ec != boost::asio::error::eof && ec != boost::asio::error::broken_pipe &&
                        ec != boost::asio::error::operation_aborted) {
                        SPDLOG_WARN("stderr read error | name={} error=\"{}\"", self->entry_.name,
                                    dztrader::to_utf8_from_system(ec.message()));
                    }
                    if (!self->stderr_buf_.empty()) {
                        forward_stderr(self->stderr_buf_);
                        self->stderr_buf_.clear();
                    }
                    return;
                }

                auto line = self->stderr_buf_.substr(0, bytes_transferred);
                self->stderr_buf_.erase(0, bytes_transferred);

                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }

                if (!line.empty()) {
                    forward_stderr(line);
                }

                self->read_stderr_line();
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("stderr read callback error | name={} error=\"{}\"",
                                self->entry_.name, e.what());
                self->stderr_buf_.clear();
            }
        });
}

}  // namespace dztrader::master
