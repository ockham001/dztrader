#include "mirror_store.h"
#include <gtest/gtest.h>

namespace dztrader::webui {
namespace {

TEST(MirrorStoreTest, InitiallyEmpty) {
    const MirrorStore m;
    EXPECT_TRUE(m.snapshot().empty());
    EXPECT_TRUE(m.instance("dzmd_ctp").empty());
}

TEST(MirrorStoreTest, UpdateAndReadDomain) {
    MirrorStore m;
    m.update("dzmd_ctp", "log_config", nlohmann::json{{"level", "info"}});
    EXPECT_EQ(m.instance("dzmd_ctp")["log_config"]["level"], "info");
    // 同实例另一领域互不影响
    m.update("dzmd_ctp", "md_status", nlohmann::json{{"trading_day", "20260812"}});
    EXPECT_EQ(m.instance("dzmd_ctp")["log_config"]["level"], "info");
    EXPECT_EQ(m.instance("dzmd_ctp")["md_status"]["trading_day"], "20260812");
    // 快照含全部
    EXPECT_EQ(m.snapshot()["dzmd_ctp"]["log_config"]["level"], "info");
}

TEST(MirrorStoreTest, UpdateOverwritesSameDomain) {
    MirrorStore m;
    m.update("dzmd_ctp", "log_config", nlohmann::json{{"level", "debug"}});
    m.update("dzmd_ctp", "log_config", nlohmann::json{{"level", "warning"}});
    EXPECT_EQ(m.instance("dzmd_ctp")["log_config"]["level"], "warning");
    EXPECT_EQ(m.snapshot()["dzmd_ctp"]["log_config"].size(), 1u);  // 覆盖而非累积
}

TEST(MirrorStoreTest, EraseDomain) {
    MirrorStore m;
    m.update("dzmd_ctp", "log_config", nlohmann::json{{"level", "info"}});
    m.update("dzmd_ctp", "md_status", nlohmann::json{{"trading_day", "20260812"}});
    m.erase("dzmd_ctp", "md_status");
    EXPECT_FALSE(m.instance("dzmd_ctp").contains("md_status"));
    EXPECT_TRUE(m.instance("dzmd_ctp").contains("log_config"));  // 其他领域保留
    m.erase("dzmd_ctp", "md_status");  // 幂等：不存在不报错
    m.erase("no_such_instance", "log_config");  // 实例不存在不报错
}

TEST(MirrorStoreTest, RemoveInstance) {
    MirrorStore m;
    m.update("dzmd_ctp", "log_config", nlohmann::json{{"level", "info"}});
    m.update("dztraderd", "event_shm_config", nlohmann::json{{"check_interval_min", 5}});
    m.remove("dzmd_ctp");
    EXPECT_FALSE(m.snapshot().contains("dzmd_ctp"));
    EXPECT_TRUE(m.snapshot().contains("dztraderd"));  // 其他实例保留
    m.remove("dzmd_ctp");  // 幂等
}

TEST(MirrorStoreTest, SnapshotIsLiveView) {
    MirrorStore m;
    m.update("a", "x", nlohmann::json{{"v", 1}});
    const auto& snap = m.snapshot();
    m.update("a", "x", nlohmann::json{{"v", 2}});
    EXPECT_EQ(snap["a"]["x"]["v"], 2);  // 引用视图，实时反映
}

}  // namespace
}  // namespace dztrader::webui
