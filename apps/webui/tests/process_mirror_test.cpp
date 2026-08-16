#include "process_mirror.h"
#include <gtest/gtest.h>

namespace dztrader::webui {
namespace {

TEST(ProcessMirrorTest, InitiallyEmpty) {
    const ProcessMirror mirror;
    EXPECT_TRUE(mirror.get_all().empty());
    EXPECT_FALSE(mirror.get_config("foo").has_value());
    EXPECT_FALSE(mirror.get_gateway_status("foo").has_value());
}

TEST(ProcessMirrorTest, UpdateAndGetStatus) {
    ProcessMirror mirror;
    dztrader::platform::ProcessStatus s;
    s.name = "dzmd_ctp";
    s.state = dztrader::platform::ChildState::Running;
    s.pid = 12345;
    s.message = "ok";
    mirror.update_status("dzmd_ctp", s);

    auto all = mirror.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].name, "dzmd_ctp");
    EXPECT_EQ(all[0].state, dztrader::platform::ChildState::Running);
    EXPECT_EQ(all[0].pid, 12345);
}

TEST(ProcessMirrorTest, UpdateAndGetConfig) {
    ProcessMirror mirror;
    const nlohmann::json config = nlohmann::json{{"front_address", "tcp://x:1"}};
    mirror.update_config("dzmd_ctp", config);

    auto got = mirror.get_config("dzmd_ctp");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ((*got)["front_address"], "tcp://x:1");
}

TEST(ProcessMirrorTest, OverwriteExisting) {
    ProcessMirror mirror;
    dztrader::platform::ProcessStatus s1;
    s1.name = "dzmd_ctp";
    s1.state = dztrader::platform::ChildState::Starting;
    mirror.update_status("dzmd_ctp", s1);

    dztrader::platform::ProcessStatus s2;
    s2.name = "dzmd_ctp";
    s2.state = dztrader::platform::ChildState::Running;
    mirror.update_status("dzmd_ctp", s2);

    auto all = mirror.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].state, dztrader::platform::ChildState::Running);
}

TEST(ProcessMirrorTest, Clear) {
    ProcessMirror mirror;
    dztrader::platform::ProcessStatus s;
    s.name = "dzmd_ctp";
    mirror.update_status("dzmd_ctp", s);
    mirror.update_config("dzmd_ctp", nlohmann::json{{"log_level", "info"}});
    ASSERT_EQ(mirror.get_all().size(), 1u);
    ASSERT_TRUE(mirror.get_config("dzmd_ctp").has_value());

    mirror.clear();
    EXPECT_TRUE(mirror.get_all().empty());
    EXPECT_FALSE(mirror.get_config("dzmd_ctp").has_value());
}

TEST(ProcessMirrorTest, UpdateConfigOnly) {
    ProcessMirror mirror;
    const nlohmann::json config = {{"brokers", nlohmann::json::array()}, {"log_level", "info"}};
    mirror.update_config("dzmd_ctp", config);

    auto got = mirror.get_config("dzmd_ctp");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ((*got)["log_level"], "info");
}

TEST(ProcessMirrorTest, UpdateGatewayStatusOnly) {
    ProcessMirror mirror;
    const nlohmann::json status = {{"login_state", "online"}, {"trading_day", "20260704"}};
    mirror.update_gateway_status("dzmd_ctp", status);

    auto got = mirror.get_gateway_status("dzmd_ctp");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ((*got)["login_state"], "online");
    EXPECT_EQ((*got)["trading_day"], "20260704");
}

TEST(ProcessMirrorTest, UpdateConfigThenGatewayStatusIndependent) {
    ProcessMirror mirror;
    const nlohmann::json config = {{"log_level", "debug"}};
    mirror.update_config("dzmd_ctp", config);

    const nlohmann::json status = {{"login_state", "offline"}};
    mirror.update_gateway_status("dzmd_ctp", status);

    auto got_config = mirror.get_config("dzmd_ctp");
    ASSERT_TRUE(got_config.has_value());
    EXPECT_EQ((*got_config)["log_level"], "debug");
    auto got_status = mirror.get_gateway_status("dzmd_ctp");
    ASSERT_TRUE(got_status.has_value());
    EXPECT_EQ((*got_status)["login_state"], "offline");
}

