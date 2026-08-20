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

        // 临时文件名带 dz_settings_ 前缀, 避免与 config_test.cpp 的 webui_test.json 在临时目录碰撞
        master_path_ = std::filesystem::temp_directory_path() / "dz_settings_dztraderd_test.json";
        webui_path_ = std::filesystem::temp_directory_path() / "dz_settings_webui_test.json";
        std::filesystem::remove(master_path_);
        std::filesystem::remove(webui_path_);
        remove_corrupt_backups();   // 防历史运行残留的备份文件弱化备份断言

        ctrl_ = std::make_shared<SettingsCtrl>(
            repo_, shm_writer, master_path_, webui_path_, webui_cfg_);

        // Task 3: 写初始 webui.json, 供 get_webui / set_webui 测试
        {
            std::ofstream ofs(webui_path_);
            ofs << R"({"server":{"listen":"0.0.0.0","port":8080},"log":{"level":"info"},
                       "auth":{"jwt_secret":"secret","token_ttl_sec":3600},
                       "admin":{"username":"admin"},"notify":{"cache_size":100}})";
        }
    }

    // 清理 webui.json.corrupt.* 备份残留: SetUp 防历史残留弱化备份断言, TearDown 不留垃圾
    void remove_corrupt_backups() {
        const std::string prefix = webui_path_.filename().string() + ".corrupt.";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(webui_path_.parent_path(), ec)) {
            if (entry.path().filename().string().find(prefix) == 0) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    void TearDown() override {
        std::filesystem::remove_all(shm_dir_);
        std::filesystem::remove(webui_path_);
        std::filesystem::remove(master_path_);
        remove_corrupt_backups();
    }

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

TEST_F(SettingsControllerTest, GetWebuiReturnsMasked) {
    auto resp = invoke(&SettingsCtrl::get_webui, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["token_ttl_sec"], 3600);
    EXPECT_EQ(body["server_port"], 8080);
    EXPECT_EQ(body["server_listen"], "0.0.0.0");
    EXPECT_EQ(body["notify_cache_size"], 100);
    EXPECT_EQ(body["jwt_secret_set"], true);
    EXPECT_FALSE(body.contains("jwt_secret"));
}

TEST_F(SettingsControllerTest, GetWebuiAsNonAdminReturns403) {
    auto resp = invoke(&SettingsCtrl::get_webui, user_req());
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(SettingsControllerTest, SetWebuiAsNonAdminReturns403) {
    auto req = user_req();
    req->setBody(R"({"token_ttl_sec":7200})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(SettingsControllerTest, GetWebuiReportsUnsetJwtSecret) {
    // 运行期持有者 jwt_secret 为空 → jwt_secret_set=false (不回传明文)
    webui_cfg_->jwt_secret.clear();
    auto resp = invoke(&SettingsCtrl::get_webui, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parse_body(resp)["jwt_secret_set"], false);
}

TEST_F(SettingsControllerTest, SetWebuiPersistsAndHotApplies) {
    auto req = admin_req();
    req->setBody(R"({"token_ttl_sec":7200})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    // 热生效: 共享持有者已更新
    EXPECT_EQ(webui_cfg_->token_ttl_sec, 7200);
    // 持久化: 文件已写, 且保留其他 section
    std::ifstream ifs(webui_path_);
    auto disk = nlohmann::json::parse(ifs);
    EXPECT_EQ(disk["auth"]["token_ttl_sec"], 7200);
    EXPECT_EQ(disk["notify"]["cache_size"], 100);
    EXPECT_EQ(disk["admin"]["username"], "admin");
}

TEST_F(SettingsControllerTest, SetWebuiRejectsUnknownField) {
    auto req = admin_req();
    req->setBody(R"({"jwt_secret":"x"})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    // 拒绝不改状态: 共享持有者保持原值 (热生效字段未被误改)
    EXPECT_EQ(webui_cfg_->token_ttl_sec, 3600);
}

TEST_F(SettingsControllerTest, SetWebuiRejectsTooSmallTtl) {
    auto req = admin_req();
    req->setBody(R"({"token_ttl_sec":30})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SettingsControllerTest, SetWebuiRejectsTooLargeTtl) {
    auto req = admin_req();
    req->setBody(R"({"token_ttl_sec":604801})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    // 拒绝不改状态: 热生效字段未被误改
    EXPECT_EQ(webui_cfg_->token_ttl_sec, 3600);
}

TEST_F(SettingsControllerTest, SetWebuiRejectsTooLargeCacheSize) {
    auto req = admin_req();
    req->setBody(R"({"notify_cache_size":1000001})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SettingsControllerTest, SetWebuiRejectsMixedInvalidCacheAtomically) {
    // 混合 patch 中任一字段非法 → 整体拒绝 (原子), 磁盘与热生效值均不动
    auto req = admin_req();
    req->setBody(R"({"token_ttl_sec":7200,"notify_cache_size":2000000})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(webui_cfg_->token_ttl_sec, 3600);
    std::ifstream ifs(webui_path_);
    auto disk = nlohmann::json::parse(ifs);
    EXPECT_EQ(disk["auth"]["token_ttl_sec"], 3600);
}

TEST_F(SettingsControllerTest, SetWebuiRepairsCorruptFileWithBackup) {
    // 损坏文件: 备份 .corrupt.* 后从空 object 起步修复, 保留 auth/notify 段
    {
        std::ofstream ofs(webui_path_, std::ios::trunc);
        ofs << "{not json";
    }
    auto req = admin_req();
    req->setBody(R"({"token_ttl_sec":7200})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    // 文件被修复为合法 JSON, auth/notify 段存在 (损坏文件从空 object 起步,
    // notify.cache_size 仅由 patch 字段写入, 缺失属预期)
    std::ifstream ifs(webui_path_);
    auto disk = nlohmann::json::parse(ifs);
    EXPECT_EQ(disk["auth"]["token_ttl_sec"], 7200);
    EXPECT_TRUE(disk.contains("notify"));
    EXPECT_TRUE(disk["notify"].is_object());
    // 同目录出现 .corrupt. 前缀备份
    bool found_backup = false;
    const std::string prefix = webui_path_.filename().string() + ".corrupt.";
    for (const auto& entry : std::filesystem::directory_iterator(webui_path_.parent_path())) {
        if (entry.path().filename().string().find(prefix) == 0) {
            found_backup = true;
            break;
        }
    }
    EXPECT_TRUE(found_backup);
}

TEST_F(SettingsControllerTest, SetWebuiPersistsAndHotAppliesCacheSize) {
    auto req = admin_req();
    req->setBody(R"({"notify_cache_size":200})");
    auto resp = invoke(&SettingsCtrl::set_webui, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    // 热生效: 共享持有者已更新
    EXPECT_EQ(webui_cfg_->notify_cache_size, 200);
    // 持久化: 文件已写
    std::ifstream ifs(webui_path_);
    auto disk = nlohmann::json::parse(ifs);
    EXPECT_EQ(disk["notify"]["cache_size"], 200);
}

TEST_F(SettingsControllerTest, GetMasterReadsSections) {
    {
        std::ofstream ofs(master_path_);
        ofs << R"({"master":{"single_stop_timeout_sec":5,"cleanup_max_page_count":100,
                   "cleanup_max_page_age_hours":12},"shm":{"meta_file_size":2097152,"event":{}}})";
    }
    auto resp = invoke(&SettingsCtrl::get_master, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["single_stop_timeout_sec"], 5);
    EXPECT_EQ(body["cleanup_max_page_count"], 100);
    EXPECT_EQ(body["cleanup_max_page_age_hours"], 12);
    EXPECT_EQ(body["meta_file_size"], 2097152);
}

TEST_F(SettingsControllerTest, GetMasterDefaultsWhenMissing) {
    // master_path_ 指向不存在文件
    auto resp = invoke(&SettingsCtrl::get_master, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["single_stop_timeout_sec"], 3);
    EXPECT_EQ(body["cleanup_max_page_count"], 200);
    EXPECT_EQ(body["cleanup_max_page_age_hours"], 24);
    EXPECT_EQ(body["meta_file_size"], 1024 * 1024);
}

TEST_F(SettingsControllerTest, GetMasterClampsStopTimeout) {
    // master 运行时将 <1 的 single_stop_timeout_sec 钳制到 1 (config.cpp), 展示应反映生效值
    {
        std::ofstream ofs(master_path_);
        ofs << R"({"master":{"single_stop_timeout_sec":0}})";
    }
    auto resp = invoke(&SettingsCtrl::get_master, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parse_body(resp)["single_stop_timeout_sec"], 1);
}

TEST_F(SettingsControllerTest, GetMasterFallsBackOnTypeError) {
    // 字段存在但类型非法 (字符串): read_uint 兜底默认值
    {
        std::ofstream ofs(master_path_);
        ofs << R"({"master":{"single_stop_timeout_sec":"abc"},"shm":{}})";
    }
    auto resp = invoke(&SettingsCtrl::get_master, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["single_stop_timeout_sec"], 3);
    EXPECT_EQ(body["cleanup_max_page_count"], 200);
    EXPECT_EQ(body["cleanup_max_page_age_hours"], 24);
    EXPECT_EQ(body["meta_file_size"], 1024 * 1024);
}

TEST_F(SettingsControllerTest, GetMasterFallsBackOnBadJson) {
    // 文件 JSON 损坏: 解析失败走 catch + 默认值, 不 500
    {
        std::ofstream ofs(master_path_);
        ofs << "{not json";
    }
    auto resp = invoke(&SettingsCtrl::get_master, admin_req());
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_body(resp);
    EXPECT_EQ(body["single_stop_timeout_sec"], 3);
    EXPECT_EQ(body["cleanup_max_page_count"], 200);
    EXPECT_EQ(body["cleanup_max_page_age_hours"], 24);
    EXPECT_EQ(body["meta_file_size"], 1024 * 1024);
}

TEST_F(SettingsControllerTest, GetMasterForbidsNonAdmin) {
    auto resp = invoke(&SettingsCtrl::get_master, user_req());
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

}  // namespace
}  // namespace dztrader::webui
