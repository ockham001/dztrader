// 04-process 契约类型层测试：帧号、枚举、结构体、校验
#include <dztrader/core/core_data_type.h>  // 帧号宏
#include <dztrader/platform/process.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dztrader::test {

using namespace dztrader::platform;

// --- 帧号与帧头布局（契约 process：四个帧均使用 DzExtFrameHeader，无 instance_id）---

TEST(ProcessTest, FrameTypes) {
    EXPECT_EQ(DZ_FRAME_REQUEST_PROCESS_CONTROL, static_cast<DzFrameType>(115));
    EXPECT_EQ(DZ_FRAME_RTN_PROCESS_STATUS, static_cast<DzFrameType>(116));
    EXPECT_EQ(DZ_FRAME_SET_PROCESS_CONFIG, static_cast<DzFrameType>(117));
    EXPECT_EQ(DZ_FRAME_RTN_PROCESS_CONFIG, static_cast<DzFrameType>(118));
}

// --- 枚举序列化（契约 process；PascalCase 字符串）---

TEST(ProcessTest, ProcessActionSerializesToPascalCase) {
    EXPECT_EQ(nlohmann::json(ProcessAction::Start).get<std::string>(), "Start");
    EXPECT_EQ(nlohmann::json(ProcessAction::Stop).get<std::string>(), "Stop");
    EXPECT_EQ(nlohmann::json(ProcessAction::Remove).get<std::string>(), "Remove");
}

TEST(ProcessTest, ChildStateSerializesToPascalCase) {
    EXPECT_EQ(nlohmann::json(ChildState::Starting).get<std::string>(), "Starting");
    EXPECT_EQ(nlohmann::json(ChildState::Running).get<std::string>(), "Running");
    EXPECT_EQ(nlohmann::json(ChildState::Stopping).get<std::string>(), "Stopping");
    EXPECT_EQ(nlohmann::json(ChildState::Stopped).get<std::string>(), "Stopped");
    EXPECT_EQ(nlohmann::json(ChildState::Crashed).get<std::string>(), "Crashed");
}

TEST(ProcessTest, ProcessEventSerializesToPascalCase) {
    EXPECT_EQ(nlohmann::json(ProcessEvent::StartSucceeded).get<std::string>(), "StartSucceeded");
    EXPECT_EQ(nlohmann::json(ProcessEvent::StartFailed).get<std::string>(), "StartFailed");
    EXPECT_EQ(nlohmann::json(ProcessEvent::StopSucceeded).get<std::string>(), "StopSucceeded");
    EXPECT_EQ(nlohmann::json(ProcessEvent::StopFailed).get<std::string>(), "StopFailed");
    EXPECT_EQ(nlohmann::json(ProcessEvent::RemoveSucceeded).get<std::string>(), "RemoveSucceeded");
    EXPECT_EQ(nlohmann::json(ProcessEvent::RemoveFailed).get<std::string>(), "RemoveFailed");
}

TEST(ProcessTest, EnumRoundTrip) {
    for (auto a : {ProcessAction::Start, ProcessAction::Stop, ProcessAction::Remove}) {
        EXPECT_EQ(nlohmann::json(a).get<ProcessAction>(), a);
    }
    for (auto s : {ChildState::Starting, ChildState::Running, ChildState::Stopping,
                   ChildState::Stopped, ChildState::Crashed}) {
        EXPECT_EQ(nlohmann::json(s).get<ChildState>(), s);
    }
    for (auto e : {ProcessEvent::StartSucceeded, ProcessEvent::StartFailed,
                   ProcessEvent::StopSucceeded, ProcessEvent::StopFailed,
                   ProcessEvent::RemoveSucceeded, ProcessEvent::RemoveFailed}) {
        EXPECT_EQ(nlohmann::json(e).get<ProcessEvent>(), e);
    }
}

TEST(ProcessTest, EnumRejectsInvalidString) {
    EXPECT_THROW(nlohmann::json("Restart").get<ProcessAction>(), nlohmann::json::exception);
    EXPECT_THROW(nlohmann::json("running").get<ChildState>(), nlohmann::json::exception);
    EXPECT_THROW(nlohmann::json("startsucceeded").get<ProcessEvent>(), nlohmann::json::exception);  // 大小写敏感
}

// --- RestartPolicy（契约 process）---

