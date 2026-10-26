#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "test_temp_dir.h"
#include "ws_log_tail.h"

namespace fs = std::filesystem;

namespace {

using dztrader::test::unique_temp_dir;
using dztrader::webui::LogTailManager;

class WsLogTailManagerTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;
    LogTailManager mgr_;
    std::vector<std::string> attempts_;  // 每次 send 调用（含失败）
    std::vector<std::string> sent_;      // 成功发送
    int send_calls_ = 0;
    /// 第 N 次 send 调用起失败（默认永不失败）
    int fail_from_call_ = std::numeric_limits<int>::max();

    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_ws_log_tail_test");
        fs::remove_all(tmp_dir_);
        fs::create_directories(tmp_dir_);
    }
    void TearDown() override { fs::remove_all(tmp_dir_); }

    LogTailManager::SendFn send_fn() {
        return [this](const std::string& msg) {
            attempts_.push_back(msg);
            if (send_calls_++ >= fail_from_call_) {
                return false;
            }
            sent_.push_back(msg);
            return true;
        };
    }

    void write_lines(const std::string& name, const std::vector<std::string>& lines) {
        const fs::path path = tmp_dir_ / name;
        fs::create_directories(path.parent_path());
        // binary: 与生产日志字节一致（LF）。Windows 文本模式会把 \n 转成 \r\n，
        // 改变游标字节偏移（回退按 raw.size()+1 计算）并污染 raw 断言
        std::ofstream ofs(path, std::ios::binary);
        for (const auto& l : lines) {
            ofs << l << "\n";
        }
    }
    void append_lines(const std::string& name, const std::vector<std::string>& lines) {
        std::ofstream ofs(tmp_dir_ / name, std::ios::app | std::ios::binary);
        for (const auto& l : lines) {
            ofs << l << "\n";
        }
    }

    /// spdlog 默认格式单行日志
    static std::string log_line(int n,
                                const std::string& level,
                                const std::string& logger,
                                const std::string& msg) {
        return "2026-07-13T14:23:45.000000000+08:00 " + level + " " + logger +
               " [func=main file=main.cpp:" + std::to_string(n) + " pid=1 tid=2] " + msg;
    }

    static nlohmann::json parse(const std::string& raw) { return nlohmann::json::parse(raw); }
};

// ---- subscribe_log 校验（契约 webui-ws §3）----

TEST_F(WsLogTailManagerTest, SubscribeRejectsMissingFile) {
    int key = 0;
    const bool subscribed = mgr_.handle_subscribe(&key, "", 7, "dzweb", tmp_dir_, send_fn());

    EXPECT_FALSE(subscribed);
    EXPECT_FALSE(mgr_.has_subscriptions());
    ASSERT_EQ(attempts_.size(), 1u);
    const auto msg = parse(attempts_[0]);
    EXPECT_EQ(msg["type"], "error");
    EXPECT_EQ(msg["seq"], 7);
    EXPECT_EQ(msg["payload"]["message"], "missing file parameter");
}

TEST_F(WsLogTailManagerTest, SubscribeRejectsSelfLogWithReason) {
    // 自身进程日志（flat 与 subdir 两种路径）均拒绝；契约 §2.3 reason 固定
    for (const std::string file : {"dzweb.log", "sub/dzweb_2026-07-13.log"}) {
        attempts_.clear();
        int key = 0;
        EXPECT_FALSE(mgr_.handle_subscribe(&key, file, 8, "dzweb", tmp_dir_, send_fn())) << file;
        ASSERT_EQ(attempts_.size(), 1u) << file;
        const auto msg = parse(attempts_[0]);
        EXPECT_EQ(msg["type"], "log_tail_unsubscribed") << file;
        EXPECT_EQ(msg["seq"], 8) << file;
        EXPECT_EQ(msg["payload"]["file"], file) << file;
        EXPECT_EQ(msg["payload"]["reason"], "self_logs_cannot_be_tailed") << file;
        EXPECT_FALSE(mgr_.has_subscriptions()) << file;
    }
    // 非自身进程日志正常订阅
    write_lines("dztraderd.log", {log_line(1, "info", "dztraderd", "x")});
    int key = 0;
    EXPECT_TRUE(mgr_.handle_subscribe(&key, "dztraderd.log", 9, "dzweb", tmp_dir_, send_fn()));
    EXPECT_TRUE(mgr_.has_subscriptions());
}

