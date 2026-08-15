// ProcessConfigStore 单测：镜像管理、SET 流水线、强保证、remove、RTN 帧
#include "process_config.h"

#include <dztrader/core/core_data_type.h>  // DZ_FRAME_RTN_PROCESS_CONFIG (帧宏所在头)
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::master {
namespace {

nlohmann::json make_full_cfg(const std::string& display_name = "") {
    nlohmann::json cfg = {{"args", nlohmann::json::array()},
                          {"env", nlohmann::json::object()},
                          {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    if (!display_name.empty()) {
        cfg["display_name"] = display_name;
    }
    return cfg;
}

class ProcessConfigStoreTest : public ::testing::Test {
protected:
    static constexpr uint64_t MB = 1024ULL * 1024ULL;

    struct Call {
        std::string name;
        nlohmann::json full;
    };

    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<shm::ChannelMeta> meta_;
    std::optional<shm::MultiWriter> writer_;
    std::optional<shm::Reader> reader_;
    std::optional<ProcessConfigStore> store_;
    std::vector<Call> persist_calls_;
    std::vector<Call> apply_calls_;

    void SetUp() override {
        channel_name_ =
            "dz_test_proc_cfg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        std::filesystem::create_directories(shm_dir_);

        shm::ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer_ = shm::MultiWriter::create(meta_, "test_writer");
        reader_ = shm::Reader::create(meta_, "test_reader");
        store_.emplace(
            *writer_,
            [this](const std::string& name, const nlohmann::json& full) {
                persist_calls_.push_back({name, full});
            },
            [this](const std::string& name, const nlohmann::json& full) {
                apply_calls_.push_back({name, full});
            });
    }

    void TearDown() override {
        store_.reset();
        reader_.reset();
        writer_.reset();
        meta_.reset();
        std::filesystem::remove_all(shm_dir_);
    }

    // 排空 reader，返回最后一条 RTN_PROCESS_CONFIG 帧的 payload（无则 nullopt）
    std::optional<nlohmann::json> last_rtn_payload() {
        std::optional<nlohmann::json> last;
        while (auto* frame = reader_->next_frame()) {
            shm::FrameView view(frame);
            if (view.type() == DZ_FRAME_RTN_PROCESS_CONFIG) {
                const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                last = nlohmann::json::parse(data, data + view.ext_payload_size());
            }
        }
        return last;
    }
};

TEST_F(ProcessConfigStoreTest, LoadFillsMirrorAndSkipsInvalid) {
    nlohmann::json initial = {{"dzmd_ctp", make_full_cfg("CTP行情")},
                              {"bad_entry", nlohmann::json{{"args", "not-array"}}}};
    store_->load(initial);
    ASSERT_NE(store_->find("dzmd_ctp"), nullptr);
    EXPECT_EQ((*store_->find("dzmd_ctp"))["display_name"], "CTP行情");
    EXPECT_EQ(store_->find("bad_entry"), nullptr);  // 非法条目跳过
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigAppliesAndPersists) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    nlohmann::json patch = {{"display_name", "CTP行情源"}, {"env", {{"K", "V"}}}};
    EXPECT_NO_THROW(store_->set_process_config("dzmd_ctp", patch));
    // persist/apply 各调一次，参数为 name + 全量新值
    ASSERT_EQ(persist_calls_.size(), 1u);
    ASSERT_EQ(apply_calls_.size(), 1u);
    EXPECT_EQ(persist_calls_[0].name, "dzmd_ctp");
    EXPECT_EQ(persist_calls_[0].full["display_name"], "CTP行情源");
    EXPECT_EQ(persist_calls_[0].full["env"]["K"], "V");
    EXPECT_EQ(apply_calls_[0].full["display_name"], "CTP行情源");
    // 镜像更新
    EXPECT_EQ((*store_->find("dzmd_ctp"))["display_name"], "CTP行情源");
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigUnknownTargetThrows) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_THROW(store_->set_process_config("not_exist", nlohmann::json{{"display_name", "x"}}),
                 std::runtime_error);
    EXPECT_TRUE(persist_calls_.empty());  // 回调未调用
    EXPECT_TRUE(apply_calls_.empty());
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigInvalidPatchThrowsWithoutCallbacks) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_THROW(store_->set_process_config("dzmd_ctp", nlohmann::json{{"restart", nullptr}}),
                 std::runtime_error);
    EXPECT_TRUE(persist_calls_.empty());
    EXPECT_TRUE(apply_calls_.empty());
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigEmptyPatchIsNoop) {
    store_->load({{"dzmd_ctp", make_full_cfg("旧名")}});
    EXPECT_NO_THROW(store_->set_process_config("dzmd_ctp", nlohmann::json::object()));
    // 镜像不变；persist/apply 仍走（契约: 空对象无操作, 仍回 RTN 当前值）
    EXPECT_EQ((*store_->find("dzmd_ctp"))["display_name"], "旧名");
    ASSERT_EQ(persist_calls_.size(), 1u);
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigPersistFailureKeepsMirror) {
    // 覆写 persist_fn 为抛异常版本
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    // 直接构造第二个 store 太复杂, 改为: 记录 flag 由回调抛
    // （本用例通过替换回调实现: 重新 emplace）
    store_.reset();
    bool persist_throws = true;
    store_.emplace(
        *writer_,
        [this, &persist_throws](const std::string& name, const nlohmann::json& full) {
            persist_calls_.push_back({name, full});
            if (persist_throws) {
                throw std::runtime_error("disk full");
            }
        },
        [this](const std::string& name, const nlohmann::json& full) {
            apply_calls_.push_back({name, full});
        });
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_THROW(store_->set_process_config("dzmd_ctp", nlohmann::json{{"display_name", "新"}}),
                 std::runtime_error);
    EXPECT_TRUE(apply_calls_.empty());  // apply 未调用
    // 镜像不变（display_name 未被设置——make_full_cfg() 空名时不带该字段）
    EXPECT_FALSE((*store_->find("dzmd_ctp")).contains("display_name"));
}

TEST_F(ProcessConfigStoreTest, SetProcessConfigApplyFailureKeepsMirror) {
    store_.reset();
    bool apply_throws = true;
    store_.emplace(
        *writer_,
        [this](const std::string& name, const nlohmann::json& full) {
            persist_calls_.push_back({name, full});
        },
        [this, &apply_throws](const std::string& name, const nlohmann::json& full) {
            apply_calls_.push_back({name, full});
            if (apply_throws) {
                throw std::runtime_error("apply failed");
            }
        });
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_THROW(store_->set_process_config("dzmd_ctp", nlohmann::json{{"display_name", "新"}}),
                 std::runtime_error);
    // persist 已调用（文件已写——副作用窗口，头文件注释已声明），镜像不变
    ASSERT_EQ(persist_calls_.size(), 1u);
    EXPECT_EQ(apply_calls_.size(), 1u);
    EXPECT_FALSE((*store_->find("dzmd_ctp")).contains("display_name"));
}

TEST_F(ProcessConfigStoreTest, RemoveDeletesEntryAndCallsNull) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_NO_THROW(store_->remove("dzmd_ctp"));
    EXPECT_EQ(store_->find("dzmd_ctp"), nullptr);
    ASSERT_EQ(persist_calls_.size(), 1u);
    ASSERT_EQ(apply_calls_.size(), 1u);
    EXPECT_TRUE(persist_calls_[0].full.is_null());  // null = 删除
    EXPECT_TRUE(apply_calls_[0].full.is_null());
}

TEST_F(ProcessConfigStoreTest, RemoveUnknownTargetThrows) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_THROW(store_->remove("not_exist"), std::runtime_error);
    EXPECT_TRUE(persist_calls_.empty());
}

TEST_F(ProcessConfigStoreTest, RtnProcessConfigWritesFullMap) {
    store_->load({{"dzmd_ctp", make_full_cfg("CTP行情")},
                  {"dztd_ctp", make_full_cfg("CTP交易")}});
    store_->rtn_process_config();
    auto payload = last_rtn_payload();
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->size(), 2u);  // 全量（两个进程）
    EXPECT_EQ((*payload)["dzmd_ctp"]["display_name"], "CTP行情");
    EXPECT_EQ((*payload)["dztd_ctp"]["display_name"], "CTP交易");
}

TEST_F(ProcessConfigStoreTest, FindReturnsNullForUnknown) {
    store_->load({{"dzmd_ctp", make_full_cfg()}});
    EXPECT_EQ(store_->find("not_exist"), nullptr);
}

}  // namespace
}  // namespace dztrader::master