TEST(ProcessTest, RestartPolicySerializes) {
    RestartPolicy p{.enabled = true, .max_attempts = 5, .backoff_sec = 10};
    nlohmann::json j = p;
    EXPECT_EQ(j["enabled"], true);
    EXPECT_EQ(j["max_attempts"], 5);
    EXPECT_EQ(j["backoff_sec"], 10);
    // 全量角色：三字段必填（契约 process），缺失任一抛异常
    EXPECT_THROW((nlohmann::json{{"enabled", true}, {"max_attempts", 3}}).get<RestartPolicy>(),
                 nlohmann::json::exception);
}

TEST(ProcessTest, RestartPolicyRoundTrip) {
    RestartPolicy p{.enabled = true, .max_attempts = 3, .backoff_sec = 1};
    EXPECT_EQ(nlohmann::json(p).get<RestartPolicy>().max_attempts, 3);
}

// --- ProcessConfig 全量角色（契约 process）---

TEST(ProcessTest, ProcessConfigSerializes) {
    ProcessConfig c;
    c.args = {"--name", "dzmd_ctp"};
    c.env = {{"PATH", "/usr/bin"}};
    c.restart = {.enabled = true, .max_attempts = 5, .backoff_sec = 5};
    c.display_name = "CTP行情源";
    nlohmann::json j = c;
    EXPECT_EQ(j["args"], nlohmann::json::array({"--name", "dzmd_ctp"}));
    EXPECT_EQ(j["env"]["PATH"], "/usr/bin");
    EXPECT_EQ(j["restart"]["enabled"], true);
    EXPECT_EQ(j["display_name"], "CTP行情源");
}

TEST(ProcessTest, ProcessConfigOmitsEmptyDisplayName) {
    // display_name 空串与 nullopt 均省略（RTN wire 等价，契约 process"缺失=未设置"）
    ProcessConfig c;
    c.args = {};
    c.env = {};
    c.restart = {};
    c.display_name = "";
    nlohmann::json j = c;
    EXPECT_FALSE(j.contains("display_name"));
}

TEST(ProcessTest, ProcessConfigRequiresFullFields) {
    // 全量角色 args/env/restart 必须出现（契约 process），缺失抛异常
    EXPECT_THROW((nlohmann::json{{"env", nlohmann::json::object()},
                                {"restart", RestartPolicy{}}}).get<ProcessConfig>(),
                 nlohmann::json::exception);
}

TEST(ProcessTest, ProcessConfigRoundTrip) {
    ProcessConfig c;
    c.args = {"-a"};
    c.env = {{"K", "V"}};
    c.restart = {.enabled = false, .max_attempts = 0, .backoff_sec = 5};
    auto loaded = nlohmann::json(c).get<ProcessConfig>();
    EXPECT_EQ(loaded.args, c.args);
    EXPECT_EQ(loaded.env, c.env);
    EXPECT_EQ(loaded.restart.max_attempts, 0);
    EXPECT_FALSE(loaded.display_name.has_value());
}

TEST(ProcessTest, ProcessConfigEmptyDisplayNameRoundTripsToNullopt) {
    // "" 序列化省略 → 反序列化为 nullopt（wire 语义等价）
    ProcessConfig c;
    c.args = {};
    c.env = {};
    c.restart = {};
    c.display_name = "";
    EXPECT_FALSE(nlohmann::json(c).get<ProcessConfig>().display_name.has_value());
    // 但显式空串可反序列化为 ""（保留内部区分）
    nlohmann::json j = {{"args", nlohmann::json::array()},
                        {"env", nlohmann::json::object()},
                        {"restart", RestartPolicy{}},
                        {"display_name", ""}};
    EXPECT_EQ(j.get<ProcessConfig>().display_name.value_or("x"), "");
}

// --- ProcessControlReq（契约 process）---

TEST(ProcessTest, ProcessControlReqSerializes) {
    ProcessControlReq req{.action = ProcessAction::Start, .target = "dzmd_ctp"};
    nlohmann::json j = req;
    EXPECT_EQ(j["action"], "Start");
    EXPECT_EQ(j["target"], "dzmd_ctp");
    EXPECT_FALSE(j.contains("config"));  // 无 config 不写字段
}

TEST(ProcessTest, ProcessControlReqWithConfigSerializes) {
    ProcessControlReq req{.action = ProcessAction::Start, .target = "dzmd_ctp"};
    req.config = nlohmann::json{{"display_name", "CTP行情源"}};
    nlohmann::json j = req;
    EXPECT_EQ(j["config"]["display_name"], "CTP行情源");
}