TEST_F(WsLogTailManagerTest, SubscribeAcksAndBaselinesAtFileEnd) {
    write_lines("test.log", {log_line(1, "info", "test", "old")});

    int key = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "test.log", 11, "dzweb", tmp_dir_, send_fn()));
    ASSERT_EQ(attempts_.size(), 1u);
    const auto ack = parse(attempts_[0]);
    EXPECT_EQ(ack["type"], "subscribe_log_ack");
    EXPECT_EQ(ack["seq"], 11);
    EXPECT_EQ(ack["payload"]["file"], "test.log");

    // 基线 = 订阅时刻文件末尾：已有内容不推送
    mgr_.poll(tmp_dir_);
    EXPECT_EQ(attempts_.size(), 1u);

    // 追加后只推送新增行
    append_lines("test.log", {log_line(2, "warning", "test", "fresh")});
    mgr_.poll(tmp_dir_);
    ASSERT_EQ(attempts_.size(), 2u);
    const auto msg = parse(attempts_[1]);
    EXPECT_EQ(msg["type"], "log_line");
    EXPECT_EQ(msg["payload"]["file"], "test.log");
    EXPECT_EQ(msg["payload"]["line"]["n"], 2);
    EXPECT_EQ(msg["payload"]["line"]["msg"], "fresh");
}

// ---- log_line 单行格式（契约 §2.3 字段集）----

TEST_F(WsLogTailManagerTest, LogLineCarriesFullContractFieldSet) {
    int key = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "test_content.log", 0, "dzweb", tmp_dir_, send_fn()));
    const std::string raw = log_line(3, "info", "test_content", "hello");
    append_lines("test_content.log", {raw});

    mgr_.poll(tmp_dir_);
    ASSERT_EQ(attempts_.size(), 2u);
    const auto msg = parse(attempts_[1]);
    EXPECT_EQ(msg["type"], "log_line");
    EXPECT_EQ(msg["payload"]["file"], "test_content.log");

    const auto& line = msg["payload"]["line"];
    EXPECT_EQ(line["n"], 1);
    EXPECT_EQ(line["ts"], "2026-07-13T14:23:45.000000000+08:00");
    EXPECT_EQ(line["level"], "info");
    EXPECT_EQ(line["logger"], "test_content");
    EXPECT_EQ(line["func"], "main");
    EXPECT_EQ(line["file"], "main.cpp");
    EXPECT_EQ(line["line"], 3);
    EXPECT_EQ(line["pid"], "1");
    EXPECT_EQ(line["tid"], "2");
    EXPECT_EQ(line["msg"], "hello");
    EXPECT_EQ(line["raw"], raw);
    EXPECT_EQ(line["parsed"], true);
}

// ---- 单轮 200 行上限（契约 §2.3）----

TEST_F(WsLogTailManagerTest, PollCapsAt200LinesPerRound) {
    int key = 0;
    // 订阅时文件不存在 -> 基线 {0,0}，从文件头 tail
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "burst.log", 0, "dzweb", tmp_dir_, send_fn()));
    attempts_.clear();
    std::vector<std::string> lines;
    lines.reserve(250);
    for (int i = 0; i < 250; ++i) {
        lines.push_back(log_line(i, "info", "burst", "m" + std::to_string(i)));
    }
    write_lines("burst.log", lines);

    mgr_.poll(tmp_dir_);
    EXPECT_EQ(attempts_.size(), 200u);
    EXPECT_EQ(parse(attempts_.front())["payload"]["line"]["n"], 1);
    EXPECT_EQ(parse(attempts_.back())["payload"]["line"]["n"], 200);

    mgr_.poll(tmp_dir_);
    EXPECT_EQ(attempts_.size(), 250u);
    EXPECT_EQ(parse(attempts_.back())["payload"]["line"]["n"], 250);

    mgr_.poll(tmp_dir_);  // 无新增
    EXPECT_EQ(attempts_.size(), 250u);
}

// ---- send 失败：游标回退，下一轮从失败行重发 ----

