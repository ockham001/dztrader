#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include "log_service.h"

using dztrader::webui::LogService;

namespace fs = std::filesystem;

class LogServiceTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;
    std::unique_ptr<dztrader::webui::LogService> svc_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "dz_log_service_test";
        fs::remove_all(tmp_dir_);
        fs::create_directories(tmp_dir_);
        svc_ = std::make_unique<dztrader::webui::LogService>(tmp_dir_);
    }
    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    void write_log_file(const std::string& name, const std::vector<std::string>& lines) {
        std::ofstream ofs(tmp_dir_ / name);
        for (const auto& l : lines) {
            ofs << l << "\n";
        }
    }
};

TEST_F(LogServiceTest, ListFilesReturnsAllLogFiles) {
    write_log_file("dztraderd.log", {"line1"});
    write_log_file("webui_2026-07-12.log", {"line1"});
    write_log_file("not_a_log.txt", {"x"});

    auto files = svc_->list_files("", "", 30, 0);
    ASSERT_EQ(files.size(), 2u);  // only .log files
}

TEST_F(LogServiceTest, ListFilesFiltersByLogger) {
    write_log_file("dztraderd.log", {"x"});
    write_log_file("webui.log", {"x"});

    auto files = svc_->list_files("dztraderd", "", 30, 0);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].logger, "dztraderd");
}

TEST_F(LogServiceTest, ListFilesPaginates) {
    for (int i = 0; i < 5; ++i) {
        write_log_file("svc" + std::to_string(i) + ".log", {"x"});
    }
    auto page1 = svc_->list_files("", "", 2, 0);
    auto page2 = svc_->list_files("", "", 2, 2);
    ASSERT_EQ(page1.size(), 2u);
    ASSERT_EQ(page2.size(), 2u);
}

TEST_F(LogServiceTest, ReadContentReturnsParsedLines) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.123456789+08:00 info dztraderd [func=main file=apps/master/main.cpp:42 pid=12345 tid=12346] md channel initialized",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=run file=shm_manager.cpp:100 pid=12345 tid=12347] page cleanup slow",
    });

    auto content = svc_->read_content("dztraderd.log", 0, 500, "", "", "", "");
    ASSERT_EQ(content.lines.size(), 2u);
    EXPECT_TRUE(content.lines[0].parsed);
    EXPECT_EQ(content.lines[0].level, "info");
    EXPECT_EQ(content.lines[0].logger, "dztraderd");
    EXPECT_EQ(content.lines[0].func, "main");
    EXPECT_EQ(content.lines[0].file, "apps/master/main.cpp");
    EXPECT_EQ(content.lines[0].line_no, 42);
    EXPECT_EQ(content.lines[0].pid, "12345");
    EXPECT_EQ(content.lines[0].tid, "12346");
    EXPECT_EQ(content.lines[0].msg, "md channel initialized");
}

TEST_F(LogServiceTest, ParseLineWithEmptyFuncFileLine) {
    write_log_file("web.log", {
        "2026-07-16T12:30:14.406320900+08:00 info dzweb [func= file=: pid=67304 tid=67200] shm writer initialized",
    });

    auto content = svc_->read_content("web.log", 0, 500, "", "", "", "");
    ASSERT_EQ(content.lines.size(), 1u);
    EXPECT_TRUE(content.lines[0].parsed);
    EXPECT_EQ(content.lines[0].level, "info");
    EXPECT_EQ(content.lines[0].logger, "dzweb");
    EXPECT_EQ(content.lines[0].func, "");
    EXPECT_EQ(content.lines[0].file, "");
    EXPECT_EQ(content.lines[0].line_no, 0);
    EXPECT_EQ(content.lines[0].pid, "67304");
    EXPECT_EQ(content.lines[0].msg, "shm writer initialized");
}

TEST_F(LogServiceTest, ReadContentUnparsableLineMarkedRaw) {
    write_log_file("strategy.log", {
        "some random text without spdlog format",
    });

    auto content = svc_->read_content("strategy.log", 0, 500, "", "", "", "");
    ASSERT_EQ(content.lines.size(), 1u);
    EXPECT_FALSE(content.lines[0].parsed);
    EXPECT_EQ(content.lines[0].raw, "some random text without spdlog format");
}