TEST(ProcessTest, ProcessControlReqRejectsNullConfig) {
    // config: null 校验失败（契约 process），反序列化抛异常
    EXPECT_THROW((nlohmann::json{{"action", "Start"}, {"target", "dzmd_ctp"},
                                {"config", nullptr}}).get<ProcessControlReq>(),
                 std::runtime_error);
}

// --- ProcessStatus（契约 process）---

TEST(ProcessTest, ProcessStatusSerializes) {
    ProcessStatus st{.name = "dzmd_ctp", .state = ChildState::Running, .pid = 12345};
    st.display_name = "CTP行情";
    st.message = "ok";
    st.event = ProcessEvent::StartSucceeded;
    nlohmann::json j = st;
    EXPECT_EQ(j["name"], "dzmd_ctp");
    EXPECT_EQ(j["state"], "Running");
    EXPECT_EQ(j["pid"], 12345);
    EXPECT_EQ(j["display_name"], "CTP行情");
    EXPECT_EQ(j["message"], "ok");
    EXPECT_EQ(j["event"], "StartSucceeded");
}

TEST(ProcessTest, ProcessStatusOmitsEmptyAndNullopt) {
    // 空串省略（契约 process"缺省为空串"）；event 缺失 = 自发状态变化
    ProcessStatus st{.name = "dzmd_ctp", .state = ChildState::Stopped, .pid = 0};
    nlohmann::json j = st;
    EXPECT_FALSE(j.contains("display_name"));
    EXPECT_FALSE(j.contains("message"));
    EXPECT_FALSE(j.contains("event"));
}

TEST(ProcessTest, ProcessStatusDefaultsOnMissing) {
    nlohmann::json j = {{"name", "dzmd_ctp"}, {"state", "Crashed"}, {"pid", 9}};
    auto st = j.get<ProcessStatus>();
    EXPECT_EQ(st.display_name, "");
    EXPECT_EQ(st.message, "");
    EXPECT_FALSE(st.event.has_value());
}

TEST(ProcessTest, ProcessStatusNullEventMeansSpontaneous) {
    nlohmann::json j = {{"name", "dzmd_ctp"}, {"state", "Stopped"}, {"pid", 0},
                        {"event", nullptr}};
    EXPECT_FALSE(j.get<ProcessStatus>().event.has_value());
}

TEST(ProcessTest, ProcessStatusRoundTrip) {
    ProcessStatus st{.name = "dzmd_ctp", .state = ChildState::Crashed, .pid = 999};
    st.event = ProcessEvent::StartFailed;
    auto loaded = nlohmann::json(st).get<ProcessStatus>();
    EXPECT_EQ(loaded.name, "dzmd_ctp");
    EXPECT_EQ(loaded.state, ChildState::Crashed);
    EXPECT_EQ(loaded.pid, 999);
    EXPECT_EQ(loaded.event.value(), ProcessEvent::StartFailed);
}

// --- SetProcessConfigReq（契约 process，payload 命名见设计文档）---

TEST(ProcessTest, SetProcessConfigReqSerializes) {
    SetProcessConfigReq req{.target = "dzmd_ctp",
                            .config = {{"display_name", "CTP行情源"}}};
    nlohmann::json j = req;
    EXPECT_EQ(j["target"], "dzmd_ctp");
    EXPECT_EQ(j["config"]["display_name"], "CTP行情源");
}

TEST(ProcessTest, SetProcessConfigReqRejectsNullConfig) {
    EXPECT_THROW((nlohmann::json{{"target", "dzmd_ctp"}, {"config", nullptr}})
                     .get<SetProcessConfigReq>(),
                 std::runtime_error);
}

// --- 校验：增量角色 validate_process_config_patch（契约 process）---

TEST(ProcessTest, ValidatePatchAcceptsLegalPatches) {
    // 合法：空 {}（无操作）；env 内部 null（删除 key）；display_name 空串（清空）
    EXPECT_FALSE(validate_process_config_patch(nlohmann::json::object()).has_value());
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"env", {{"OLD_VAR", nullptr}}}}).has_value());
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"display_name", ""}}).has_value());
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"args", nlohmann::json::array()}}).has_value());
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"restart", {{"enabled", true},
                                                 {"max_attempts", 3},
                                                 {"backoff_sec", 10}}}}).has_value());
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"unknown_field", 42}}).has_value());  // 未知字段忽略
}

TEST(ProcessTest, ValidatePatchRejectsNonObject) {
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json(nullptr)).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json::array()).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json("x")).has_value());
}

TEST(ProcessTest, ValidatePatchRejectsNullOutsideEnv) {
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"args", nullptr}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"env", nullptr}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"restart", nullptr}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"display_name", nullptr}}).has_value());
}

