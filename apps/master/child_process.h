#ifndef DZTRADER_MASTER_CHILD_PROCESS_H_
#define DZTRADER_MASTER_CHILD_PROCESS_H_

/**
 * @file child_process.h
 * @brief 单个子进程生命周期管理。
 *
 * 封装 boost::process::v2::process，使用 default_process_launcher +
 * error_code（非抛出）。合并当前环境变量与 ProcessEntry::env，
 * 因为 process_environment 会替换整个环境。
 *
 * 生命周期安全：继承 enable_shared_from_this，所有异步回调
 * 捕获 shared_from_this() 防止 use-after-free。
 * 必须通过 create() 工厂函数创建。
 */

#include "config.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <functional>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace dztrader::master {

namespace process_v2 = boost::process::v2;

// ChildState 位于 platform 库, 这里用 alias 保持 master 命名空间内的简短引用
using ChildState = dztrader::platform::ChildState;

class ChildProcess : public std::enable_shared_from_this<ChildProcess> {
public:
    /// 工厂函数：创建 shared_ptr<ChildProcess>，禁止直接构造。
    static std::shared_ptr<ChildProcess> create(
        boost::asio::io_context& ioc, ProcessEntry entry);

    /// 启动子进程（非抛出，使用 default_process_launcher）。
    /// 成功返回 true，失败返回 false（ec 被设置）。
    bool start(boost::system::error_code& ec);

    /// 强制终止子进程。
    /// 将状态设为 stopping，async_wait 回调负责设为 stopped。
    void terminate();

    /// 取消待处理的管道读取（在 shutdown/terminate 时调用）。
    void cancel();

    /// 异步等待子进程退出。Handler 签名：void(error_code, int exit_code)。
    /// 捕获 shared_from_this() 以保持 *this 存活直到回调完成。
    template<typename Handler>
    void async_wait(Handler&& handler);

    /// 状态查询
    ChildState state() const;
    process_v2::pid_type pid() const;
    const std::string& name() const;
    const ProcessEntry& entry() const;

private:
    ChildProcess(boost::asio::io_context& ioc, ProcessEntry entry);

    void start_pipe_readers();
    void read_stdout_line();
    void read_stderr_line();

    boost::asio::io_context& ioc_;
    ProcessEntry entry_;
    std::unique_ptr<process_v2::process> proc_;
    ChildState state_ = ChildState::Stopped;

    // stdout/stderr 管道，用于异步日志捕获
    std::unique_ptr<boost::asio::readable_pipe> stdout_pipe_;
    std::unique_ptr<boost::asio::readable_pipe> stderr_pipe_;
    std::string stdout_buf_;
    std::string stderr_buf_;
};

// ── 模板实现 ─────────────────────────────────────────────────

template<typename Handler>
void ChildProcess::async_wait(Handler&& handler) {
    if (!proc_) {
        boost::system::error_code ec;
        handler(ec, -1);
        return;
    }

    auto self = shared_from_this();
    proc_->async_wait(
        [self, h = std::forward<Handler>(handler)](
            boost::system::error_code ec, int exit_code) mutable {
            self->state_ = ChildState::Stopped;
            if (ec) {
                SPDLOG_WARN("async_wait error | name={} error=\"{}\"",
                            self->entry_.name, ec.message());
            } else {
                SPDLOG_INFO("child exited | name={} exit_code={}",
                            self->entry_.name, exit_code);
            }
            try {
                h(ec, ec ? -1 : exit_code);
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("async_wait handler error | name={} error=\"{}\"",
                                self->entry_.name, e.what());
            }
        });
}

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_CHILD_PROCESS_H_