TEST_F(LogServiceTest, ReadContentFiltersByLevel) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.123456789+08:00 info dztraderd [func=main file=main.cpp:42 pid=123 tid=124] info msg",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=run file=run.cpp:100 pid=123 tid=125] warn msg",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=run file=run.cpp:101 pid=123 tid=126] error msg",
    });

    // level_filter 按 severity 阈值过滤：选 warning 返回 warning + error（不含 info）
    auto content = svc_->read_content("dztraderd.log", 0, 500, "warning", "", "", "");
    ASSERT_EQ(content.lines.size(), 2u);
    EXPECT_EQ(content.lines[0].level, "warning");
    EXPECT_EQ(content.lines[1].level, "error");
}

TEST_F(LogServiceTest, ReadContentFiltersByKeyword) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.123456789+08:00 info dztraderd [func=main file=main.cpp:42 pid=123 tid=124] order accepted",
        "2026-07-13T14:23:46.000000000+08:00 info dztraderd [func=run file=run.cpp:100 pid=123 tid=125] order rejected",
    });

    auto content = svc_->read_content("dztraderd.log", 0, 500, "", "rejected", "", "");
    ASSERT_EQ(content.lines.size(), 1u);
    EXPECT_NE(content.lines[0].msg.find("rejected"), std::string::npos);
}

TEST_F(LogServiceTest, ReadContentNonexistentFileReturnsEmpty) {
    auto content = svc_->read_content("nonexistent.log", 0, 500, "", "", "", "");
    EXPECT_EQ(content.lines.size(), 0u);
    EXPECT_EQ(content.total, 0);
}

TEST_F(LogServiceTest, GetStatsCountsByLevel) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.123456789+08:00 info dztraderd [func=main file=main.cpp:1 pid=1 tid=2] msg1",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=main file=main.cpp:2 pid=1 tid=3] msg2",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=main file=main.cpp:3 pid=1 tid=4] msg3",
        "2026-07-13T14:23:48.000000000+08:00 info dztraderd [func=main file=main.cpp:4 pid=1 tid=5] msg4",
    });

    auto stats = svc_->get_stats("dztraderd.log", "", "", "");
    EXPECT_EQ(stats.by_level["info"], 2);
    EXPECT_EQ(stats.by_level["warning"], 1);
    EXPECT_EQ(stats.by_level["error"], 1);
    EXPECT_EQ(stats.total, 4);
}

TEST_F(LogServiceTest, GetAggregateGroupsByTemplate) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.123456789+08:00 error dztraderd [func=main file=main.cpp:1 pid=1 tid=2] order rejected: order_id=12345, reason=price_limit",
        "2026-07-13T14:23:46.000000000+08:00 error dztraderd [func=main file=main.cpp:2 pid=1 tid=3] order rejected: order_id=67890, reason=price_limit",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=main file=main.cpp:3 pid=1 tid=4] connection lost: timeout=3000",
        "2026-07-13T14:23:48.000000000+08:00 info dztraderd [func=main file=main.cpp:4 pid=1 tid=5] info msg",
    });

    auto agg = svc_->get_aggregate("dztraderd.log", "error", 20);
    ASSERT_EQ(agg.size(), 2u);  // 2 distinct templates
    // The "order rejected" template should have count=2
    bool found_order = false;
    for (const auto& a : agg) {
        if (a.msg_pattern.find("order rejected") != std::string::npos) {
            EXPECT_EQ(a.count, 2);
            EXPECT_EQ(a.samples.size(), 2u);
            found_order = true;
        }
    }
    EXPECT_TRUE(found_order);
}

