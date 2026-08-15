#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "log_service.h"

namespace fs = std::filesystem;
using dztrader::webui::LogService;

TEST(WsLogTailTest, LogServiceReadsAppendedContent) {
    const fs::path tmp = fs::temp_directory_path() / "dz_ws_tail_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    LogService svc(tmp);

    // Write initial content
    {
        std::ofstream ofs(tmp / "test.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info test [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
    }

    auto c1 = svc.read_content("test.log", 0, 500, "", "", "", "");
    EXPECT_EQ(c1.lines.size(), 1u);

    // Append a line
    {
        std::ofstream ofs(tmp / "test.log", std::ios::app);
        ofs << "2026-07-13T14:23:46.000000000+08:00 warning test [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
    }

    // Read again — should see 2 lines
    auto c2 = svc.read_content("test.log", 0, 500, "", "", "", "");
    EXPECT_EQ(c2.lines.size(), 2u);

    fs::remove_all(tmp);
}

TEST(WsLogTailTest, LogServiceReadsFromOffset) {
    const fs::path tmp = fs::temp_directory_path() / "dz_ws_tail_offset_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    LogService svc(tmp);

    {
        std::ofstream ofs(tmp / "test.log");
        ofs << "2026-07-13T14:23:45.000000000+08:00 info test [func=m file=f.cpp:1 pid=1 tid=2] line1\n";
        ofs << "2026-07-13T14:23:46.000000000+08:00 info test [func=m file=f.cpp:2 pid=1 tid=3] line2\n";
        ofs << "2026-07-13T14:23:47.000000000+08:00 info test [func=m file=f.cpp:3 pid=1 tid=4] line3\n";
    }

    // Read from offset 2 (skip first 2 lines)
    auto c = svc.read_content("test.log", 2, 500, "", "", "", "");
    EXPECT_EQ(c.lines.size(), 1u);
    EXPECT_EQ(c.lines[0].n, 3);

    fs::remove_all(tmp);
}
