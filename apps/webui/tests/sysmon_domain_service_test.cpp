#include "ws_broadcaster.h"
#include "mirror_store.h"
#include "sysmon_domain_service.h"

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
nlohmann::json make_snapshot() {
    return {
        {"source", "dzsysmon"},
        {"timestamp", 1790000005000},
        {"sampling",
         {{"measure_interval_ms", 1000},
          {"last_sample_unix_ms", 1790000005123},
          {"missed_ticks", 0}}},
        {"platform",
         {{"family", "linux"},
          {"version", "Ubuntu 24.04.1 LTS"},
          {"kernel", "6.8.0-45-generic"},
          {"arch", "x86_64"},
          {"build", 0}}},
        {"system",
         {{"hostname", "trade-01"},
          {"cpu_model", "AMD Ryzen 9 7950X 16-Core Processor"},
          {"logical_cores", 32},
          {"boot_time_unix_sec", 1789990000},
          {"uptime_sec", 10005}}},
        {"cpu",
         {{"total", {{"usage_ratio", 0.187}}},
          {"cores", nlohmann::json::array()},
          {"saturated_cores", nlohmann::json::array()},
          {"load_average", nullptr},
          {"context_switches_per_sec", nullptr},
          {"interrupts_per_sec", nullptr}}},
        {"memory", nullptr},
        {"swap", nullptr},
        {"volumes", nullptr},
        {"disk_io", nullptr},
        {"network", nullptr},
        {"shm_dir", nullptr},
        {"availability",
         {{"platform_description", "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)"},
          {"metrics",
           {{"system", {{"available", true}}},
            {"cpu.total", {{"available", true}}},
            {"cpu.cores", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.load_average", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.context_switches", {{"available", false}, {"reason", "not_implemented"}}},
            {"memory", {{"available", false}, {"reason", "not_implemented"}}},
            {"swap", {{"available", false}, {"reason", "not_implemented"}}},
            {"volumes", {{"available", false}, {"reason", "not_implemented"}}},
            {"disk_io", {{"available", false}, {"reason", "not_implemented"}}},
            {"network", {{"available", false}, {"reason", "not_implemented"}}},
            {"shm_dir", {{"available", false}, {"reason", "not_implemented"}}}}},
          {"semantic_hints", nlohmann::json::object()}}},
    };
}

nlohmann::json make_config() { return platform::SysmonConfig::default_config(); }

}  // namespace

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
    EXPECT_EQ(ws.messages[0].data, snapshot);
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