TEST(ProcessMirrorTest, UpdateConfigOverwritesExisting) {
    ProcessMirror mirror;
    const nlohmann::json old_config = {{"old_field", "old_value"}};
    mirror.update_config("dzmd_ctp", old_config);

    const nlohmann::json new_config = {{"new_field", "new_value"}};
    mirror.update_config("dzmd_ctp", new_config);

    auto got = mirror.get_config("dzmd_ctp");
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->contains("old_field"));
    EXPECT_EQ((*got)["new_field"], "new_value");
}

// ---- 契约 process 条目消失 = 进程已移除（Remove 竞态修复回归测试）----

TEST(ProcessMirrorTest, UpdateProcessConfigsEntryDisappearanceCleansInstanceMirror) {
    // Remove 流程帧序: 118(条目消失) 先于 116(Stopped); Stopped 被注册守卫拒绝,
    // 若 118 处理时不清理实例镜像, 残留 process_status=Stopping 会让已移除进程
    // 在 snapshot/REST list 中"复活"。
    ProcessMirror mirror;
    dztrader::platform::ProcessStatus s;
    s.name = "dzmd_ctp";
    s.state = dztrader::platform::ChildState::Stopping;  // RemoveSucceeded 时写入的残留
    mirror.update_status("dzmd_ctp", s);
    mirror.update_config("dzmd_ctp", nlohmann::json{{"brokers", nlohmann::json::array()}});
    mirror.update_gateway_status("dzmd_ctp", nlohmann::json{{"api_version", "v6.7.2"}});
    mirror.update_process_configs(nlohmann::json{
        {"dzmd_ctp", {{"args", nlohmann::json::array()}, {"env", nlohmann::json::object()},
                      {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}}}});
    ASSERT_TRUE(mirror.get_status("dzmd_ctp").has_value());

    // 118 全量覆盖: dzmd_ctp 条目消失 → 实例镜像清理
    mirror.update_process_configs(nlohmann::json::object());
    EXPECT_FALSE(mirror.get_status("dzmd_ctp").has_value());
    EXPECT_FALSE(mirror.get_config("dzmd_ctp").has_value());
    EXPECT_FALSE(mirror.get_gateway_status("dzmd_ctp").has_value());
    EXPECT_TRUE(mirror.get_all().empty());  // list API 不再返回残留 Stopping

    // 其他仍注册的进程不受影响
    mirror.update_status("dzmd_xtp", s);
    mirror.update_process_configs(nlohmann::json{
        {"dzmd_xtp", {{"args", nlohmann::json::array()}, {"env", nlohmann::json::object()},
                      {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}}}});
    EXPECT_TRUE(mirror.get_status("dzmd_xtp").has_value());
}

TEST(ProcessMirrorTest, MarkStaleClearsProgress) {
    // 崩溃/停止后残留旧进度（如崩溃前 "已登录"）会造成前端 loginState 与
    // process_state 矛盾（Crashed + online）, mark_stale 须一并清除 progress 域。
    MirrorStore store;
    ProcessMirror mirror(store);
    dztrader::platform::ProcessStatus s;
    s.name = "dzmd_ctp";
    s.state = dztrader::platform::ChildState::Crashed;
    mirror.update_status("dzmd_ctp", s);
    mirror.update_config("dzmd_ctp", nlohmann::json::object());
    mirror.update_gateway_status("dzmd_ctp", nlohmann::json::object());
    // progress 域由 ProgressDomainService 直接写共享 MirrorStore
    store.update("dzmd_ctp", "progress", nlohmann::json{{"min", 0}, {"max", 4}, {"current", 4}});

    mirror.mark_stale("dzmd_ctp");
    EXPECT_TRUE(mirror.get_status("dzmd_ctp").has_value());  // 保留 status
    EXPECT_FALSE(mirror.get_config("dzmd_ctp").has_value());
    EXPECT_FALSE(mirror.get_gateway_status("dzmd_ctp").has_value());
    // progress 域已清除 (snapshot 中不再有 progress)
    const nlohmann::json& snap = store.snapshot();
    EXPECT_FALSE(snap.at("dzmd_ctp").contains("progress"));
}

}  // namespace
}  // namespace dztrader::webui
