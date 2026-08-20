#include <gtest/gtest.h>

#include <drogon/drogon.h>

#include <dztrader/data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

#include "settings_controller.h"
#include "repository.h"
#include "shm_writer.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace dztrader::webui {
namespace {

class SettingsControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<SettingsCtrl> ctrl_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<Reader> reader_;
    std::shared_ptr<WebuiConfig> webui_cfg_;
    std::filesystem::path master_path_;
    std::filesystem::path webui_path_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        // 种子 admin/普通用户: is_admin 按 user_id attribute 查库判 role
        repo_->create_user("admin", "Administrator", "a@test.com", "pass", "admin");
        repo_->create_user("regular", "Regular User", "r@test.com", "pass", "user");

        shm_dir_ = std::filesystem::temp_directory_path() / "dz_test_settings";
        std::filesystem::remove_all(shm_dir_);
        ChannelConfig cfg{
            .channel_name = "dz_test_settings",
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        reader_ = Reader::create(meta_, "test_reader");
        // ShmWriter 注入共享 writer (唯一写入者), reader 观察写出的帧
        auto shm_writer = std::make_shared<ShmWriter>(
            std::make_shared<MultiWriter>(MultiWriter::create(meta_, "test_writer")));

        webui_cfg_ = std::make_shared<WebuiConfig>();
        webui_cfg_->token_ttl_sec = 3600;
        webui_cfg_->jwt_secret = "secret";
        webui_cfg_->notify_cache_size = 100;

        master_path_ = std::filesystem::temp_directory_path() / "dztraderd_test.json";
        webui_path_ = std::filesystem::temp_directory_path() / "webui_test.json";
        std::filesystem::remove(master_path_);
        std::filesystem::remove(webui_path_);

        ctrl_ = std::make_shared<SettingsCtrl>(
            repo_, shm_writer, master_path_, webui_path_, webui_cfg_);
    }

    void TearDown() override { std::filesystem::remove_all(shm_dir_); }

    drogon::HttpRequestPtr admin_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }
    drogon::HttpRequestPtr user_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        return req;
    }
    template <typename Fn, typename... Args>
    drogon::HttpResponsePtr invoke(Fn fn, const drogon::HttpRequestPtr& req, Args&&... args) {
        drogon::HttpResponsePtr captured;
        (ctrl_.get()->*fn)(req,
                          [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
                          std::forward<Args>(args)...);
        return captured;
    }
    static nlohmann::json parse_body(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

TEST_F(SettingsControllerTest, SetEventShmConfigForbidsNonAdmin) {
    auto req = user_req();
    req->setBody(R"({"check_interval_min":10})");
    auto resp = invoke(&SettingsCtrl::set_event_shm_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(SettingsControllerTest, SetEventShmConfigRejectsNonObject) {
    auto req = admin_req();
    req->setBody(R"([1,2])");
    auto resp = invoke(&SettingsCtrl::set_event_shm_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SettingsControllerTest, SetEventShmConfigDispatches) {
    auto req = admin_req();
    req->setBody(R"({"check_interval_min":10})");
    auto resp = invoke(&SettingsCtrl::set_event_shm_config, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parse_body(resp)["message"], "dispatched");

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_SET_EVENT_SHM_CONFIG);
}

TEST_F(SettingsControllerTest, SetEventShmConfigReturns503WhenWriterNotReady) {
    // 构造 shm_writer 为 null 的控制器: is_ready() 恒 false → 503 (对齐
    // market_source_controller_test.cpp 的 503 先例, 如 ToggleAutoLoginReturns503WhenWriterNotReady)
    auto ctrl_no_writer = std::make_shared<SettingsCtrl>(
        repo_, nullptr, master_path_, webui_path_, webui_cfg_);
    auto req = admin_req();
    req->setBody(R"({"check_interval_min":10})");
    drogon::HttpResponsePtr captured;
    ctrl_no_writer->set_event_shm_config(
        req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    // gtest 打印 std::shared_ptr 在 MSVC /W4 /WX 下触发 C4702(PrintSmartPointer),
    // 故用 .get() 降级为原始指针比较, 语义等价
    ASSERT_NE(captured.get(), nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k503ServiceUnavailable);
}

TEST_F(SettingsControllerTest, SetEventShmConfigRejectsMalformedJson) {
    auto req = admin_req();
    req->setBody("{not json");
    auto resp = invoke(&SettingsCtrl::set_event_shm_config, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

}  // namespace
}  // namespace dztrader::webui
