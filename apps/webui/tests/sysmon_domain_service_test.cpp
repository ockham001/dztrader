#include "ws_broadcaster.h"
#include "mirror_store.h"
#include "sysmon_domain_service.h"
#include "sysmon_snapshot_fixture.h"

#include <dztrader/platform/sysmon.h>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace dztrader::webui {
namespace {

/// 最小 WsBroadcaster 桩（domain_service_test.cpp 同款）
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

/// 构造一份契约合法的首版快照（系统信息 + CPU 总使用率；其余域 null）。
/// 形状（15 键 availability + clock 空态）的唯一实现在 `sysmon_snapshot_fixture.h`
/// （#309 AC5）；本文件只给本例关心的数值。
nlohmann::json make_snapshot() {
    return dztrader::test::sysmon_snapshot_fixture(
        {.cpu_usage_ratio = 0.187,
         .cpu_model = "AMD Ryzen 9 7950X 16-Core Processor",
         .logical_cores = 32});
}

nlohmann::json make_config() { return platform::SysmonConfig::default_config(); }

}  // namespace

// 夹具与契约同步：快照夹具必须通过类型层校验——否则 domain service 会静默丢弃广播，
// 相关用例只看得到「无广播」，看不出夹具已漂移（固定 15 个可用性键 + clock 域）。
TEST(SysmonDomainServiceTest, SnapshotFixturePassesContractValidation) {
    EXPECT_EQ(platform::SysmonSnapshot::validate(make_snapshot()), std::nullopt);
}

TEST(SysmonDomainServiceTest, SnapshotUpdatesMirrorAndBroadcasts) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    const auto snapshot = make_snapshot();
    service.on_rtn_sysmon_snapshot(snapshot);

    const auto instance = mirror.instance("dzsysmon");
    ASSERT_TRUE(instance.contains("sysmon_snapshot"));
    EXPECT_EQ(instance["sysmon_snapshot"]["cpu"]["total"]["usage_ratio"].get<double>(), 0.187);

    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "sysmon_snapshot");
    EXPECT_EQ(ws.messages[0].instance_id, "dzsysmon");
    EXPECT_TRUE(ws.messages[0].data == snapshot);  // 避免 gtest 打印 nlohmann::json（MSVC C4702）
}

TEST(SysmonDomainServiceTest, SnapshotIncludedInWsSnapshot) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    service.on_rtn_sysmon_snapshot(make_snapshot());
    // 新连接立即拿到：镜像快照必须包含固定实例与快照域（契约 webui-ws §2.1）
    const auto full = mirror.snapshot();
    ASSERT_TRUE(full.contains("dzsysmon"));
    EXPECT_TRUE(full["dzsysmon"].contains("sysmon_snapshot"));
}

TEST(SysmonDomainServiceTest, InvalidSnapshotIgnored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    auto broken = make_snapshot();
    broken["cpu"]["total"]["usage_ratio"] = 1.5;  // 超出 [0,1]
    service.on_rtn_sysmon_snapshot(broken);

    EXPECT_TRUE(mirror.instance("dzsysmon").empty());
    EXPECT_TRUE(ws.messages.empty());
}

TEST(SysmonDomainServiceTest, SnapshotFromUnexpectedSourceIgnored) {
    // 契约 sysmon：快照发送方固定 dzsysmon；source 不符视为坏帧（不更新镜像、不广播）。
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    auto snapshot = make_snapshot();
    snapshot["source"] = "dzweb";
    service.on_rtn_sysmon_snapshot(snapshot);

    EXPECT_TRUE(mirror.instance("dzsysmon").empty());
    EXPECT_TRUE(ws.messages.empty());
}

TEST(SysmonDomainServiceTest, ConfigUpdatesMirrorAndBroadcasts) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    const auto config = make_config();
    service.on_rtn_sysmon_config("dzsysmon", config);

    const auto instance = mirror.instance("dzsysmon");
    ASSERT_TRUE(instance.contains("sysmon_config"));
    EXPECT_EQ(instance["sysmon_config"]["realtime"]["push_interval_sec"].get<int>(), 5);

    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "sysmon_config");
    EXPECT_EQ(ws.messages[0].instance_id, "dzsysmon");
}

TEST(SysmonDomainServiceTest, InvalidConfigIgnored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    SysmonDomainService service(mirror, ws);

    auto broken = make_config();
    broken["realtime"]["push_interval_sec"] = 0;  // 越界
    service.on_rtn_sysmon_config("dzsysmon", broken);

    EXPECT_TRUE(mirror.instance("dzsysmon").empty());
    EXPECT_TRUE(ws.messages.empty());
}

}  // namespace dztrader::webui
