#include <gtest/gtest.h>

#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/sysmon.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace dztrader::platform {
namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

constexpr uint64_t kMB = 1024 * 1024;

/// 构造契约合法的最小快照（系统信息 + CPU 总使用率，其余域 null）。
SysmonSnapshot make_snapshot() {
    SysmonSnapshot snap;
    snap.source = "dzsysmon";
    snap.timestamp = 1790000005000;
    snap.sampling.measure_interval_ms = 1000;
    snap.sampling.last_sample_unix_ms = 1790000005123;
    snap.sampling.missed_ticks = 2;
    snap.platform = {.family = "linux",
                     .version = "Ubuntu 24.04.1 LTS",
                     .kernel = "6.8.0-45-generic",
                     .arch = "x86_64",
                     .build = 0};
    snap.system = {.hostname = "trade-01",
                   .cpu_model = "AMD Ryzen 9 7950X 16-Core Processor",
                   .logical_cores = 32,
                   .boot_time_unix_sec = 1789990000,
                   .uptime_sec = 10005};
    snap.cpu.total.usage_ratio = 0.187;
    snap.memory = std::nullopt;
    snap.swap = std::nullopt;
    snap.volumes = std::nullopt;
    snap.disk_io = std::nullopt;
    snap.network = std::nullopt;
    snap.shm_dir = std::nullopt;
    snap.availability.platform_description = "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)";
    for (const char* key :
         {"system", "cpu.total", "cpu.cores", "cpu.load_average", "cpu.context_switches", "memory",
          "swap", "volumes", "disk_io", "network", "shm_dir"}) {
        snap.availability.metrics[key] = {.available = false,
                                          .reason = std::string("not_implemented")};
    }
    snap.availability.metrics["system"].available = true;
    snap.availability.metrics["system"].reason = std::nullopt;
    snap.availability.metrics["cpu.total"].available = true;
    snap.availability.metrics["cpu.total"].reason = std::nullopt;
    return snap;
}

class SysmonFrameTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    void SetUp() override {
        channel_name_ = "dz_test_sysmon_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        const ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        writer_ = MultiWriter::create(meta_, "test_writer");
        reader_ = Reader::create(meta_, "test_reader");
    }

    void TearDown() override {
        reader_.reset();
        writer_.reset();
        meta_.reset();
        std::error_code ec;
        std::filesystem::remove_all(shm_dir_, ec);
    }
};

// 网卡信息的 C++ 字段名避开 Windows `interface` 宏，但 JSON 键必须是契约的 `interface`。
TEST(SysmonSnapshotJsonTest, NetworkInfoSerializationKeepsContractKeys) {
    SysmonNetworkInfo network;
    network.interface_name = "eth0";
    network.rx_bytes_total = 100;
    network.tx_bytes_total = 200;
    network.rx_bytes_per_sec = 1.5;
    network.is_virtual = true;

    const nlohmann::json j = network;
    EXPECT_EQ(j["interface"], "eth0");
    EXPECT_FALSE(j.contains("interface_name"));
    EXPECT_EQ(j["rx_bytes_total"], 100);
    EXPECT_DOUBLE_EQ(j["rx_bytes_per_sec"].get<double>(), 1.5);
    EXPECT_TRUE(j["is_virtual"].get<bool>());

    const auto decoded = j.get<SysmonNetworkInfo>();
    EXPECT_EQ(decoded.interface_name, "eth0");
    EXPECT_EQ(decoded.tx_bytes_total, 200u);
    ASSERT_TRUE(decoded.rx_bytes_per_sec.has_value());
    EXPECT_DOUBLE_EQ(*decoded.rx_bytes_per_sec, 1.5);
    EXPECT_TRUE(decoded.is_virtual);
}

