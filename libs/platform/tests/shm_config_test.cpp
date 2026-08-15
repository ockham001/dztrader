#include <dztrader/data_type.h>
#include <dztrader/platform/shm_config.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using dztrader::platform::EventShmConfig;
using dztrader::platform::MdShmConfig;
using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class ShmConfigTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::filesystem::path cfg_path_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024ULL * 1024ULL;

    void SetUp() override {
        channel_name_ =
            "dz_test_shm_cfg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        std::filesystem::create_directories(shm_dir_);

        cfg_path_ = shm_dir_ / "config.json";
        std::filesystem::remove(cfg_path_);

        ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
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

    void write_cfg_file(const std::string& content) {
        std::ofstream ofs(cfg_path_, std::ios::binary | std::ios::trunc);
        ofs << content;
    }

    std::string read_cfg_file() {
        std::ifstream ifs(cfg_path_);
        return std::string(std::istreambuf_iterator<char>(ifs),
                           std::istreambuf_iterator<char>());
    }

    // 排空 reader,返回最后一个 RTN_EVENT_SHM_CONFIG(无 instance_id 帧)的 payload
    nlohmann::json read_last_event_rtn() {
        nlohmann::json last;
        while (auto* frame = reader_->next_frame()) {
            FrameView view(frame);
            if (view.type() == DZ_FRAME_RTN_EVENT_SHM_CONFIG) {
                const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                last = nlohmann::json::parse(data, data + view.ext_payload_size());
            }
        }
        return last;
    }

    // 排空 reader,返回最后一个 RTN_MD_SHM_CONFIG(含 instance_id 帧)的 payload,并填 instance_id
    nlohmann::json read_last_md_rtn(std::string& out_instance_id) {
        nlohmann::json last;
        out_instance_id.clear();
        while (auto* frame = reader_->next_frame()) {
            FrameView view(frame);
            if (view.type() == DZ_FRAME_RTN_MD_SHM_CONFIG) {
                const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                last = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
                out_instance_id = view.ext_inst_id();
            }
        }
        return last;
    }
};

// ===== EventShmConfig: load() 测试 =====

// 1. 文件缺失:load 后 rtn 推默认值(page_size_mb=64,其余 0/空)
TEST_F(ShmConfigTest, EventLoadDefaultsWhenFileMissing) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();  // 不抛

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);
    EXPECT_TRUE(rtn["preload_points"].is_object());
    EXPECT_TRUE(rtn["preload_points"].empty());
    EXPECT_EQ(rtn["check_interval_min"], 0);
    EXPECT_EQ(rtn["check_pages"], 0);
    EXPECT_EQ(rtn["check_bytes"], 0);
}

// 2. 文件有合法 event_shm section:load 后 rtn 推文件值
TEST_F(ShmConfigTest, EventLoadValidConfig) {
    write_cfg_file(R"({"event_shm":{"page_size_mb":128,
                    "preload_points":{"08:45":{"pages":2,"bytes":1024}},
                    "check_interval_min":10,"check_pages":3,"check_bytes":4096}})");

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 128);
    EXPECT_EQ(rtn["preload_points"]["08:45"]["pages"], 2);
    EXPECT_EQ(rtn["preload_points"]["08:45"]["bytes"], 1024);
    EXPECT_EQ(rtn["check_interval_min"], 10);
    EXPECT_EQ(rtn["check_pages"], 3);
    EXPECT_EQ(rtn["check_bytes"], 4096);
}

// 3. 文件 JSON 损坏:load 不抛,自愈后文件被修复为默认值
TEST_F(ShmConfigTest, EventLoadCorruptFileHeals) {
    write_cfg_file("{not a valid json");
    EXPECT_FALSE(read_cfg_file().empty());

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();  // 不抛

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["event_shm"]["page_size_mb"], 64);

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);
}

// 4. 文件 page_size_mb=0(非法):load 后用默认值
TEST_F(ShmConfigTest, EventLoadZeroPageSizeFallsBackToDefault) {
    write_cfg_file(R"({"event_shm":{"page_size_mb":0}})");

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);  // 回退默认
}