TEST_F(LogServiceTest, GetTimelineBucketsByMinute) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:24:10.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
    });

    auto timeline = svc_->get_timeline("dztraderd.log", "minute");
    ASSERT_EQ(timeline.size(), 2u);  // 2 distinct minutes
    EXPECT_EQ(timeline[0].ts, "2026-07-13T14:23");
    EXPECT_EQ(timeline[0].counts["info"], 1);
    EXPECT_EQ(timeline[0].counts["warning"], 1);
    EXPECT_EQ(timeline[1].ts, "2026-07-13T14:24");
    EXPECT_EQ(timeline[1].counts["error"], 1);
}

// ---------------------------------------------------------------------------
// read_tail：偏移量读取，为 ws_controller 的 500ms 轮询优化
// ---------------------------------------------------------------------------

TEST_F(LogServiceTest, ReadTailFromZeroReadsAll) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
    });

    LogService::TailCursor cur;
    auto c = svc_->read_tail("dztraderd.log", cur, 0);  // limit=0(不限)，全新游标从文件头读
    EXPECT_EQ(c.lines.size(), 3u);
    EXPECT_EQ(c.total, 3);
}

TEST_F(LogServiceTest, ReadTailSkipsAlreadyRead) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
        "2026-07-13T14:23:48.000000000+08:00 error dztraderd [func=m file=f.cpp:4 pid=1 tid=5] d",
        "2026-07-13T14:23:49.000000000+08:00 error dztraderd [func=m file=f.cpp:5 pid=1 tid=6] e",
    });

    LogService::TailCursor cur;
    (void)svc_->read_tail("dztraderd.log", cur, 2);  // 先消费前 2 行建立游标
    auto c = svc_->read_tail("dztraderd.log", cur, 0);  // 从第 3 行继续增量读
    EXPECT_EQ(c.lines.size(), 3u);
    EXPECT_EQ(c.lines[0].n, 3);  // 第 3 行开始
    EXPECT_EQ(c.lines[2].n, 5);
    EXPECT_EQ(c.total, 5);
}

TEST_F(LogServiceTest, ReadTailCapsAtLimit) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
        "2026-07-13T14:23:48.000000000+08:00 error dztraderd [func=m file=f.cpp:4 pid=1 tid=5] d",
        "2026-07-13T14:23:49.000000000+08:00 error dztraderd [func=m file=f.cpp:5 pid=1 tid=6] e",
    });

    LogService::TailCursor cur;
    auto c = svc_->read_tail("dztraderd.log", cur, 2);  // limit=2
    EXPECT_EQ(c.lines.size(), 2u);
    EXPECT_EQ(c.total, 2);  // 游标推进到 2（cap 达到，剩余行下次再读）
}

TEST_F(LogServiceTest, ReadTailNoNewLinesReturnsSameBaseline) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
    });

    LogService::TailCursor cur;
    (void)svc_->read_tail("dztraderd.log", cur, 0);  // 已读到第 3 行
    auto c = svc_->read_tail("dztraderd.log", cur, 0);  // 无新增
    EXPECT_EQ(c.lines.size(), 0u);
    EXPECT_EQ(c.total, 3);  // 游标停在 3
}

TEST_F(LogServiceTest, ReadTailDetectsTruncation) {
    // 模拟日志轮转：上次消费时文件较大（游标 byte_offset 落在末尾），
    // 文件被重建为更短内容（字节数 < 已消费偏移）→ 游标重置，从头读
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] ppppppppppppppppppppppppppppppppppppppppppppppppp",
        "2026-07-13T14:23:46.000000000+08:00 info dztraderd [func=m file=f.cpp:2 pid=1 tid=3] ppppppppppppppppppppppppppppppppppppppppppppppppp",
    });
    LogService::TailCursor cur;
    (void)svc_->read_tail("dztraderd.log", cur, 0);  // 消费全部，游标落在文件末尾

    // 重建为更短文件（大小 < 旧游标 offset）
    write_log_file("dztraderd.log", {
        "2026-07-13T14:24:00.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
        "2026-07-13T14:24:01.000000000+08:00 info dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b",
        "2026-07-13T14:24:02.000000000+08:00 info dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c",
    });

    auto c = svc_->read_tail("dztraderd.log", cur, 0);
    EXPECT_EQ(c.lines.size(), 3u);  // 重置后从头读到全部新行
    EXPECT_EQ(c.lines[0].n, 1);
    EXPECT_EQ(c.total, 3);
}