// 含网卡条目的快照应通过契约校验（network[] 的 `interface` 键与 per-entry 规则）。
TEST(SysmonSnapshotValidateTest, AcceptsNetworkEntries) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["network"] = nlohmann::json::array({{{"interface", "eth0"},
                                           {"rx_bytes_total", 1},
                                           {"tx_bytes_total", 2},
                                           {"rx_bytes_per_sec", 0.5},
                                           {"tx_bytes_per_sec", nullptr},
                                           {"rx_errors_per_sec", nullptr},
                                           {"tx_errors_per_sec", nullptr},
                                           {"rx_drops_per_sec", nullptr},
                                           {"tx_drops_per_sec", nullptr},
                                           {"is_virtual", false}}});
    j["availability"]["metrics"]["network"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

// 快照 JSON 结构应与契约一致（可选域序列化为 null、availability 不出现 null reason）。
TEST(SysmonSnapshotJsonTest, DefaultSnapshotShape) {
    const auto snap = make_snapshot();
    const nlohmann::json j = snap;
    EXPECT_EQ(j["platform"]["family"], "linux");
    EXPECT_TRUE(j["memory"].is_null());
    EXPECT_TRUE(j["cpu"]["load_average"].is_null());
    EXPECT_TRUE(j["cpu"]["cores"].is_array());
    EXPECT_FALSE(j["availability"]["metrics"]["system"].contains("reason"));
    EXPECT_EQ(j["availability"]["metrics"]["memory"]["reason"], "not_implemented");
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

// 帧编解码：写入无 instance_id 快照帧 → 读出 → 解析 → 校验通过。
TEST_F(SysmonFrameTest, SnapshotFrameRoundTrip) {
    const auto snap = make_snapshot();
    ASSERT_TRUE(write_ext_json(*writer_, DZ_FRAME_RTN_SYSMON_SNAPSHOT, snap));

    const auto* raw = reader_->next_frame();
    ASSERT_NE(raw, nullptr);
    const FrameView view(raw);
    EXPECT_EQ(view.type(), DZ_FRAME_RTN_SYSMON_SNAPSHOT);
    const auto payload = nlohmann::json::parse(
        reinterpret_cast<const char*>(view.ext_payload()),
        reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
    EXPECT_EQ(payload, nlohmann::json(snap));
    EXPECT_EQ(SysmonSnapshot::validate(payload), std::nullopt);
}

// 配置帧编解码：带 instance_id 定向帧 → instance_id/payload 均正确，配置校验通过。
TEST_F(SysmonFrameTest, ConfigFrameRoundTrip) {
    const auto cfg = SysmonConfig::default_config();
    const auto payload = nlohmann::json(cfg);
    ASSERT_TRUE(write_ext_inst_json_obj(*writer_, DZ_FRAME_RTN_SYSMON_CONFIG, "dzsysmon", payload));

    const auto* raw = reader_->next_frame();
    ASSERT_NE(raw, nullptr);
    const FrameView view(raw);
    EXPECT_EQ(view.type(), DZ_FRAME_RTN_SYSMON_CONFIG);
    EXPECT_EQ(std::string(view.ext_inst_id()), "dzsysmon");
    const auto decoded = nlohmann::json::parse(
        reinterpret_cast<const char*>(view.ext_inst_payload()),
        reinterpret_cast<const char*>(view.ext_inst_payload()) + view.ext_inst_payload_size());
    EXPECT_EQ(decoded, payload);
    EXPECT_EQ(SysmonConfig::validate(decoded), std::nullopt);
}

// 校验：比率越界、内存口径不一致、不可用缺 reason、显式 null 非允许位置均拒绝。
TEST(SysmonSnapshotValidateTest, RejectsInvalidPayloads) {
    {  // usage_ratio 越界
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["cpu"]["total"]["usage_ratio"] = 1.25;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 内存已用 != 总量 - 可用
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["memory"] = {{"total_bytes", 100}, {"available_bytes", 40}, {"used_bytes", 50}};
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 不可用但缺 reason
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["availability"]["metrics"]["memory"].erase("reason");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 允许 null 的位置之外出现 null（system.hostname）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["system"]["hostname"] = nullptr;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 未知字段忽略（前向兼容）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["future_field"] = 1;
        EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
    }
    {  // availability 固定键集合缺一不可（契约 sysmon §2.5）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["availability"]["metrics"].erase("network");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 无 instance_id 帧的来源显式携带且固定为 dzsysmon（总则 §5）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j.erase("source");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j = nlohmann::json(snap);
        j["source"] = "dzweb";
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // cpu 必填键缺失（null 允许，键必须在）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["cpu"].erase("load_average");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j = nlohmann::json(snap);
        j["cpu"]["total"].erase("usage_ratio");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // available=true 时不得出现 reason（含 null）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["availability"]["metrics"]["system"]["reason"] = nullptr;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
}

// 配置：默认值合法、patch 合并生效、null/越界 patch 拒绝。
TEST(SysmonConfigTest, DefaultConfigValid) {
    const auto cfg = SysmonConfig::default_config();
    const auto j = nlohmann::json(cfg);
    ASSERT_EQ(SysmonConfig::validate(j), std::nullopt);
    EXPECT_EQ(j["sampling"]["measure_interval_ms"], 1000);
    EXPECT_EQ(j["realtime"]["push_interval_sec"], 5);
    EXPECT_EQ(j["sampling"]["groups"].size(), 6u);
}

TEST(SysmonConfigTest, ApplyPatchMergesAndValidates) {
    const auto current = nlohmann::json(SysmonConfig::default_config());
    const auto merged =
        SysmonConfig::apply_patch(current, {{"realtime", {{"push_interval_sec", 10}}}});
    EXPECT_EQ(merged["realtime"]["push_interval_sec"], 10);
    EXPECT_EQ(merged["sampling"]["measure_interval_ms"], 1000);  // 未提及字段保留

    // [[nodiscard]] 返回值在 EXPECT_THROW 中被丢弃：显式 (void) 规避 MSVC C4834
    // （Release /WX 下会升级为 C2220 错误）。
    EXPECT_THROW(
        (void)SysmonConfig::apply_patch(current, {{"realtime", {{"push_interval_sec", 0}}}}),
        std::runtime_error);
    EXPECT_THROW((void)SysmonConfig::apply_patch(current, {{"extra", nullptr}}),
                 std::runtime_error);
    EXPECT_THROW((void)SysmonConfig::apply_patch(current, nlohmann::json::array()),
                 std::runtime_error);
}

// 契约 sysmon §2.5：Windows 平台描述必须含 build 号（区分 Win10/Win11）。
TEST(SysmonSnapshotValidateTest, WindowsDescriptionMustIncludeBuild) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["platform"]["family"] = "windows";
    j["platform"]["version"] = "Windows 10 Pro";
    j["platform"]["kernel"] = "10.0.19045";
    j["platform"]["build"] = 19045;
    j["availability"]["platform_description"] = "Windows 10 Pro (build 19045)";
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);

    j["availability"]["platform_description"] = "Windows 10 Pro";
    EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
}

}  // namespace
}  // namespace dztrader::platform