// 5. 文件 page_size_mb 为字符串(类型错):load 后用默认值
TEST_F(ShmConfigTest, EventLoadNonIntegerPageSizeFallsBackToDefault) {
    write_cfg_file(R"({"event_shm":{"page_size_mb":"32"}})");

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);
}

// 6. section 缺失:load 后用默认值
TEST_F(ShmConfigTest, EventLoadSectionMissingFallsBackToDefault) {
    write_cfg_file(R"({"other":{"foo":1}})");

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);
}

// 7. 文件 preload_points 含非法 key:load 时跳过该 key
TEST_F(ShmConfigTest, EventLoadSkipsInvalidPreloadKey) {
    write_cfg_file(R"({"event_shm":{"preload_points":{
        "08:45":{"pages":1,"bytes":0},
        "25:99":{"pages":2,"bytes":0}
    }}})");

    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_TRUE(rtn["preload_points"].contains("08:45"));
    EXPECT_FALSE(rtn["preload_points"].contains("25:99"));
}

// ===== EventShmConfig: set_shm_config() 测试 =====

// 8. 空 patch {} 无操作,cfg_ 不变
TEST_F(ShmConfigTest, EventSetEmptyPatchIsNoOp) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    nlohmann::json patch = nlohmann::json::object();
    cfg.set_shm_config(patch);  // 不抛
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));  // 无操作:不写盘

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);
    EXPECT_TRUE(rtn["preload_points"].empty());
    EXPECT_EQ(rtn["check_interval_min"], 0);
}

// 9. 增量改 check_interval_min,其余不变
TEST_F(ShmConfigTest, EventSetIncrementCheckInterval) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"check_interval_min", 10}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 10);
    EXPECT_EQ(rtn["check_pages"], 0);  // 不变
    EXPECT_EQ(rtn["check_bytes"], 0);
}

// 10. 递归合并 preload_points:08:45 的 pages 改 2,bytes 保留
TEST_F(ShmConfigTest, EventSetRecursiveMergePreloadPoint) {
    write_cfg_file(R"({"event_shm":{"preload_points":{
        "08:45":{"pages":1,"bytes":1024}
    }}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"preload_points", {{"08:45", {{"pages", 2}}}}}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["preload_points"]["08:45"]["pages"], 2);
    EXPECT_EQ(rtn["preload_points"]["08:45"]["bytes"], 1024);  // 保留旧值
}

// 11. preload_points 内部 null 删除 key
TEST_F(ShmConfigTest, EventSetNullDeletesPreloadKey) {
    write_cfg_file(R"({"event_shm":{"preload_points":{
        "08:45":{"pages":1,"bytes":0},
        "20:00":{"pages":2,"bytes":0}
    }}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"preload_points", {{"08:45", nullptr}}}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_FALSE(rtn["preload_points"].contains("08:45"));
    EXPECT_TRUE(rtn["preload_points"].contains("20:00"));  // 保留
}

// 12. preload_points 为 {} 时纯 RFC 7386 = 无操作
TEST_F(ShmConfigTest, EventSetEmptyPreloadPointsIsNoOp) {
    write_cfg_file(R"({"event_shm":{"preload_points":{
        "08:45":{"pages":1,"bytes":0}
    }}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"preload_points", nlohmann::json::object()}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_TRUE(rtn["preload_points"].contains("08:45"));  // 未清空
}

// 13. page_size_mb 不可变:patch 含 page_size_mb 完全跳过,即使 null 也不报错
TEST_F(ShmConfigTest, EventSetPageSizeMbSkipped) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"page_size_mb", 999}});  // 被忽略
    cfg.set_shm_config({{"page_size_mb", nullptr}});  // null 也跳过,不报错

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 64);  // 原值不变
}

// 14. 新增 preload_points key 时缺失 bytes 补默认值 0
TEST_F(ShmConfigTest, EventSetNewPreloadKeyFillsDefaults) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"preload_points", {{"09:00", {{"pages", 1}}}}}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["preload_points"]["09:00"]["pages"], 1);
    EXPECT_EQ(rtn["preload_points"]["09:00"]["bytes"], 0);  // 补默认
}

