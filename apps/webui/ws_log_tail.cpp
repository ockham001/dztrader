#include "ws_log_tail.h"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

namespace dztrader::webui {

namespace {

/// 取 basename（兼容 subdir/name 与 flat name 两种 file 形式）
std::string basename_of(const std::string& file) {
    const auto pos = file.find_last_of("/\\");
    return pos == std::string::npos ? file : file.substr(pos + 1);
}

/// 发送一条 JSON 消息；回调缺失/抛异常/返回 false 均视为发送失败。
bool send_json(const LogTailManager::SendFn& send, const nlohmann::json& msg) {
    if (!send) {
        return false;
    }
    try {
        return send(msg.dump());
    } catch (...) {
        return false;
    }
}

}  // namespace

bool LogTailManager::handle_subscribe(const void* conn_key,
                                      const std::string& file,
                                      uint64_t seq,
                                      const std::string& self_process,
                                      const std::filesystem::path& log_dir,
                                      const SendFn& send) {
    if (file.empty()) {
        send_json(send, {{"type", "error"},
                         {"seq", seq},
                         {"payload", {{"message", "missing file parameter"}}}});
        return false;
    }
    // 拒绝订阅自身日志 (避免自引用死循环)；自身进程名由调用方注入，
    // 支持重命名 dzweb.exe 部署与单测
    if (LogService::extract_logger(basename_of(file)) == self_process) {
        SPDLOG_DEBUG("self log tail rejected | file={} process={}", file, self_process);
        send_json(send, {{"type", "log_tail_unsubscribed"},
                         {"seq", seq},
                         {"payload", {{"file", file}, {"reason", kReasonSelfLog}}}});
        return false;
    }

    Subscription sub;
    sub.file = file;
    sub.send = send;
    // 基线 = 订阅时刻文件末尾（只追新增）：分块数换行，不逐行 parse
    try {
        LogService svc(log_dir);
        sub.cursor = svc.tail_baseline(file);
    } catch (...) {
        // 吞因：基线读取失败则保持 {0,0}，从文件头开始 tail，订阅本身继续
        SPDLOG_DEBUG("log tail baseline read failed, ignored | file={}", file);
    }
    subscriptions_[conn_key] = std::move(sub);

    send_json(send, {{"type", "subscribe_log_ack"}, {"seq", seq}, {"payload", {{"file", file}}}});
    return true;
}

void LogTailManager::handle_unsubscribe(const void* conn_key, uint64_t seq, const SendFn& send) {
    subscriptions_.erase(conn_key);
    // 契约 webui-ws §2.4: unsubscribe_log_ack 载荷为 {}
    send_json(
        send,
        {{"type", "unsubscribe_log_ack"}, {"seq", seq}, {"payload", nlohmann::json::object()}});
}

void LogTailManager::drop(const void* conn_key) { subscriptions_.erase(conn_key); }

void LogTailManager::poll(const std::filesystem::path& log_dir) {
    if (subscriptions_.empty()) {
        return;
    }
    // 拷贝 key：自动退订会 erase 当前项，直接遍历会导致迭代器失效
    std::vector<const void*> keys;
    keys.reserve(subscriptions_.size());
    for (const auto& [key, sub] : subscriptions_) {
        keys.push_back(key);
    }

    try {
        LogService svc(log_dir);
        for (const void* key : keys) {
            auto it = subscriptions_.find(key);
            if (it == subscriptions_.end()) {
                continue;
            }
            Subscription& sub = it->second;

            // 增量读：只读 cursor 之后的字节，不全量重读
            auto content = svc.read_tail(sub.file, sub.cursor, kPollLineLimit);

            int pushed = 0;
            for (const auto& line : content.lines) {
                if (!send_json(sub.send, build_log_line(sub.file, line))) {
                    ++sub.fail_count;
                    break;
                }
                ++pushed;
            }

            // 更新 baseline：游标已由 read_tail 推进；中途 send 失败则回退到已推送位置
            // （行字节长 = raw.size() + 1，含 '\n'），下次从失败处继续
            if (pushed < static_cast<int>(content.lines.size())) {
                for (int i = static_cast<int>(content.lines.size()) - 1; i >= pushed; --i) {
                    sub.cursor.byte_offset -=
                        static_cast<long long>(content.lines[static_cast<size_t>(i)].raw.size()) +
                        1;
                    sub.cursor.line_no -= 1;
                }
            } else if (pushed > 0) {
                sub.fail_count = 0;  // 本轮全部成功，重置连续失败计数
            }

            // 连续失败 3 次自动退订，防止僵尸连接每 500ms 无效 send
            if (sub.fail_count >= kMaxSendFailures) {
                SPDLOG_WARN("auto-unsubscribe log tail | file={} fails={}", sub.file,
                            sub.fail_count);
                const nlohmann::json err = {
                    {"type", "log_tail_unsubscribed"},
                    {"payload", {{"file", sub.file}, {"reason", kReasonSendFailures}}}};
                // 连接已坏（连续 send 失败），退订通知本身也是尽力而为
                send_json(sub.send, err);
                subscriptions_.erase(it);
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("poll_log_tail failed | error={}", e.what());
    }
}

nlohmann::json LogTailManager::build_log_line(const std::string& file, const LogLine& l) {
    return {{"type", "log_line"},
            {"payload",
             {{"file", file},
              {"line",
               {{"n", l.n},
                {"ts", l.ts},
                {"level", l.level},
                {"logger", l.logger},
                {"func", l.func},
                {"file", l.file},
                {"line", l.line_no},
                {"pid", l.pid},
                {"tid", l.tid},
                {"msg", l.msg},
                {"raw", l.raw},
                {"parsed", l.parsed}}}}}};
}

}  // namespace dztrader::webui