TEST(ProcessTest, ValidatePatchRejectsBadTypes) {
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"args", "not-array"}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"args", {1, 2}}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"env", "not-object"}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"env", {{"K", 1}}}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(nlohmann::json{{"display_name", 7}}).has_value());
}

TEST(ProcessTest, ValidatePatchRejectsBadRestart) {
    // restart 三子字段必须全部出现（契约 process）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true}, {"max_attempts", 3}}}})
                    .has_value());
    // enabled 必须为 JSON boolean（拒绝 0/1）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", 1},
                                                {"max_attempts", 3},
                                                {"backoff_sec", 10}}}})
                    .has_value());
    // max_attempts/backoff_sec 必须 >= 0 的整数
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", -1},
                                                {"backoff_sec", 10}}}})
                    .has_value());
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", 3},
                                                {"backoff_sec", "10"}}}})
                    .has_value());
    // 超出 int64 范围的 unsigned 拒绝（避免 get<int64_t>() 抛异常而非返回错误消息）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", 18446744073709551615ULL},
                                                {"backoff_sec", 1}}}})
                    .has_value());
    // 整数值 double 可接受（02-shm 惯例）
    EXPECT_FALSE(validate_process_config_patch(
                     nlohmann::json{{"restart", {{"enabled", true},
                                                 {"max_attempts", 3.0},
                                                 {"backoff_sec", 10.0}}}})
                     .has_value());
    // 超出 int32 范围拒绝（契约字段类型为 int, 避免 get_to(int) 静默回绕）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", 3000000000LL},
                                                {"backoff_sec", 1}}}})
                    .has_value());
}

TEST(ProcessTest, ValidatePatchRejectsNonIntegerValues) {
    // 非整 double (3.5) 拒绝
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", 3.5},
                                                {"backoff_sec", 10}}}})
                    .has_value());
    // NaN 拒绝（isfinite 检查）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts",
                                                 std::numeric_limits<double>::quiet_NaN()},
                                                {"backoff_sec", 10}}}})
                    .has_value());
    // 2^63（double 表示）拒绝（>= 2^63 上界）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", {{"enabled", true},
                                                {"max_attempts", 9.2233720368547758e18},
                                                {"backoff_sec", 10}}}})
                    .has_value());
}

TEST(ProcessTest, ValidatePatchRejectsNonObjectRestart) {
    // restart 必须为 object（数组/字符串/数字均拒绝）
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", nlohmann::json::array()}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", "x"}}).has_value());
    EXPECT_TRUE(validate_process_config_patch(
                    nlohmann::json{{"restart", 3}}).has_value());
}

// --- 校验：全量角色 validate_process_config_full（契约 process）---