TEST_F(LogServiceTest, ReadTailEmptyFile) {
    write_log_file("dztraderd.log", {});

    LogService::TailCursor cur;
    auto c = svc_->read_tail("dztraderd.log", cur, 0);
    EXPECT_EQ(c.lines.size(), 0u);
    EXPECT_EQ(c.total, 0);
}

TEST_F(LogServiceTest, ReadTailNonexistentFileReturnsEmpty) {
    LogService::TailCursor cur;
    auto c = svc_->read_tail("nonexistent.log", cur, 0);
    EXPECT_EQ(c.lines.size(), 0u);
    EXPECT_EQ(c.total, 0);  // 默认无进展
}

TEST_F(LogServiceTest, ReadTailIncrementalAppends) {
    // 模拟 500ms 轮询场景：文件持续追加
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] a",
    });

    LogService::TailCursor cur;
    auto c1 = svc_->read_tail("dztraderd.log", cur, 0);
    ASSERT_EQ(c1.lines.size(), 1u);
    EXPECT_EQ(c1.total, 1);

    // 追加 2 行
    {
        std::ofstream ofs(tmp_dir_ / "dztraderd.log", std::ios::app);
        ofs << "2026-07-13T14:23:46.000000000+08:00 warning dztraderd [func=m file=f.cpp:2 pid=1 tid=3] b\n";
        ofs << "2026-07-13T14:23:47.000000000+08:00 error dztraderd [func=m file=f.cpp:3 pid=1 tid=4] c\n";
    }

    auto c2 = svc_->read_tail("dztraderd.log", cur, 0);  // 同一游标继续增量读
    EXPECT_EQ(c2.lines.size(), 2u);
    EXPECT_EQ(c2.lines[0].n, 2);
    EXPECT_EQ(c2.lines[1].n, 3);
    EXPECT_EQ(c2.total, 3);
}

// ---------------------------------------------------------------------------
// 正则表达式边界情况：func 名称包含空格
// ---------------------------------------------------------------------------

TEST_F(LogServiceTest, ParseLineWithSpaceInFuncName) {
    // C++ lambda 或 operator() 会产生带空格的 func 名称
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=ns::Class::async_wait::<lambda_1>::operator () file=child_process.h:105 pid=30428 tid=66028] child exited",
        "2026-07-13T14:23:46.000000000+08:00 info dztraderd.fwd [func=forwarded file=external:1 pid=30428 tid=66028] forwarded child stdout",
    });

    auto content = svc_->read_content("dztraderd.log", 0, 500, "", "", "", "");
    ASSERT_EQ(content.lines.size(), 2u);

    // 第 1 行：func=operator () 包含空格，应能正确解析
    EXPECT_TRUE(content.lines[0].parsed) << "Line with space in func name should be parsed";
    EXPECT_EQ(content.lines[0].func, "ns::Class::async_wait::<lambda_1>::operator ()");
    EXPECT_EQ(content.lines[0].file, "child_process.h");
    EXPECT_EQ(content.lines[0].line_no, 105);
    EXPECT_EQ(content.lines[0].logger, "dztraderd");

    // 第 2 行：转发日志（无空格 func），应能正确解析
    EXPECT_TRUE(content.lines[1].parsed);
    EXPECT_EQ(content.lines[1].func, "forwarded");
    EXPECT_EQ(content.lines[1].file, "external");
    EXPECT_EQ(content.lines[1].line_no, 1);
    EXPECT_EQ(content.lines[1].logger, "dztraderd.fwd");
}

// ---------------------------------------------------------------------------
// 递归目录列举 + 子目录路径访问 + 路径安全
// ---------------------------------------------------------------------------

TEST_F(LogServiceTest, ListFilesRecursiveIncludesSubdirectory) {
    fs::create_directories(tmp_dir_ / "dzweb");
    write_log_file("dzweb/dzweb_2026-07-16.log", {"line1"});

    auto files = svc_->list_files("", "", 30, 0);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].name, "dzweb_2026-07-16.log");
    EXPECT_EQ(files[0].logger, "dzweb");
    EXPECT_EQ(files[0].path, "dzweb/dzweb_2026-07-16.log");
}