// 15. patch check_interval_min=null 抛异常,cfg_ 不变
TEST_F(ShmConfigTest, EventSetNullCheckIntervalThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_interval_min", nullptr}}), std::runtime_error);

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 0);  // 仍是默认值
}

// 16. preload_points 本身为 null 抛异常(顶层 null 非法)
TEST_F(ShmConfigTest, EventSetNullPreloadPointsTopLevelThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"preload_points", nullptr}}), std::runtime_error);
}

// 17. 非 object payload(数组)抛异常
TEST_F(ShmConfigTest, EventSetNonObjectPayloadThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config(nlohmann::json::array({1, 2, 3})), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config(nlohmann::json("string")), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config(nlohmann::json(123)), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config(nullptr), std::runtime_error);
}

// 18. 类型不匹配(check_pages 传字符串)抛异常
TEST_F(ShmConfigTest, EventSetTypeMismatchThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_pages", "8"}}), std::runtime_error);
}

// 19. 范围超界抛异常:check_interval_min=1441 / pages=9 / bytes=2^40+1
TEST_F(ShmConfigTest, EventSetRangeErrorThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_interval_min", 1441}}), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config({{"check_pages", 9}}), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config({{"check_bytes", (1LL << 40) + 1}}), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"08:45", {{"pages", 9}}}}}}), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"08:45", {{"bytes", (1LL << 40) + 1}}}}}}), std::runtime_error);
}

// 20. preload_points key 格式非法抛异常
TEST_F(ShmConfigTest, EventSetInvalidKeyFormatThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"24:00", {{"pages", 1}}}}}}), std::runtime_error);  // HH 超 23
    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"08:60", {{"pages", 1}}}}}}), std::runtime_error);  // MM 超 59
    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"8:45", {{"pages", 1}}}}}}), std::runtime_error);   // 非 HH:MM
    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"abcd", {{"pages", 1}}}}}}), std::runtime_error);
}

// 21. preload_points value 非 object 非 null(数字)抛异常
TEST_F(ShmConfigTest, EventSetPreloadValueNonObjectThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"08:45", 5}}}}), std::runtime_error);
}

// 22. patch 含额外字段:成功,extra 被忽略,cfg_ 不含 extra
TEST_F(ShmConfigTest, EventSetExtraFieldIgnored) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    nlohmann::json patch = {{"check_interval_min", 5}, {"extra", "foo"}, {"unknown", 123}};
    cfg.set_shm_config(patch);

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 5);
    EXPECT_FALSE(rtn.contains("extra"));
    EXPECT_FALSE(rtn.contains("unknown"));
}

// 23. 非整数值(5.5)抛异常
TEST_F(ShmConfigTest, EventSetNonIntegralNumberThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_pages", 5.5}}), std::runtime_error);
}

// 23b. 超 int64 可表示范围的整数 double(2^63、1e30)拒绝,不触发 UB 转换
TEST_F(ShmConfigTest, EventSetHugeIntegralDoubleThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_pages", 9223372036854775808.0}}),
                 std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config({{"check_pages", 1e30}}), std::runtime_error);
}

// 24. 整数值浮点(5.0)接受
TEST_F(ShmConfigTest, EventSetIntegralFloatAccepted) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"check_pages", 5.0}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_pages"], 5);
}

// 25. 负值(check_pages=-1)抛异常(uint64 字段不可负)
TEST_F(ShmConfigTest, EventSetNegativeThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config({{"check_pages", -1}}), std::runtime_error);
    EXPECT_THROW(cfg.set_shm_config({{"check_interval_min", -1}}), std::runtime_error);
}

// 26. preload_points 内部 pages=null 抛异常(只有 key 的 value 能 null)
TEST_F(ShmConfigTest, EventSetNullPagesInPointThrows) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_shm_config(
        {{"preload_points", {{"08:45", {{"pages", nullptr}}}}}}), std::runtime_error);
}

