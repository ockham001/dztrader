#ifndef DZTRADER_WEBUI_WS_LOG_TAIL_H_
#define DZTRADER_WEBUI_WS_LOG_TAIL_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "log_service.h"

namespace dztrader::webui {

/// 日志 tail 订阅管理（契约 webui-ws §3 subscribe_log/unsubscribe_log、§2.3
/// log_line/log_tail_unsubscribed）。从 WsController 提取：WsController 依赖 drogon
/// 连接对象无法脱离框架实例化，故把文件/游标/失败退订状态机独立成不依赖 drogon 的类
/// （连接身份为不透明 key，发送走注入回调）；WsController 负责连接生命周期、seq 回填
/// 之外的协议封装与 tail 定时器启停。
class LogTailManager {
public:
    /// 单连接消息发送回调；返回 false 表示发送失败（计入连续失败数）。
    using SendFn = std::function<bool(const std::string& message)>;

    static constexpr int kPollLineLimit = 200;  ///< 单轮单连接推送上限（契约 §2.3）
    static constexpr int kMaxSendFailures = 3;  ///< 连续发送失败自动退订阈值（契约 §2.3）
    static constexpr const char* kReasonSelfLog = "self_logs_cannot_be_tailed";
    static constexpr const char* kReasonSendFailures = "send failures";

    /// 处理 subscribe_log：校验（file 非空、拒绝订阅自身日志）→ 建立订阅（基线 =
    /// 订阅时刻文件末尾，只追新增）→ 回 subscribe_log_ack；校验失败回 error /
    /// log_tail_unsubscribed。重订阅覆盖旧订阅并重置失败计数。
    /// @param conn_key     连接身份（不透明，仅作 key）
    /// @param file         payload.file
    /// @param seq          消息 seq（ack/error 回填）
    /// @param self_process 当前后端进程名（生产 = this_process::exe_stem()）
    /// @param log_dir      日志目录
    /// @param send         发送回调
    /// @return true = 订阅已建立（调用方应确保 tail 定时器运行）
    bool handle_subscribe(const void* conn_key,
                          const std::string& file,
                          uint64_t seq,
                          const std::string& self_process,
                          const std::filesystem::path& log_dir,
                          const SendFn& send);

    /// 处理 unsubscribe_log：清除订阅并回 unsubscribe_log_ack。
    void handle_unsubscribe(const void* conn_key, uint64_t seq, const SendFn& send);

    /// 连接关闭：丢弃该连接订阅（无 ack）。
    void drop(const void* conn_key);

    /// 是否仍有任意订阅（WsController 据此启停 tail 定时器）。
    bool has_subscriptions() const { return !subscriptions_.empty(); }

    /// 单轮轮询：增量读取并逐行推送 log_line（单轮单连接上限 kPollLineLimit 行）。
    /// 推送中途失败时游标回退到已推送位置（下轮从失败行重发）；连续
    /// kMaxSendFailures 轮发送失败自动退订并尝试推 log_tail_unsubscribed
    /// （reason = send failures）。
    void poll(const std::filesystem::path& log_dir);

private:
    struct Subscription {
        std::string file;
        LogService::TailCursor cursor;
        int fail_count = 0;
        SendFn send;
    };

    /// log_line 单行消息（契约 webui-ws §2.3 字段集）
    static nlohmann::json build_log_line(const std::string& file, const LogLine& line);

    std::map<const void*, Subscription> subscriptions_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_WS_LOG_TAIL_H_