TEST_F(LogServiceTest, ReadContentFromSubdirectoryPath) {
    fs::create_directories(tmp_dir_ / "dzweb");
    write_log_file("dzweb/dzweb_2026-07-16.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dzweb [func=main file=main.cpp:42 pid=1 tid=2] webui started",
    });

    auto content = svc_->read_content("dzweb/dzweb_2026-07-16.log", 0, 500, "", "", "", "");
    ASSERT_EQ(content.lines.size(), 1u);
    EXPECT_EQ(content.lines[0].logger, "dzweb");
    EXPECT_EQ(content.lines[0].msg, "webui started");
}

TEST_F(LogServiceTest, PathTraversalRejected) {
    write_log_file("dztraderd.log", {"line1"});

    auto content = svc_->read_content("../etc/passwd", 0, 500, "", "", "", "");
    EXPECT_EQ(content.lines.size(), 0u);
    EXPECT_EQ(content.total, 0);
}

TEST_F(LogServiceTest, ReadTailFromSubdirectoryPath) {
    fs::create_directories(tmp_dir_ / "dzweb");
    write_log_file("dzweb/dzweb.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dzweb [func=m file=f.cpp:1 pid=1 tid=2] a",
    });

    LogService::TailCursor cur;
    auto c = svc_->read_tail("dzweb/dzweb.log", cur, 0);
    EXPECT_EQ(c.lines.size(), 1u);
    EXPECT_EQ(c.total, 1);
}

// ---------------------------------------------------------------------------
// from_end 参数：从文件末尾读取最新日志行（seek 优化大文件）
// ---------------------------------------------------------------------------

TEST_F(LogServiceTest, ReadContentFromEndReturnsLastLines) {
    std::vector<std::string> lines;
    for (int i = 1; i <= 10; ++i) {
        // 秒数用 2 位补零，避免 i>=10 时产生 3 位秒数（14:23:410 无效）
        // 缓冲区需 37 字节（36 字符 + null），用 64 防止截断
        char ts[64];
        std::snprintf(ts, sizeof(ts), "2026-07-13T14:23:%02d.000000000+08:00", i);
        lines.push_back(std::string(ts) +
            " info dztraderd [func=m file=f.cpp:" + std::to_string(i) +
            " pid=1 tid=2] line " + std::to_string(i));
    }
    write_log_file("dztraderd.log", lines);

    auto content = svc_->read_content("dztraderd.log", 0, 3, "", "", "", "", true);
    ASSERT_EQ(content.lines.size(), 3u);
    EXPECT_EQ(content.lines[0].msg, "line 8");
    EXPECT_EQ(content.lines[1].msg, "line 9");
    EXPECT_EQ(content.lines[2].msg, "line 10");
    EXPECT_EQ(content.total, 10);
}

TEST_F(LogServiceTest, ReadContentFromEndWithFilter) {
    write_log_file("dztraderd.log", {
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] info 1",
        "2026-07-13T14:23:46.000000000+08:00 error dztraderd [func=m file=f.cpp:2 pid=1 tid=3] error 1",
        "2026-07-13T14:23:47.000000000+08:00 info dztraderd [func=m file=f.cpp:3 pid=1 tid=4] info 2",
        "2026-07-13T14:23:48.000000000+08:00 error dztraderd [func=m file=f.cpp:4 pid=1 tid=5] error 2",
        "2026-07-13T14:23:49.000000000+08:00 info dztraderd [func=m file=f.cpp:5 pid=1 tid=6] info 3",
    });

    auto content = svc_->read_content("dztraderd.log", 0, 1, "error", "", "", "", true);
    ASSERT_EQ(content.lines.size(), 1u);
    EXPECT_EQ(content.lines[0].msg, "error 2");
    EXPECT_EQ(content.total, 2);
}