TEST_F(WsLogTailManagerTest, PollRollsBackCursorToFirstUnsentLine) {
    int key = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "rollback.log", 0, "dzweb", tmp_dir_, send_fn()));
    // 重置发送计数：subscribe 的 ack 已消耗一次 send 调用
    send_calls_ = 0;
    attempts_.clear();
    sent_.clear();
    // 第一轮：第 2 次 send 失败（第 1 行成功）
    fail_from_call_ = 1;
    append_lines("rollback.log",
                 {log_line(1, "info", "rb", "one"), log_line(2, "info", "rb", "two"),
                  log_line(3, "info", "rb", "three")});
    mgr_.poll(tmp_dir_);
    ASSERT_EQ(sent_.size(), 1u);
    ASSERT_EQ(attempts_.size(), 2u);
    EXPECT_EQ(parse(sent_[0])["payload"]["line"]["msg"], "one");

    // 第二轮恢复发送：从失败行继续，不重发已成功行
    fail_from_call_ = std::numeric_limits<int>::max();
    sent_.clear();
    attempts_.clear();
    mgr_.poll(tmp_dir_);
    ASSERT_EQ(sent_.size(), 2u);
    EXPECT_EQ(parse(sent_[0])["payload"]["line"]["msg"], "two");
    EXPECT_EQ(parse(sent_[0])["payload"]["line"]["n"], 2);
    EXPECT_EQ(parse(sent_[1])["payload"]["line"]["msg"], "three");
    EXPECT_EQ(parse(sent_[1])["payload"]["line"]["n"], 3);
}

// ---- 连续 3 次 send 失败自动退订（契约 §2.3 reason=send failures）----

TEST_F(WsLogTailManagerTest, PollAutoUnsubscribesAfterThreeConsecutiveFailures) {
    int key = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "dead.log", 0, "dzweb", tmp_dir_, send_fn()));
    attempts_.clear();
    append_lines("dead.log", {log_line(1, "info", "dead", "one")});
    fail_from_call_ = 0;  // 始终失败

    mgr_.poll(tmp_dir_);
    EXPECT_TRUE(mgr_.has_subscriptions());
    mgr_.poll(tmp_dir_);
    EXPECT_TRUE(mgr_.has_subscriptions());
    mgr_.poll(tmp_dir_);

    // 3 次 log_line 尝试 + 1 次退订通知尝试
    ASSERT_EQ(attempts_.size(), 4u);
    const auto last = parse(attempts_.back());
    EXPECT_EQ(last["type"], "log_tail_unsubscribed");
    EXPECT_EQ(last["payload"]["file"], "dead.log");
    EXPECT_EQ(last["payload"]["reason"], "send failures");
    EXPECT_FALSE(mgr_.has_subscriptions());

    // 退订后不再轮询该连接
    mgr_.poll(tmp_dir_);
    EXPECT_EQ(attempts_.size(), 4u);
}

// ---- unsubscribe_log / 连接关闭 ----

TEST_F(WsLogTailManagerTest, UnsubscribeLogClearsAndAcks) {
    int key = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key, "test.log", 0, "dzweb", tmp_dir_, send_fn()));
    EXPECT_TRUE(mgr_.has_subscriptions());
    attempts_.clear();

    mgr_.handle_unsubscribe(&key, 42, send_fn());
    ASSERT_EQ(attempts_.size(), 1u);
    const auto ack = parse(attempts_[0]);
    EXPECT_EQ(ack["type"], "unsubscribe_log_ack");
    EXPECT_EQ(ack["seq"], 42);
    // 契约 webui-ws §2.4: payload = {}
    ASSERT_TRUE(ack.contains("payload"));
    EXPECT_TRUE(ack["payload"].is_object());
    EXPECT_TRUE(ack["payload"].empty());
    EXPECT_FALSE(mgr_.has_subscriptions());

    mgr_.poll(tmp_dir_);
    EXPECT_EQ(attempts_.size(), 1u);
}

TEST_F(WsLogTailManagerTest, DropRemovesOnlyClosedConnection) {
    int key_a = 0;
    int key_b = 0;
    ASSERT_TRUE(mgr_.handle_subscribe(&key_a, "a.log", 0, "dzweb", tmp_dir_, send_fn()));
    ASSERT_TRUE(mgr_.handle_subscribe(&key_b, "b.log", 0, "dzweb", tmp_dir_, send_fn()));

    write_lines("a.log", {log_line(1, "info", "a", "from-a")});
    write_lines("b.log", {log_line(1, "info", "b", "from-b")});
    mgr_.drop(&key_a);

    // 只保留另一连接的推送（行为断言，不依赖内部计数）
    attempts_.clear();
    mgr_.poll(tmp_dir_);
    ASSERT_EQ(attempts_.size(), 1u);
    EXPECT_EQ(parse(attempts_[0])["payload"]["file"], "b.log");
    EXPECT_EQ(parse(attempts_[0])["payload"]["line"]["msg"], "from-b");
}

}  // namespace
