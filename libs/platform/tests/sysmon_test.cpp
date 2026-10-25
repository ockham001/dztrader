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
    snap.watch_paths = std::nullopt;
    snap.disk_io = std::nullopt;
    snap.network = std::nullopt;
    snap.shm_dir = std::nullopt;
    snap.availability.platform_description = "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)";
    for (const char* key : {"system", "cpu.total", "cpu.cores", "cpu.load_average",
                            "cpu.context_switches", "cpu.frequency", "memory", "swap", "volumes",
                            "disk_io", "network", "shm_dir", "temperature"}) {
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

// 契约 sysmon §7：八个 `*_total` 是累计计数器，没有"不可用"语义——缺失即校验失败
// （不得像速率那样允许 null/缺省）。
namespace {

/// 一份形状合法的 network[] 条目（八个累计计数器 + 六个可空速率 + is_virtual）。
nlohmann::json valid_network_entry() {
    return {{"interface", "eth0"},          {"rx_bytes_total", 1},
            {"tx_bytes_total", 2},          {"rx_packets_total", 3},
            {"tx_packets_total", 4},        {"rx_errors_total", 0},
            {"tx_errors_total", 0},         {"rx_drops_total", 5},
            {"tx_drops_total", 6},          {"rx_bytes_per_sec", 0.5},
            {"tx_bytes_per_sec", nullptr},  {"rx_errors_per_sec", nullptr},
            {"tx_errors_per_sec", nullptr}, {"rx_drops_per_sec", nullptr},
            {"tx_drops_per_sec", nullptr},  {"is_virtual", false}};
}

}  // namespace

// 含网卡条目的快照应通过契约校验（network[] 的 `interface` 键与 per-entry 规则）。
TEST(SysmonSnapshotValidateTest, AcceptsNetworkEntries) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["network"] = nlohmann::json::array({valid_network_entry()});
    j["availability"]["metrics"]["network"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

TEST(SysmonSnapshotValidateTest, RejectsNetworkEntryMissingCumulativeCounter) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["availability"]["metrics"]["network"] = {{"available", true}};
    for (const char* key :
         {"rx_bytes_total", "tx_bytes_total", "rx_packets_total", "tx_packets_total",
          "rx_errors_total", "tx_errors_total", "rx_drops_total", "tx_drops_total"}) {
        auto missing = valid_network_entry();
        missing.erase(key);
        j["network"] = nlohmann::json::array({missing});
        EXPECT_NE(SysmonSnapshot::validate(j), std::nullopt) << "missing " << key;
    }
    // 累计计数器也不接受显式 null（与速率字段的 null 语义不同）。
    auto nulled = valid_network_entry();
    nulled["rx_bytes_total"] = nullptr;
    j["network"] = nlohmann::json::array({nulled});
    EXPECT_NE(SysmonSnapshot::validate(j), std::nullopt);
}

TEST(SysmonSnapshotValidateTest, RejectsDuplicateInterfaceAndMissingVirtualFlag) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["availability"]["metrics"]["network"] = {{"available", true}};

    // interface 帧内唯一（契约 sysmon §7）
    const auto entry = valid_network_entry();
    j["network"] = nlohmann::json::array({entry, entry});
    EXPECT_NE(SysmonSnapshot::validate(j), std::nullopt);

    // is_virtual 必填 bool（不是可选默认值）
    auto no_flag = valid_network_entry();
    no_flag.erase("is_virtual");
    j["network"] = nlohmann::json::array({no_flag});
    EXPECT_NE(SysmonSnapshot::validate(j), std::nullopt);

    // interface 非空
    auto empty_name = valid_network_entry();
    empty_name["interface"] = "";
    j["network"] = nlohmann::json::array({empty_name});
    EXPECT_NE(SysmonSnapshot::validate(j), std::nullopt);
}

// 契约 sysmon §2.3/§2.4：temperature[] 与 cpu.core_frequencies[] 的合法条目通过校验，
// 序列化键名与可空语义保持契约形状。
TEST(SysmonSnapshotValidateTest, AcceptsTemperatureAndCoreFrequencies) {
    auto snap = make_snapshot();
    snap.cpu.core_frequencies = std::vector<SysmonCoreFrequency>{
        SysmonCoreFrequency{.index = 0, .mhz = 3600},
        SysmonCoreFrequency{.index = 2, .mhz = 3800},
    };
    snap.temperature = std::vector<SysmonTemperatureInfo>{
        SysmonTemperatureInfo{.sensor = "coretemp:Package id 0", .celsius = 45.5},
    };
    snap.availability.metrics["cpu.frequency"] = {.available = true, .reason = std::nullopt};
    snap.availability.metrics["temperature"] = {.available = true, .reason = std::nullopt};

    auto j = nlohmann::json(snap);
    ASSERT_TRUE(j["cpu"]["core_frequencies"].is_array());
    EXPECT_EQ(j["cpu"]["core_frequencies"][0]["index"], 0);
    EXPECT_EQ(j["cpu"]["core_frequencies"][0]["mhz"], 3600);
    ASSERT_TRUE(j["temperature"].is_array());
    EXPECT_EQ(j["temperature"][0]["sensor"], "coretemp:Package id 0");
    EXPECT_DOUBLE_EQ(j["temperature"][0]["celsius"].get<double>(), 45.5);
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);

    // null 表示本平台不可用/无传感器，是契约允许的形状（默认快照即 null）。
    j["cpu"]["core_frequencies"] = nullptr;
    j["temperature"] = nullptr;
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