// 27. save 失败时 cfg_ 不变(强保证):cfg_path 指向不存在父目录
TEST_F(ShmConfigTest, EventSetFailureRollsBackCfg) {
    auto bad_path = shm_dir_ / "no_such_dir" / "config.json";
    EventShmConfig cfg(bad_path, *writer_);
    cfg.load();  // 文件缺失,不触发 save,cfg_ = 默认值

    EXPECT_THROW(cfg.set_shm_config({{"check_interval_min", 10}}), std::runtime_error);

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 0);  // save 失败,cfg_ 回滚
}

// 28. 多次 set 累积生效
TEST_F(ShmConfigTest, EventSetMultipleAccumulate) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"check_interval_min", 5}});
    cfg.set_shm_config({{"check_pages", 2}});
    cfg.set_shm_config({{"preload_points", {{"08:45", {{"pages", 1}}}}}});

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 5);
    EXPECT_EQ(rtn["check_pages"], 2);
    EXPECT_EQ(rtn["preload_points"]["08:45"]["pages"], 1);
    EXPECT_EQ(rtn["preload_points"]["08:45"]["bytes"], 0);
}

// ===== EventShmConfig: rtn_shm_config() 测试 =====

// 29. rtn 推送全量配置,无 error 字段
TEST_F(ShmConfigTest, EventRtnSendsFullConfigNoError) {
    write_cfg_file(R"({"event_shm":{"page_size_mb":256,
                    "preload_points":{"08:45":{"pages":1,"bytes":0}},
                    "check_interval_min":5,"check_pages":1,"check_bytes":0}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["page_size_mb"], 256);
    EXPECT_EQ(rtn["check_interval_min"], 5);
    EXPECT_FALSE(rtn.contains("error"));  // 契约:RTN 无 error 字段
}

// 30. set 失败后 rtn 推送旧值
TEST_F(ShmConfigTest, EventRtnAfterFailedSetSendsOldValue) {
    write_cfg_file(R"({"event_shm":{"check_interval_min":5}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();  // cfg_ check_interval_min=5

    EXPECT_THROW(cfg.set_shm_config({{"check_interval_min", 9999}}), std::runtime_error);

    cfg.rtn_shm_config();
    auto rtn = read_last_event_rtn();
    EXPECT_EQ(rtn["check_interval_min"], 5);  // 旧值
}

// 31. 持久化:set 后文件被更新
TEST_F(ShmConfigTest, EventSetPersistsToFile) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();
    cfg.set_shm_config({{"check_interval_min", 15}});

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["event_shm"]["check_interval_min"], 15);
    EXPECT_EQ(file_json["event_shm"]["page_size_mb"], 64);  // 默认值也持久化
}

// ===== MdShmConfig 测试(行情通道:含 instance_id 帧) =====

// 32. Md 默认 page_size_mb=1024
TEST_F(ShmConfigTest, MdLoadDefaultsPageSizeMb1024) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));

    MdShmConfig cfg("dzmd_ctp", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    std::string iid;
    auto rtn = read_last_md_rtn(iid);
    EXPECT_EQ(rtn["page_size_mb"], 1024);  // Md 默认
    EXPECT_EQ(iid, "dzmd_ctp");
}

// 33. Md RTN 含 instance_id,全量,无 error
TEST_F(ShmConfigTest, MdRtnIsInstFrameWithInstanceId) {
    write_cfg_file(R"({"md_shm":{"page_size_mb":2048,
                    "preload_points":{"09:30":{"pages":2,"bytes":512}},
                    "check_interval_min":15,"check_pages":4,"check_bytes":2048}})");
    MdShmConfig cfg("dzmd_ctp", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_shm_config();
    std::string iid;
    auto rtn = read_last_md_rtn(iid);
    EXPECT_EQ(iid, "dzmd_ctp");  // 帧头 instance_id
    EXPECT_EQ(rtn["page_size_mb"], 2048);
    EXPECT_EQ(rtn["preload_points"]["09:30"]["pages"], 2);
    EXPECT_EQ(rtn["check_interval_min"], 15);
    EXPECT_FALSE(rtn.contains("error"));
}