TEST_F(LogServiceTest, ReadContentFromEndLargeFileSeekOptimized) {
    const std::string prefix =
        "2026-07-13T14:23:45.000000000+08:00 info dztraderd [func=m file=f.cpp:1 pid=1 tid=2] ";
    const std::string padding(150, 'x');

    std::ofstream ofs(tmp_dir_ / "dztraderd.log");
    for (int i = 1; i <= 10000; ++i) {
        ofs << prefix << padding << " n=" << i << "\n";
    }
    ofs.close();

    auto file_size = fs::file_size(tmp_dir_ / "dztraderd.log");
    ASSERT_GT(file_size, 1048576u);

    auto content = svc_->read_content("dztraderd.log", 0, 5, "", "", "", "", true);
    ASSERT_EQ(content.lines.size(), 5u);
    for (const auto& line : content.lines) {
        EXPECT_TRUE(line.parsed);
    }
    EXPECT_EQ(content.total, 10000);
}

// ---------------------------------------------------------------------------
// TailCursor 增量读（I4-Phase1：字节偏移游标替代全文件重读）
// ---------------------------------------------------------------------------

TEST(LogServiceTailCursor, IncrementalReadAdvancesCursor) {
    const fs::path tmp = fs::temp_directory_path() / "dz_tail_cursor_inc";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    LogService svc(tmp);

    {
        std::ofstream ofs(tmp / "a.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
    }

    // 全新游标从文件头读：1 行，游标推进
    LogService::TailCursor cur;
    auto c1 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c1.lines.size(), 1u);
    EXPECT_EQ(c1.lines[0].n, 1);
    EXPECT_EQ(c1.lines[0].parsed, true);  // Windows text 模式 ofstream 写入 CRLF，剥 \r 后应能解析
    EXPECT_GT(cur.byte_offset, 0);
    EXPECT_EQ(cur.line_no, 1);
    EXPECT_EQ(c1.total, 1);

    // 追加 1 行后再读：只返回新增行，行号连续
    {
        std::ofstream ofs(tmp / "a.log", std::ios::app);
        ofs << "2026-07-13T14:23:46.000000000+08:00 warning t [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
    }
    auto c2 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c2.lines.size(), 1u);
    EXPECT_EQ(c2.lines[0].n, 2);
    EXPECT_EQ(c2.lines[0].parsed, true);
    EXPECT_EQ(c2.total, 2);

    // 无新增：0 行，游标不变
    auto c3 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c3.lines.size(), 0u);
    EXPECT_EQ(c3.total, 2);

    fs::remove_all(tmp);
}

TEST(LogServiceTailCursor, PartialLineWithoutNewlineNotConsumed) {
    const fs::path tmp = fs::temp_directory_path() / "dz_tail_cursor_partial";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    LogService svc(tmp);

    {
        std::ofstream ofs(tmp / "a.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
    }
    LogService::TailCursor cur;
    (void)svc.read_tail("a.log", cur, 200);  // 消费 line1
    const auto offset_after_line1 = cur.byte_offset;

    // 追加半行（无换行符，模拟 spdlog 正在写入）
    {
        std::ofstream ofs(tmp / "a.log", std::ios::app);
        ofs << "2026-07-13T14:23:46.000";  // 无 '\n'
    }
    auto c = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c.lines.size(), 0u);                       // 半行不推送
    EXPECT_EQ(cur.byte_offset, offset_after_line1);      // 偏移不推进
    EXPECT_EQ(cur.line_no, 1);

    // 补齐换行后可读到完整行
    {
        std::ofstream ofs(tmp / "a.log", std::ios::app);
        ofs << "000000+08:00 info t [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
    }
    auto c2 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c2.lines.size(), 1u);
    EXPECT_EQ(c2.lines[0].n, 2);
    EXPECT_EQ(c2.lines[0].parsed, true);

    fs::remove_all(tmp);
}