// 快照 JSON 结构应与契约一致（可选域序列化为 null、availability 不出现 null reason）。
TEST(SysmonSnapshotJsonTest, DefaultSnapshotShape) {
    const auto snap = make_snapshot();
    const nlohmann::json j = snap;
    EXPECT_EQ(j["platform"]["family"], "linux");
    EXPECT_TRUE(j["memory"].is_null());
    EXPECT_TRUE(j["temperature"].is_null());
    EXPECT_TRUE(j["cpu"]["load_average"].is_null());
    EXPECT_TRUE(j["cpu"]["core_frequencies"].is_null());
    EXPECT_TRUE(j["cpu"]["cores"].is_array());
    // 契约 sysmon §2.8：alerts 恒为数组，空数组 = 无活动告警（不是 null/缺键）。
    ASSERT_TRUE(j["alerts"].is_array());
    EXPECT_TRUE(j["alerts"].empty());
    EXPECT_FALSE(j["availability"]["metrics"]["system"].contains("reason"));
    EXPECT_EQ(j["availability"]["metrics"]["memory"]["reason"], "not_implemented");
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

// 契约 sysmon §2.8：快照 alerts[] 的形状、subject 规则与帧内唯一性。
TEST(SysmonSnapshotValidateTest, AcceptsAlertEntriesAndRejectsInvalidOnes) {
    auto snap = make_snapshot();
    snap.alerts = {
        SysmonAlertInfo{.kind = SYSMON_ALERT_MEMORY_USED, .subject = "", .since = 1},
        SysmonAlertInfo{.kind = SYSMON_ALERT_SWAP_USED, .subject = "", .since = 2},
        SysmonAlertInfo{.kind = SYSMON_ALERT_VOLUME_FREE, .subject = "/data", .since = 3},
    };
    const auto valid = nlohmann::json(snap);
    EXPECT_EQ(SysmonSnapshot::validate(valid), std::nullopt);
    EXPECT_EQ(valid["alerts"][0]["kind"], "memory_used");

    {  // 缺 alerts 键（必填，与"空"不等价）
        auto j = valid;
        j.erase("alerts");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // alerts 非数组
        auto j = valid;
        j["alerts"] = nlohmann::json::object();
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 未知 kind
        auto j = valid;
        j["alerts"][0]["kind"] = "disk_io_hot";
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // volume_free 缺 subject
        auto j = valid;
        j["alerts"][2]["subject"] = "";
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 非 volume_free 带 subject
        auto j = valid;
        j["alerts"][0]["subject"] = "/data";
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // since 缺失 / 为负
        auto j = valid;
        j["alerts"][0].erase("since");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j = valid;
        j["alerts"][0]["since"] = -1;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // (kind, subject) 帧内重复
        auto j = valid;
        j["alerts"].push_back({{"kind", "memory_used"}, {"subject", ""}, {"since", 4}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 同 kind 不同 subject（不同卷）合法
        auto j = valid;
        j["alerts"].push_back({{"kind", "volume_free"}, {"subject", "/home"}, {"since", 4}});
        EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
    }
    {  // 条目不是对象
        auto j = valid;
        j["alerts"].push_back(3);
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
}

// 契约 sysmon §2.4（#74）：内存 cached_bytes / buffers_bytes 允许 null（本平台不提供/
// 本帧未读到）或非负整数且不超过 total_bytes。
TEST(SysmonSnapshotValidateTest, AcceptsMemoryWithCacheFields) {
    auto snap = make_snapshot();
    snap.memory = SysmonMemoryInfo{.total_bytes = 1024,
                                   .available_bytes = 512,
                                   .used_bytes = 512,
                                   .used_ratio = 0.5,
                                   .cached_bytes = 256,
                                   .buffers_bytes = 64};
    snap.availability.metrics["memory"].available = true;
    snap.availability.metrics["memory"].reason = std::nullopt;
    const auto j = nlohmann::json(snap);
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);

    // Windows：buffers 不提供（null），cached 仍可得。
    auto windows_like = j;
    windows_like["memory"]["buffers_bytes"] = nullptr;
    EXPECT_EQ(SysmonSnapshot::validate(windows_like), std::nullopt);

    // 两者都可为 null（平台不提供）。
    auto no_cache = j;
    no_cache["memory"]["cached_bytes"] = nullptr;
    no_cache["memory"]["buffers_bytes"] = nullptr;
    EXPECT_EQ(SysmonSnapshot::validate(no_cache), std::nullopt);
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
    {  // memory.cached_bytes 超过 total_bytes
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["memory"] = {{"total_bytes", 100}, {"available_bytes", 40}, {"used_bytes", 60},
                       {"used_ratio", 0.6},  {"cached_bytes", 101},   {"buffers_bytes", 0}};
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // memory.buffers_bytes 为负数
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["memory"] = {{"total_bytes", 100}, {"available_bytes", 40}, {"used_bytes", 60},
                       {"used_ratio", 0.6},  {"cached_bytes", 0},     {"buffers_bytes", -1}};
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
    {  // cpu.core_frequencies 键必须存在（null 允许，键必须在）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["cpu"].erase("core_frequencies");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // cpu.core_frequencies 非 null 时必须是数组，条目 index 非负、mhz > 0 且为整数
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["cpu"]["core_frequencies"] = 0;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["cpu"]["core_frequencies"] = nlohmann::json::array({{{"index", 0}, {"mhz", 0}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["cpu"]["core_frequencies"] = nlohmann::json::array({{{"index", -1}, {"mhz", 3600}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["cpu"]["core_frequencies"] = nlohmann::json::array({{{"index", 0}, {"mhz", 1.5}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["cpu"]["core_frequencies"] = nlohmann::json::array({1});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // temperature 键必须存在（null 允许）；非 null 时 sensor 非空、celsius 为数字
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j.erase("temperature");
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j = nlohmann::json(snap);
        j["temperature"] = 42;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["temperature"] = nlohmann::json::array({{{"sensor", ""}, {"celsius", 40.0}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["temperature"] = nlohmann::json::array({{{"sensor", "x"}, {"celsius", "hot"}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
        j["temperature"] = nlohmann::json::array({{{"sensor", "x"}}});
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
    // 契约 sysmon §3.1：A 组六项 + B 组（温度 / 每核频率）共 8 项，全部必须出现在默认值里。
    EXPECT_EQ(j["sampling"]["groups"].size(), 8u);
    for (const char* group : {"cpu", "memory", "swap", "volumes", "disk_io", "network"}) {
        EXPECT_TRUE(j["sampling"]["groups"][group].get<bool>()) << group;
    }
#if defined(_WIN32)
    // 契约 sysmon §3.1：Windows 上温度/每核频率不可靠，默认关闭。
    EXPECT_FALSE(j["sampling"]["groups"]["temperature"].get<bool>());
    EXPECT_FALSE(j["sampling"]["groups"]["cpu_frequency"].get<bool>());
#else
    // 契约 sysmon §3.1：Linux 默认开启 B 组（无传感器时由可用性映射报告不可用）。
    EXPECT_TRUE(j["sampling"]["groups"]["temperature"].get<bool>());
    EXPECT_TRUE(j["sampling"]["groups"]["cpu_frequency"].get<bool>());
#endif
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

// #81 配置边界（契约 sysmon §3.1）：每个取值范围两端的**拒绝表**。
// 与已有用例（push_interval_sec=0、任意 null、非 object patch）互补，不重复。
TEST(SysmonConfigTest, RejectsEveryOutOfRangeConfigField) {
    const auto current = nlohmann::json(SysmonConfig::default_config());
    const struct {
        const char* label;
        nlohmann::json patch;
    } rejected[] = {
        {"measure_interval_ms below", nlohmann::json{{"sampling", {{"measure_interval_ms", 99}}}}},
        {"measure_interval_ms above",
         nlohmann::json{{"sampling", {{"measure_interval_ms", 60001}}}}},
        {"record_interval_sec below", nlohmann::json{{"sampling", {{"record_interval_sec", 0}}}}},
        {"record_interval_sec above",
         nlohmann::json{{"sampling", {{"record_interval_sec", 86401}}}}},
        {"phase_offset_sec below", nlohmann::json{{"sampling", {{"phase_offset_sec", -1}}}}},
        {"phase_offset_sec above", nlohmann::json{{"sampling", {{"phase_offset_sec", 86401}}}}},
        {"push_interval_sec above", nlohmann::json{{"realtime", {{"push_interval_sec", 3601}}}}},
        // 百分比阈值：四个字段各自的两端越界（闭区间 [0,100]）。
        {"cpu_sustain_used_pct below",
         nlohmann::json{{"thresholds", {{"cpu_sustain_used_pct", -0.1}}}}},
        {"cpu_sustain_used_pct above",
         nlohmann::json{{"thresholds", {{"cpu_sustain_used_pct", 100.1}}}}},
        {"memory_used_pct below", nlohmann::json{{"thresholds", {{"memory_used_pct", -0.1}}}}},
        {"memory_used_pct above", nlohmann::json{{"thresholds", {{"memory_used_pct", 100.1}}}}},
        {"swap_used_pct below", nlohmann::json{{"thresholds", {{"swap_used_pct", -0.1}}}}},
        {"swap_used_pct above", nlohmann::json{{"thresholds", {{"swap_used_pct", 100.1}}}}},
        {"volume_free_pct below", nlohmann::json{{"thresholds", {{"volume_free_pct", -0.1}}}}},
        {"volume_free_pct above", nlohmann::json{{"thresholds", {{"volume_free_pct", 100.1}}}}},
        {"cpu_sustain_samples below", nlohmann::json{{"thresholds", {{"cpu_sustain_samples", 0}}}}},
        {"cpu_sustain_samples above",
         nlohmann::json{{"thresholds", {{"cpu_sustain_samples", 3601}}}}},
        {"log_repeat_sec below", nlohmann::json{{"thresholds", {{"log_repeat_sec", -1}}}}},
        {"log_repeat_sec above", nlohmann::json{{"thresholds", {{"log_repeat_sec", 86401}}}}},
        {"raw_retention_hours below", nlohmann::json{{"history", {{"raw_retention_hours", 0}}}}},
        {"raw_retention_hours above", nlohmann::json{{"history", {{"raw_retention_hours", 721}}}}},
        {"max_retention_days below", nlohmann::json{{"history", {{"max_retention_days", 0}}}}},
        {"max_retention_days above", nlohmann::json{{"history", {{"max_retention_days", 3651}}}}},
        {"max_bytes negative", nlohmann::json{{"history", {{"max_bytes", -1}}}}},
        {"groups value not bool", nlohmann::json{{"sampling", {{"groups", {{"cpu", 1}}}}}}},
        {"groups value string",
         nlohmann::json{{"sampling", {{"groups", {{"temperature", "on"}}}}}}},
        {"devices element empty", nlohmann::json{{"disk", {{"devices", {""}}}}}},
        {"watch_paths element empty", nlohmann::json{{"disk", {{"watch_paths", {""}}}}}},
        // 任意位置 null 非法（general §8）：根、段、嵌套键、数组元素。
        {"patch root null", nlohmann::json(nullptr)},
        {"section null", nlohmann::json{{"disk", nullptr}}},
        {"nested config key null", nlohmann::json{{"sampling", {{"groups", {{"cpu", nullptr}}}}}}},
        {"nested array element null", nlohmann::json{{"disk", {{"devices", {nullptr}}}}}},
    };
    for (const auto& item : rejected) {
        EXPECT_THROW((void)SysmonConfig::apply_patch(current, item.patch), std::runtime_error)
            << item.label;
    }
}

// #81 配置边界：闭区间端点必须**接受**（只拒绝越界值，不得把合法边界当非法）。
TEST(SysmonConfigTest, AcceptsConfigBoundaryValues) {
    const auto current = nlohmann::json(SysmonConfig::default_config());
    const struct {
        const char* label;
        nlohmann::json patch;
    } accepted[] = {
        {"measure_interval_ms lower", nlohmann::json{{"sampling", {{"measure_interval_ms", 100}}}}},
        {"measure_interval_ms upper",
         nlohmann::json{{"sampling", {{"measure_interval_ms", 60000}}}}},
        {"record_interval_sec lower", nlohmann::json{{"sampling", {{"record_interval_sec", 1}}}}},
        {"record_interval_sec upper",
         nlohmann::json{{"sampling", {{"record_interval_sec", 86400}}}}},
        {"phase_offset_sec lower", nlohmann::json{{"sampling", {{"phase_offset_sec", 0}}}}},
        {"phase_offset_sec upper", nlohmann::json{{"sampling", {{"phase_offset_sec", 86400}}}}},
        {"push_interval_sec lower", nlohmann::json{{"realtime", {{"push_interval_sec", 1}}}}},
        {"push_interval_sec upper", nlohmann::json{{"realtime", {{"push_interval_sec", 3600}}}}},
        {"thresholds lower", nlohmann::json{{"thresholds",
                                             {{"cpu_sustain_used_pct", 0.0},
                                              {"memory_used_pct", 0.0},
                                              {"swap_used_pct", 0.0},
                                              {"volume_free_pct", 0.0}}}}},
        {"thresholds upper", nlohmann::json{{"thresholds",
                                             {{"cpu_sustain_used_pct", 100.0},
                                              {"memory_used_pct", 100.0},
                                              {"swap_used_pct", 100.0},
                                              {"volume_free_pct", 100.0}}}}},
        {"cpu_sustain_samples lower", nlohmann::json{{"thresholds", {{"cpu_sustain_samples", 1}}}}},
        {"cpu_sustain_samples upper",
         nlohmann::json{{"thresholds", {{"cpu_sustain_samples", 3600}}}}},
        {"log_repeat_sec lower", nlohmann::json{{"thresholds", {{"log_repeat_sec", 0}}}}},
        {"log_repeat_sec upper", nlohmann::json{{"thresholds", {{"log_repeat_sec", 86400}}}}},
        {"raw_retention_hours lower", nlohmann::json{{"history", {{"raw_retention_hours", 1}}}}},
        {"raw_retention_hours upper", nlohmann::json{{"history", {{"raw_retention_hours", 720}}}}},
        {"max_retention_days lower", nlohmann::json{{"history", {{"max_retention_days", 1}}}}},
        {"max_retention_days upper", nlohmann::json{{"history", {{"max_retention_days", 3650}}}}},
        {"max_bytes zero", nlohmann::json{{"history", {{"max_bytes", 0}}}}},
        {"groups bool false", nlohmann::json{{"sampling", {{"groups", {{"cpu", false}}}}}}},
        {"empty arrays", nlohmann::json{{"disk",
                                         {{"devices", nlohmann::json::array()},
                                          {"watch_paths", nlohmann::json::array()}}}}},
        {"unknown field ignored", nlohmann::json{{"unknown_section", {{"x", 1}}}}},
        {"empty patch", nlohmann::json::object()},
    };
    for (const auto& item : accepted) {
        EXPECT_NO_THROW((void)SysmonConfig::apply_patch(current, item.patch)) << item.label;
    }
}

// #81 配置边界：端点值必须**按原值**进入生效配置——只放行但钳制/丢弃也算失败
// （上一个用例只证明"不抛"，会放过一个静默钳制的实现）。
TEST(SysmonConfigTest, BoundaryValuesRoundTripThroughTypedConfig) {
    const auto current = nlohmann::json(SysmonConfig::default_config());
    // 每个字段取一个端点（上下界交替，覆盖两个方向）。
    const auto patch = nlohmann::json{
        {"sampling",
         {{"measure_interval_ms", 60000},
          {"record_interval_sec", 1},
          {"phase_offset_sec", 86400},
          {"groups", {{"cpu", false}, {"temperature", false}}}}},
        {"disk",
         {{"watch_paths", nlohmann::json::array({"/srv/shm", "/srv/db"})},
          {"devices", nlohmann::json::array({"sda"})}}},
        {"network", {{"hide_virtual", false}}},
        {"history",
         {{"enabled", false},
          {"raw_retention_hours", 1},
          {"max_retention_days", 3650},
          {"max_bytes", 0}}},
        {"realtime", {{"push_interval_sec", 3600}}},
        {"thresholds",
         {{"cpu_sustain_used_pct", 0.0},
          {"cpu_sustain_samples", 3600},
          {"memory_used_pct", 100.0},
          {"swap_used_pct", 0.0},
          {"volume_free_pct", 100.0},
          {"log_repeat_sec", 86400}}},
    };
    const SysmonConfig typed = SysmonConfig::apply_patch(current, patch).get<SysmonConfig>();

    EXPECT_EQ(typed.sampling.measure_interval_ms, 60000);
    EXPECT_EQ(typed.sampling.record_interval_sec, 1);
    EXPECT_EQ(typed.sampling.phase_offset_sec, 86400);
    EXPECT_EQ(typed.sampling.groups.at("cpu"), false);
    EXPECT_EQ(typed.sampling.groups.at("temperature"), false);
    ASSERT_EQ(typed.disk.watch_paths.size(), 2U);
    EXPECT_EQ(typed.disk.watch_paths[1], "/srv/db");
    ASSERT_EQ(typed.disk.devices.size(), 1U);
    EXPECT_EQ(typed.disk.devices[0], "sda");
    EXPECT_FALSE(typed.network.hide_virtual);
    EXPECT_FALSE(typed.history.enabled);
    EXPECT_EQ(typed.history.raw_retention_hours, 1);
    EXPECT_EQ(typed.history.max_retention_days, 3650);
    EXPECT_EQ(typed.history.max_bytes, 0U);
    EXPECT_EQ(typed.realtime.push_interval_sec, 3600);
    EXPECT_DOUBLE_EQ(typed.thresholds.cpu_sustain_used_pct, 0.0);
    EXPECT_EQ(typed.thresholds.cpu_sustain_samples, 3600);
    EXPECT_DOUBLE_EQ(typed.thresholds.memory_used_pct, 100.0);
    EXPECT_DOUBLE_EQ(typed.thresholds.swap_used_pct, 0.0);
    EXPECT_DOUBLE_EQ(typed.thresholds.volume_free_pct, 100.0);
    EXPECT_EQ(typed.thresholds.log_repeat_sec, 86400);
}

// #123 契约 sysmon §3.1：阈值段的键集合已收敛——`log_repeat_sec` 必填，
// 旧段名 `alerts` 与已删除的 `volume_free_bytes` / `cooldown_sec` 不再是阈值来源。
TEST(SysmonConfigTest, ThresholdsSectionRequiresNewKeysAndIgnoresRemovedOnes) {
    const auto current = nlohmann::json(SysmonConfig::default_config());

    // 默认配置本身就是新形状（没有 alerts 段，也没有已删除的键）。
    EXPECT_TRUE(current.contains("thresholds"));
    EXPECT_FALSE(current.contains("alerts"));
    EXPECT_FALSE(current["thresholds"].contains("volume_free_bytes"));
    EXPECT_FALSE(current["thresholds"].contains("cooldown_sec"));
    EXPECT_EQ(current["thresholds"]["log_repeat_sec"], 600);
    EXPECT_DOUBLE_EQ(current["thresholds"]["volume_free_pct"].get<double>(), 10.0);

    // 缺 `log_repeat_sec` 的全量配置非法（必填键）：拿旧形状冒充新段会被拒绝。
    auto legacy_shaped = current;
    legacy_shaped["thresholds"].erase("log_repeat_sec");
    legacy_shaped["thresholds"]["volume_free_bytes"] = 1'073'741'824;
    legacy_shaped["thresholds"]["cooldown_sec"] = 600;
    const auto error = SysmonConfig::validate(legacy_shaped);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("log_repeat_sec"), std::string::npos);

    // 已删除的键若作为未知字段出现则被忽略（总则 §8），不改变生效值。
    const auto merged = SysmonConfig::apply_patch(
        current, {{"thresholds", {{"volume_free_bytes", 0}, {"cooldown_sec", 0}}}});
    const SysmonConfig typed = merged.get<SysmonConfig>();
    EXPECT_EQ(typed.thresholds.log_repeat_sec, 600);
    EXPECT_FALSE(nlohmann::json(typed)["thresholds"].contains("volume_free_bytes"));
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

// ===== #75：卷、关注路径与共享内存目录 =====

/// 契约 sysmon §2.4：读不到的卷必须输出 available=false + reason，且三个容量字段为 null。
TEST(SysmonSnapshotValidateTest, AcceptsVolumeUnavailableWithNullCapacities) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                           {"watched", true},
                                           {"available", false},
                                           {"total_bytes", nullptr},
                                           {"used_bytes", nullptr},
                                           {"used_ratio", nullptr},
                                           {"reason", "no_permission"}}});
    j["watch_paths"] = nlohmann::json::array({{{"path", "/srv/shm"},
                                               {"mount", nullptr},
                                               {"available", false},
                                               {"reason", "path_not_found"}}});
    j["availability"]["metrics"]["volumes"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

/// 契约 sysmon §7：available=false 的卷不得用 0 冒充容量，且必须给 reason。
TEST(SysmonSnapshotValidateTest, RejectsUnavailableVolumeWithZeroCapacities) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                           {"watched", false},
                                           {"available", false},
                                           {"total_bytes", 0},
                                           {"used_bytes", 0},
                                           {"used_ratio", nullptr},
                                           {"reason", "no_permission"}}});
    j["availability"]["metrics"]["volumes"] = {{"available", true}};
    // 读不到不是 0：这是本票最核心的协议约束（契约 sysmon §2.4）。
    EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
}

TEST(SysmonSnapshotValidateTest, RejectsUnavailableVolumeWithoutReason) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                           {"watched", false},
                                           {"available", false},
                                           {"total_bytes", nullptr},
                                           {"used_bytes", nullptr},
                                           {"used_ratio", nullptr}}});
    j["availability"]["metrics"]["volumes"] = {{"available", true}};
    EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
}

/// 契约 sysmon §7：可读卷的容量自洽（used ≤ total、比率在 [0,1]）、不得出现 reason。
TEST(SysmonSnapshotValidateTest, RejectsInconsistentReadableVolumes) {
    {  // used > total
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                               {"watched", false},
                                               {"available", true},
                                               {"total_bytes", 100},
                                               {"used_bytes", 150},
                                               {"used_ratio", 0.5}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 比率越界
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                               {"watched", false},
                                               {"available", true},
                                               {"total_bytes", 100},
                                               {"used_bytes", 50},
                                               {"used_ratio", 1.5}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // available=true 却带 reason
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                               {"watched", false},
                                               {"available", true},
                                               {"total_bytes", 100},
                                               {"used_bytes", 50},
                                               {"used_ratio", 0.5},
                                               {"reason", "collect_failed"}}});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 挂载点重复
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        const nlohmann::json volume = {{"mount", "/data"},  {"watched", false},
                                       {"available", true}, {"total_bytes", 100},
                                       {"used_bytes", 50},  {"used_ratio", 0.5}};
        j["volumes"] = nlohmann::json::array({volume, volume});
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
}

/// 契约 sysmon §7：落卷解析成功时 mount 必须指向同帧 volumes 里真实存在的卷。
TEST(SysmonSnapshotValidateTest, RejectsWatchPathPointingToUnknownMount) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["volumes"] = nlohmann::json::array({{{"mount", "/data"},
                                           {"watched", false},
                                           {"available", true},
                                           {"total_bytes", 100},
                                           {"used_bytes", 50},
                                           {"used_ratio", 0.5}}});
    j["watch_paths"] = nlohmann::json::array(
        {{{"path", "/srv/shm"}, {"mount", "/elsewhere"}, {"available", true}}});
    j["availability"]["metrics"]["volumes"] = {{"available", true}};
    EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
}

/// 契约 sysmon §2.4：shm_dir 目录不可读时整个对象为 null，不得输出全 0 的对象。
TEST(SysmonSnapshotValidateTest, AcceptsShmDirZeroWhenDirectoryIsEmpty) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    // 空目录是真实事实：total_bytes=0 + page_files=0 + oldest_file_age_sec=null 合法。
    j["shm_dir"] = {{"path", "/srv/dztrader/shm"},
                    {"total_bytes", 0},
                    {"page_files", 0},
                    {"oldest_file_age_sec", nullptr}};
    j["availability"]["metrics"]["shm_dir"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

TEST(SysmonSnapshotValidateTest, RejectsNegativeShmDirAge) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["shm_dir"] = {{"path", "/srv/dztrader/shm"},
                    {"total_bytes", 10},
                    {"page_files", 1},
                    {"oldest_file_age_sec", -5}};
    j["availability"]["metrics"]["shm_dir"] = {{"available", true}};
    EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
}

/// 序列化往返：卷/落卷/shm 目录的手写 to_json/from_json 不得丢字段或改键名。
TEST(SysmonSnapshotJsonTest, DiskDomainsRoundTrip) {
    SysmonVolumeInfo volume;
    volume.mount = "C:\\";
    volume.watched = true;
    volume.available = false;
    volume.reason = "no_permission";
    const auto volume_json = nlohmann::json(volume);
    EXPECT_EQ(volume_json["mount"], "C:\\");
    EXPECT_TRUE(volume_json["watched"].get<bool>());
    EXPECT_FALSE(volume_json["available"].get<bool>());
    EXPECT_TRUE(volume_json["total_bytes"].is_null());
    EXPECT_TRUE(volume_json["used_bytes"].is_null());
    EXPECT_TRUE(volume_json["used_ratio"].is_null());
    EXPECT_EQ(volume_json["reason"], "no_permission");
    const auto volume_back = volume_json.get<SysmonVolumeInfo>();
    EXPECT_EQ(volume_back.mount, "C:\\");
    EXPECT_FALSE(volume_back.available);
    EXPECT_FALSE(volume_back.total_bytes.has_value());
    ASSERT_TRUE(volume_back.reason.has_value());
    EXPECT_EQ(*volume_back.reason, "no_permission");

    SysmonWatchPathInfo watch;
    watch.path = "/srv/shm";
    watch.mount = "/data";
    const auto watch_json = nlohmann::json(watch);
    EXPECT_EQ(watch_json["path"], "/srv/shm");
    EXPECT_EQ(watch_json["mount"], "/data");
    EXPECT_TRUE(watch_json["available"].get<bool>());
    EXPECT_FALSE(watch_json.contains("reason"));  // available=true 时省略 reason
    const auto watch_back = watch_json.get<SysmonWatchPathInfo>();
    EXPECT_EQ(watch_back.path, "/srv/shm");
    ASSERT_TRUE(watch_back.mount.has_value());
    EXPECT_EQ(*watch_back.mount, "/data");
}

// ===== #76：每物理盘 I/O（契约 sysmon §2.4、§7）=====

/// 序列化：disk_io[] 的键名与可空语义必须与契约一致（null = 计数器不可用而非 0）。
TEST(SysmonSnapshotJsonTest, DiskIoInfoSerializationKeepsContractKeys) {
    SysmonDiskIoInfo disk;
    disk.device = "PhysicalDrive0";
    disk.read_bytes_per_sec = 1'000'000.0;
    disk.write_bytes_per_sec = std::nullopt;
    disk.read_iops = 15.0;
    disk.write_iops = 5.0;
    disk.latency_ms = std::nullopt;
    disk.queue_depth = 3.0;
    disk.utilization = 0.5;

    const auto json = nlohmann::json(disk);
    EXPECT_EQ(json["device"], "PhysicalDrive0");
    EXPECT_DOUBLE_EQ(json["read_bytes_per_sec"].get<double>(), 1'000'000.0);
    // 计数器不可用的字段必须显式 null（不得省略、不得用 0）。
    EXPECT_TRUE(json["write_bytes_per_sec"].is_null());
    EXPECT_TRUE(json["latency_ms"].is_null());
    EXPECT_DOUBLE_EQ(json["utilization"].get<double>(), 0.5);

    // 首次采样：设备条目存在、派生字段全 null，是通过契约校验的合法形状。
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    SysmonDiskIoInfo first_frame;
    first_frame.device = "sda";
    j["disk_io"] = nlohmann::json::array({nlohmann::json(first_frame)});
    j["availability"]["metrics"]["disk_io"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

TEST(SysmonSnapshotValidateTest, AcceptsDiskIoEntries) {
    auto snap = make_snapshot();
    auto j = nlohmann::json(snap);
    j["disk_io"] = nlohmann::json::array({{{"device", "sda"},
                                           {"read_bytes_per_sec", 1'000'000.0},
                                           {"write_bytes_per_sec", 0.0},
                                           {"read_iops", 15.0},
                                           {"write_iops", 5.0},
                                           {"latency_ms", 1.25},
                                           {"queue_depth", 3.0},
                                           {"utilization", 0.5}},
                                          {{"device", "nvme0n1"},
                                           {"read_bytes_per_sec", nullptr},
                                           {"write_bytes_per_sec", nullptr},
                                           {"read_iops", nullptr},
                                           {"write_iops", nullptr},
                                           {"latency_ms", nullptr},
                                           {"queue_depth", nullptr},
                                           {"utilization", nullptr}}});
    j["availability"]["metrics"]["disk_io"] = {{"available", true}};
    EXPECT_EQ(SysmonSnapshot::validate(j), std::nullopt);
}

TEST(SysmonSnapshotValidateTest, RejectsInvalidDiskIoEntries) {
    const auto with_disk = [](const nlohmann::json& entry) {
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["disk_io"] = nlohmann::json::array({entry});
        j["availability"]["metrics"]["disk_io"] = {{"available", true}};
        return j;
    };
    const auto valid_entry = nlohmann::json{
        {"device", "sda"},    {"read_bytes_per_sec", 1.0}, {"write_bytes_per_sec", nullptr},
        {"read_iops", 1.0},   {"write_iops", nullptr},     {"latency_ms", nullptr},
        {"queue_depth", 0.0}, {"utilization", 0.5}};

    {  // utilization 是比率：[0,1] 之外非法
        auto entry = valid_entry;
        entry["utilization"] = 1.5;
        EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value());
        entry["utilization"] = -0.1;
        EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value());
    }
    {  // 速率/延迟/队列深度为负数非法
        for (const char* key : {"read_bytes_per_sec", "write_bytes_per_sec", "read_iops",
                                "write_iops", "latency_ms", "queue_depth"}) {
            auto entry = valid_entry;
            entry[key] = -1;
            EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value()) << key;
        }
    }
    {  // device 必须非空
        auto entry = valid_entry;
        entry["device"] = "";
        EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value());
        entry.erase("device");
        EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value());
    }
    {  // 非 null 时必须是数字
        auto entry = valid_entry;
        entry["read_iops"] = "fast";
        EXPECT_TRUE(SysmonSnapshot::validate(with_disk(entry)).has_value());
    }
    {  // disk_io 非 null 时必须是数组
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["disk_io"] = 42;
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
    {  // 同一帧内 device 唯一（契约 sysmon §2.4、§7）
        auto snap = make_snapshot();
        auto j = nlohmann::json(snap);
        j["disk_io"] = nlohmann::json::array({valid_entry, valid_entry});
        j["availability"]["metrics"]["disk_io"] = {{"available", true}};
        EXPECT_TRUE(SysmonSnapshot::validate(j).has_value());
    }
}

}  // namespace
}  // namespace dztrader::platform