// 34. Md set 合并语义与 Event 一致(抽样验证)
TEST_F(ShmConfigTest, MdSetMergeSameSemantics) {
    MdShmConfig cfg("dzmd_ctp", cfg_path_, *writer_);
    cfg.load();

    cfg.set_shm_config({{"check_interval_min", 30}});
    cfg.set_shm_config({{"preload_points", {{"09:30", {{"pages", 3}}}}}});
    EXPECT_THROW(cfg.set_shm_config({{"check_pages", 99}}), std::runtime_error);  // 超范围

    cfg.rtn_shm_config();
    std::string iid;
    auto rtn = read_last_md_rtn(iid);
    EXPECT_EQ(rtn["check_interval_min"], 30);
    EXPECT_EQ(rtn["preload_points"]["09:30"]["pages"], 3);
    EXPECT_EQ(rtn["preload_points"]["09:30"]["bytes"], 0);  // 补默认
    EXPECT_EQ(rtn["check_pages"], 0);  // 失败的 set 未生效
}

// 35. Md load 自愈:文件损坏后用默认值 + 修复文件
TEST_F(ShmConfigTest, MdLoadCorruptFileHeals) {
    write_cfg_file("%%% broken json %%%");

    MdShmConfig cfg("dzmd_ctp", cfg_path_, *writer_);
    cfg.load();  // 不抛

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["md_shm"]["page_size_mb"], 1024);  // 修复为 Md 默认

    cfg.rtn_shm_config();
    std::string iid;
    auto rtn = read_last_md_rtn(iid);
    EXPECT_EQ(rtn["page_size_mb"], 1024);
}

// 36. Event 与 Md 互不干扰:同通道不同帧类型
TEST_F(ShmConfigTest, EventAndMdCoexistOnSameChannel) {
    EventShmConfig ecfg(cfg_path_, *writer_);
    ecfg.load();
    ecfg.set_shm_config({{"check_interval_min", 7}});
    ecfg.rtn_shm_config();

    // Md 用不同 cfg 文件避免覆盖
    auto md_cfg_path = shm_dir_ / "md_config.json";
    MdShmConfig mcfg("dzmd_ctp", md_cfg_path, *writer_);
    mcfg.load();
    mcfg.set_shm_config({{"check_pages", 4}});
    mcfg.rtn_shm_config();

    std::string iid;
    auto md_rtn = read_last_md_rtn(iid);
    EXPECT_EQ(iid, "dzmd_ctp");
    EXPECT_EQ(md_rtn["check_pages"], 4);
    EXPECT_EQ(md_rtn["page_size_mb"], 1024);
}

// 37. getter 返回正确值
TEST_F(ShmConfigTest, GettersReturnCorrectValues) {
    write_cfg_file(R"({"event_shm":{"page_size_mb":128,
                    "preload_points":{"08:45":{"pages":2,"bytes":1024}},
                    "check_interval_min":10,"check_pages":3,"check_bytes":4096}})");
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_EQ(cfg.page_size_mb(), 128u);
    EXPECT_EQ(cfg.check_interval_min(), 10);
    EXPECT_EQ(cfg.check_pages(), 3);
    EXPECT_EQ(cfg.check_bytes(), 4096u);
    auto pp = cfg.preload_points();
    ASSERT_EQ(pp.size(), 1u);
    EXPECT_EQ(pp[0].time, "08:45");
    EXPECT_EQ(pp[0].pages, 2);
    EXPECT_EQ(pp[0].bytes, 1024u);
}

// 38. getter 返回默认值(空配置)
TEST_F(ShmConfigTest, GettersReturnDefaultsWhenEmpty) {
    EventShmConfig cfg(cfg_path_, *writer_);
    cfg.load();

    EXPECT_EQ(cfg.page_size_mb(), 64u);  // Event 默认
    EXPECT_EQ(cfg.check_interval_min(), 0);
    EXPECT_EQ(cfg.check_pages(), 0);
    EXPECT_EQ(cfg.check_bytes(), 0u);
    EXPECT_TRUE(cfg.preload_points().empty());
}

}  // namespace