TEST(LogServiceTailCursor, TruncatedFileResetsCursor) {
    const fs::path tmp = fs::temp_directory_path() / "dz_tail_cursor_trunc";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    LogService svc(tmp);

    {
        std::ofstream ofs(tmp / "a.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
        ofs << "2026-07-13T14:23:46.000000000+08:00 info t [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
    }
    LogService::TailCursor cur;
    (void)svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(cur.line_no, 2);

    // 轮换/重写为更短文件（size < 已消费偏移）→ 游标重置，从头读
    {
        std::ofstream ofs(tmp / "a.log", std::ios::trunc);
        ofs << "2026-07-13T15:00:00.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] new1\n";
    }
    auto c = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c.lines.size(), 1u);
    EXPECT_EQ(c.lines[0].n, 1);   // 新文件行号重新累计
    EXPECT_EQ(c.lines[0].parsed, true);
    EXPECT_EQ(c.total, 1);

    fs::remove_all(tmp);
}

TEST(LogServiceTailCursor, BaselineSkipsExistingContent) {
    const fs::path tmp = fs::temp_directory_path() / "dz_tail_cursor_base";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    LogService svc(tmp);

    {
        std::ofstream ofs(tmp / "a.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
        ofs << "2026-07-13T14:23:46.000000000+08:00 info t [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
    }

    // 基线 = 订阅时刻文件末尾：既有内容不推，只追新增
    auto cur = svc.tail_baseline("a.log");
    EXPECT_EQ(cur.line_no, 2);
    auto c1 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c1.lines.size(), 0u);

    {
        std::ofstream ofs(tmp / "a.log", std::ios::app);
        ofs << "2026-07-13T14:23:47.000000000+08:00 info t [func=m file=f.cpp:3 pid=1 tid=4] line3\n";
    }
    auto c2 = svc.read_tail("a.log", cur, 200);
    EXPECT_EQ(c2.lines.size(), 1u);
    EXPECT_EQ(c2.lines[0].n, 3);
    EXPECT_EQ(c2.lines[0].parsed, true);

    fs::remove_all(tmp);
}

TEST(LogServiceTailCursor, CrLfLineParsedTrue) {
    const fs::path tmp = fs::temp_directory_path() / "dz_tail_cursor_crlf";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    LogService svc(tmp);

    // 用 binary 模式显式写 CRLF 行尾，保证跨平台（Linux/macOS 亦复现 Windows CRLF 场景）
    const std::string line1 =
        "2026-07-13T14:23:45.000000000+08:00 info t [func=m file=f.cpp:1 pid=1 tid=2] line1\r\n";
    const std::string line2 =
        "2026-07-13T14:23:46.000000000+08:00 warning t [func=m file=f.cpp:2 pid=1 tid=3] line2\r\n";
    {
        std::ofstream ofs(tmp / "a.log", std::ios::binary);
        ofs << line1;
    }

    // 全新游标读 CRLF 文件：剥行尾 \r 后 parsed=true，msg 不含 \r
    LogService::TailCursor cur;
    auto c1 = svc.read_tail("a.log", cur, 200);
    ASSERT_EQ(c1.lines.size(), 1u);
    EXPECT_EQ(c1.lines[0].parsed, true);
    EXPECT_EQ(c1.lines[0].msg, "line1");
    EXPECT_EQ(c1.lines[0].n, 1);
    EXPECT_EQ(cur.line_no, 1);
    // 游标偏移按剥前长度（含 \r\n 两字节）精确推进
    EXPECT_EQ(cur.byte_offset, static_cast<long long>(line1.size()));

    // 追加第二行后继续读：偏移持续精确推进，parsed=true
    {
        std::ofstream ofs(tmp / "a.log", std::ios::app | std::ios::binary);
        ofs << line2;
    }
    auto c2 = svc.read_tail("a.log", cur, 200);
    ASSERT_EQ(c2.lines.size(), 1u);
    EXPECT_EQ(c2.lines[0].parsed, true);
    EXPECT_EQ(c2.lines[0].msg, "line2");
    EXPECT_EQ(c2.lines[0].n, 2);
    EXPECT_EQ(cur.line_no, 2);
    EXPECT_EQ(cur.byte_offset, static_cast<long long>(line1.size() + line2.size()));

    fs::remove_all(tmp);
}
