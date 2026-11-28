// NotifyDomainService 单测（契约 notify-ui / ADR 0060 决策 4/5）。
//
// 覆盖的是**服务层编排**（NotifyCache 自身的折叠算法由 notify_cache_test.cpp 覆盖）：
// - 未命中折叠 → 写入缓存 + 广播**补齐 count** 后的条目（不是原 payload）
// - 命中折叠 → 不新增缓存条目 + 广播**折叠后**的 payload（count 已累加）
// - 转发日志降级为 DEBUG（ADR 0060 决策 5：转发不是独立事件，不得以 WARN 再报一次）
#include "notify_cache.h"
#include "notify_domain_service.h"

#include <gtest/gtest.h>

#include <spdlog/sinks/base_sink.h>

namespace dztrader::webui {
namespace {

/// 记录日志级别的测试 sink：用于断言「转发日志不得以 WARN/ERROR 出现」。
class LevelCapturingSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<spdlog::level::level_enum> levels;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override { levels.push_back(msg.level); }
    void flush_() override {}
};

class FakeBroadcaster : public WsBroadcaster {
public:
    struct Msg {
        std::string type;
        std::string instance_id;
        nlohmann::json data;
    };
    std::vector<Msg> messages;

    void broadcast(const std::string& type,
                   const std::string& instance_id,
                   const nlohmann::json& data) override {
        messages.push_back({.type = type, .instance_id = instance_id, .data = data});
    }
};

nlohmann::json make_payload(std::string_view source,
                            std::string_view level,
                            std::string_view message,
                            int64_t ts) {
    return nlohmann::json{{"source", source},
                          {"level", level},
                          {"message", message},
                          {"timestamp", ts},
                          {"popup", false}};
}

TEST(NotifyDomainServiceTest, BroadcastsNotifyUiTypeWithEmptyInstanceId) {
    NotifyCache cache(100, 60, 300);
    FakeBroadcaster ws;
    NotifyDomainService svc(cache, ws);

    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "下单被拒 #1", 1000));

    ASSERT_EQ(ws.messages.size(), 1U);
    EXPECT_EQ(ws.messages[0].type, "notify_ui");
    // 契约 notify-ui：source 在 payload 不在帧头，故 instance_id 为空
    EXPECT_TRUE(ws.messages[0].instance_id.empty());
    EXPECT_EQ(ws.messages[0].data.value("message", ""), "下单被拒 #1");
    EXPECT_EQ(ws.messages[0].data.value("count", 0), 1);  // add() 补 count=1
}

TEST(NotifyDomainServiceTest, FirstOccurrenceIsCached) {
    NotifyCache cache(100, 60, 300);
    FakeBroadcaster ws;
    NotifyDomainService svc(cache, ws);

    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "错误 1", 1000));

    ASSERT_EQ(cache.get_all().size(), 1U);
    EXPECT_EQ(cache.get_all()[0].value("count", 0), 1);
}

TEST(NotifyDomainServiceTest, FoldedOccurrenceDoesNotAddCacheEntryButBroadcastsCount) {
    NotifyCache cache(100, 60, 300);
    FakeBroadcaster ws;
    NotifyDomainService svc(cache, ws);

    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "平今仓位不足 1", 1000));
    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "平今仓位不足 2", 1010));

    // 缓存只有一条（折叠），但广播了两次（折叠结果要推给已渲染的 UI 更新计数）
    ASSERT_EQ(cache.get_all().size(), 1U);
    EXPECT_EQ(cache.get_all()[0].value("count", 0), 2);
    ASSERT_EQ(ws.messages.size(), 2U);
    EXPECT_EQ(ws.messages[1].data.value("count", 0), 2);
    // 折叠后广播的仍是首报正文（不因归一化改写用户看到的消息）
    EXPECT_EQ(ws.messages[1].data.value("message", ""), "平今仓位不足 1");
}

TEST(NotifyDomainServiceTest, DifferentLevelsAreNotFolded) {
    NotifyCache cache(100, 60, 300);
    FakeBroadcaster ws;
    NotifyDomainService svc(cache, ws);

    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "连接异常", 1000));
    svc.on_notify_ui(make_payload("dztd_ctp", "error", "连接异常", 1010));

    // 指纹含 level：两个级别是两条不同事件（ADR 0060 决策 2 的直接后果）
    ASSERT_EQ(cache.get_all().size(), 2U);
}

TEST(NotifyDomainServiceTest, ForwardingLogIsNotWarnOrError) {
    // ADR 0060 决策 5：转发轨迹不是独立事件。旧实现把 error 通知打成 WARN，
    // 造成「同一事件在日志里出现 ERROR + WARN 两行」——用户实测报的正是这个。
    // 断言只看「不出现 WARN/ERROR」，**不**反向要求必须有 DEBUG：
    // Release 下 SPDLOG_DEBUG 被编译掉（SPDLOG_ACTIVE_LEVEL），日志行根本不存在，
    // 反向断言会在 Release 假红。这与仓库既有事实一致（见 AGENTS.md 编译期门禁段）。
    auto sink = std::make_shared<LevelCapturingSink>();
    auto logger = std::make_shared<spdlog::logger>("notify_test_sink", sink);
    logger->set_level(spdlog::level::trace);
    auto previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);

    NotifyCache cache(100, 60, 300);
    FakeBroadcaster ws;
    NotifyDomainService svc(cache, ws);
    svc.on_notify_ui(make_payload("dztd_ctp", "error", "交易登录失败", 1000));
    svc.on_notify_ui(make_payload("dztd_ctp", "warning", "下单被拒", 1001));
    svc.on_notify_ui(make_payload("dztd_ctp", "info", "交易连接成功", 1002));

    for (const auto level : sink->levels) {
        EXPECT_LT(level, spdlog::level::warn) << "notify forwarding must not log at WARN/ERROR"
                                                 " (same event must not appear twice in the log)";
    }

    spdlog::set_default_logger(previous);
}

}  // namespace
}  // namespace dztrader::webui