TEST(ProcessTest, ValidateFullAcceptsLegalConfig) {
    nlohmann::json cfg = {{"args", nlohmann::json::array()},
                          {"env", nlohmann::json::object()},
                          {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    EXPECT_FALSE(validate_process_config_full(cfg).has_value());
    cfg["display_name"] = "CTP行情源";  // display_name 可选
    EXPECT_FALSE(validate_process_config_full(cfg).has_value());
}

TEST(ProcessTest, ValidateFullRequiresArgsEnvRestart) {
    nlohmann::json cfg = {{"env", nlohmann::json::object()},
                          {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    EXPECT_TRUE(validate_process_config_full(cfg).has_value());  // 缺 args
    cfg["args"] = nlohmann::json::array();
    cfg.erase("env");
    EXPECT_TRUE(validate_process_config_full(cfg).has_value());  // 缺 env
    cfg["env"] = nlohmann::json::object();
    cfg.erase("restart");
    EXPECT_TRUE(validate_process_config_full(cfg).has_value());  // 缺 restart
}

TEST(ProcessTest, ValidateFullRejectsBadValues) {
    nlohmann::json cfg = {{"args", nlohmann::json::array()},
                          {"env", nlohmann::json::object()},
                          {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    cfg["restart"]["max_attempts"] = -2;  // 范围越界（复用 patch 规则）
    EXPECT_TRUE(validate_process_config_full(cfg).has_value());
    // env 内部 value 必须为 string（null 仅增量删除语义, 全量角色拒绝）
    nlohmann::json cfg2 = {{"args", nlohmann::json::array()},
                           {"env", {{"K", nullptr}}},
                           {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    EXPECT_TRUE(validate_process_config_full(cfg2).has_value());
}

TEST(ProcessTest, FromJsonRequiresRequiredFields) {
    // ProcessControlReq: action/target 必填（契约 process）
    EXPECT_THROW((nlohmann::json{{"target", "dzmd_ctp"}}).get<ProcessControlReq>(),
                 nlohmann::json::exception);
    EXPECT_THROW((nlohmann::json{{"action", "Start"}}).get<ProcessControlReq>(),
                 nlohmann::json::exception);
    // ProcessStatus: name/state/pid 必填（契约 process）
    EXPECT_THROW((nlohmann::json{{"state", "Running"}, {"pid", 1}}).get<ProcessStatus>(),
                 nlohmann::json::exception);
    // SetProcessConfigReq: target 必填（契约 process；config 缺失已在
    // SetProcessConfigReqRejectsNullConfig 覆盖 null, 此处覆盖缺失与 target 缺失）
    EXPECT_THROW((nlohmann::json{{"config", nlohmann::json::object()}}).get<SetProcessConfigReq>(),
                 nlohmann::json::exception);
}

TEST(ProcessTest, ValidateFullRejectsNonObject) {
    // full 输入必须为 object（null/数组/字符串均拒绝）
    EXPECT_TRUE(validate_process_config_full(nlohmann::json(nullptr)).has_value());
    EXPECT_TRUE(validate_process_config_full(nlohmann::json::array()).has_value());
    EXPECT_TRUE(validate_process_config_full(nlohmann::json("x")).has_value());
}

// --- apply_process_config_patch（契约 process选择性合并）---

TEST(ProcessTest, ApplyPatchOverridesArgsAndRestart) {
    nlohmann::json current = {{"args", {"--name", "dzmd_ctp"}},
                              {"env", {{"A", "1"}, {"B", "2"}}},
                              {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    nlohmann::json patch = {{"args", {"-x"}},
                            {"restart", {{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 1}}}};
    auto merged = apply_process_config_patch(current, patch);
    EXPECT_EQ(merged["args"], nlohmann::json::array({"-x"}));   // args 整体覆盖（契约 process）
    EXPECT_EQ(merged["restart"]["enabled"], false);             // restart 整体覆盖（契约 process）
    EXPECT_EQ(merged["restart"], patch["restart"]);             // 原子替换：结果与 patch 完全一致（契约 process整体覆盖）
    EXPECT_EQ(merged["env"]["A"], "1");                         // env 未动
    EXPECT_EQ(merged["env"]["B"], "2");
    EXPECT_EQ(current["args"], nlohmann::json::array({"--name", "dzmd_ctp"}));  // current 不变（纯函数）
}

TEST(ProcessTest, ApplyPatchMergesEnvAndDeletesWithNull) {
    nlohmann::json current = {{"args", nlohmann::json::array()},
                              {"env", {{"A", "1"}, {"B", "2"}}},
                              {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    nlohmann::json patch = {{"env", {{"B", nullptr}, {"C", "3"}}}};
    auto merged = apply_process_config_patch(current, patch);
    EXPECT_EQ(merged["env"]["A"], "1");                // 保留（契约 process递归合并）
    EXPECT_FALSE(merged["env"].contains("B"));         // null 删除该 key
    EXPECT_EQ(merged["env"]["C"], "3");                // 新增
}

TEST(ProcessTest, ApplyPatchDisplayNameSemantics) {
    nlohmann::json current = {{"args", nlohmann::json::array()},
                              {"env", nlohmann::json::object()},
                              {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    auto m1 = apply_process_config_patch(current, nlohmann::json{{"display_name", "CTP行情"}});
    EXPECT_EQ(m1["display_name"], "CTP行情");           // 覆盖（契约 process）
    auto m2 = apply_process_config_patch(m1, nlohmann::json{{"display_name", ""}});
    EXPECT_FALSE(m2.contains("display_name"));          // 空串 = 清空（合并结果移除该字段）
}

TEST(ProcessTest, ApplyPatchEmptyAndUnknownFields) {
    nlohmann::json current = {{"args", nlohmann::json::array()},
                              {"env", nlohmann::json::object()},
                              {"restart", {{"enabled", true}, {"max_attempts", 5}, {"backoff_sec", 5}}}};
    EXPECT_EQ(apply_process_config_patch(current, nlohmann::json::object()), current);  // 空 {} 无操作
    auto m = apply_process_config_patch(current, nlohmann::json{{"unknown_field", 42}});
    EXPECT_FALSE(m.contains("unknown_field"));          // 未知字段忽略
}

}  // namespace dztrader::test
