#include <gtest/gtest.h>
#include "ws_control_precheck.h"

#include <nlohmann/json.hpp>

namespace dztrader::webui {
namespace {

using Opt = std::optional<platform::ChildState>;

// 共享守卫纯函数（control_guard.h）：REST/WS 双通道唯一决策处，顺序 admin → source → writer → 镜像 Running
TEST(ControlGuardTest, SharedGuardOrderAndResults) {
    // admin 未通过 -> NotAdmin
    EXPECT_EQ(evaluate_control_guard(false, true, true, true), ControlGuard::NotAdmin);
    // source 缺失 -> SourceInvalid
    EXPECT_EQ(evaluate_control_guard(true, false, true, true), ControlGuard::SourceInvalid);
    // writer 不可用 -> ChannelUnavailable（放行 source，阻塞通道）
    EXPECT_EQ(evaluate_control_guard(true, true, false, true), ControlGuard::ChannelUnavailable);
    // 进程镜像非 Running -> ProcessNotRunning
    EXPECT_EQ(evaluate_control_guard(true, true, true, false), ControlGuard::ProcessNotRunning);
    // 全部通过 -> Ok
    EXPECT_EQ(evaluate_control_guard(true, true, true, true), ControlGuard::Ok);
    // 多路同时失败：admin 优先于其余（守卫的短路顺序锚定第一失败）
    EXPECT_EQ(evaluate_control_guard(false, false, false, false), ControlGuard::NotAdmin);
}

// 契约 webui-ws §3（修订）: 守卫顺序 admin -> source/writer -> 镜像 Running
TEST(WsControlPrecheckTest, MdConnectPrecheckOrderAndResults) {
    EXPECT_EQ(evaluate_md_connect_precheck(false, "dzmd_ctp", true, Opt(platform::ChildState::Running)),
              MdConnectPrecheck::NotAdmin);
    EXPECT_EQ(evaluate_md_connect_precheck(true, "", true, Opt(platform::ChildState::Running)),
              MdConnectPrecheck::InvalidSource);
    EXPECT_EQ(evaluate_md_connect_precheck(true, "dzmd_ctp", false, Opt(platform::ChildState::Running)),
              MdConnectPrecheck::InvalidSource);
    // 镜像未就绪（nullopt）保守拒绝；非 Running 拒绝
    EXPECT_EQ(evaluate_md_connect_precheck(true, "dzmd_ctp", true, std::nullopt),
              MdConnectPrecheck::ProcessNotRunning);
    EXPECT_EQ(evaluate_md_connect_precheck(true, "dzmd_ctp", true, Opt(platform::ChildState::Stopped)),
              MdConnectPrecheck::ProcessNotRunning);
    EXPECT_EQ(evaluate_md_connect_precheck(true, "dzmd_ctp", true, Opt(platform::ChildState::Running)),
              MdConnectPrecheck::Ok);
}

TEST(WsControlPrecheckTest, MdConnectPrecheckMessages) {
    EXPECT_EQ(md_connect_precheck_message(MdConnectPrecheck::NotAdmin, "x"), "admin required");
    EXPECT_EQ(md_connect_precheck_message(MdConnectPrecheck::InvalidSource, "x"),
              "invalid source or writer not ready");
    EXPECT_EQ(md_connect_precheck_message(MdConnectPrecheck::ProcessNotRunning, "dzmd_ctp"),
              "process not running: dzmd_ctp");
    EXPECT_TRUE(md_connect_precheck_message(MdConnectPrecheck::Ok, "x").empty());
}

// 契约 md-subscription: dzweb 不校验互斥——同时出现时两者均透传，
// 由目标 md 进程经 RTN.error=ambiguous_query 表达（修复 WS 路径该错误码不可达）
TEST(WsControlPrecheckTest, SubscriptionQueryPayloadPassesBoth) {
    nlohmann::json out;
    const nlohmann::json in = {{"query", "unsuccessful"}, {"instruments", {"IF2506", "IC2506"}}};
    EXPECT_TRUE(build_subscription_query_payload(in, out));
    EXPECT_EQ(out["query"], "unsuccessful");
    ASSERT_TRUE(out["instruments"].is_array());
    EXPECT_EQ(out["instruments"].size(), 2u);
}

TEST(WsControlPrecheckTest, SubscriptionQueryPayloadSingleMode) {
    nlohmann::json out;
    EXPECT_TRUE(build_subscription_query_payload({{"query", "unsuccessful"}}, out));
    EXPECT_EQ(out.dump(), R"({"query":"unsuccessful"})");

    nlohmann::json out2;
    EXPECT_TRUE(build_subscription_query_payload({{"instruments", {"rb2510"}}}, out2));
    EXPECT_TRUE(out2.contains("instruments"));
    EXPECT_FALSE(out2.contains("query"));

    // 两者皆缺/类型非法 -> false（error 路径）；非法类型不透传
    nlohmann::json out3;
    EXPECT_FALSE(build_subscription_query_payload({{"foo", 1}}, out3));
    EXPECT_TRUE(out3.empty());
    nlohmann::json out4;
    EXPECT_FALSE(build_subscription_query_payload({{"query", 123}}, out4));  // 非字符串忽略
    EXPECT_TRUE(out4.empty());
}

}  // namespace
}  // namespace dztrader::webui