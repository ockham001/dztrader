#include "frame_router.h"
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/core/path.h>
#include <dztrader/core/core_data_type.h>
#include <gtest/gtest.h>

namespace dztrader::webui {
namespace {

// 同步 poster：立即执行（单测不跑 drogon 事件循环）
FrameRouter make_router() {
    return FrameRouter([](std::function<void()> f) { f(); });
}

// 写一帧原始字节到真实 SHM 通道并读回 FrameView（复用 ws_log_report_test 模式）
// 注意：Reader::create 的 read_pos 初始化为当前写入位置（reader.cpp:30），
// 因此只读到"本用例写入之后"的帧——多用例共享 dzevent 通道天然隔离，无需 drain。
// 返回 {FrameView, Reader}：Reader 必须随 FrameView 存活（析构会 unmap 页映射，
// 导致 FrameView 悬垂——首轮实测 SEH 0xc0000005），测试体内保持 reader 到断言结束。
std::pair<shm::FrameView, shm::Reader> write_bytes_and_read(DzFrameType type,
                                                            const std::string& inst,
                                                            const std::byte* data,
                                                            uint32_t size) {
    auto shm_dir = dztrader::paths::shm();
    const shm::ChannelConfig evt_cfg{
        .channel_name = dztrader::shm::channel_name("dzevent"),
        .shm_dir = shm_dir,
        .meta_file_size = uint64_t{1} * 1024 * 1024,
        .page_size = uint64_t{1024} * 1024,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)shm::ChannelMeta::open_or_create(evt_cfg);
    auto reader =
        shm::Reader::create(dztrader::shm::channel_name("dzevent"), shm_dir, "test_router_reader");
    auto meta = shm::ChannelMeta::open_only(dztrader::shm::channel_name("dzevent"), shm_dir);
    auto writer = shm::MultiWriter::create(std::make_shared<shm::ChannelMeta>(std::move(meta)),
                                           "test_router_writer");
    if (inst.empty()) {
        EXPECT_TRUE(writer.write_ext_frame(type, data, size));
    } else {
        EXPECT_TRUE(writer.write_ext_inst_frame(type, inst.c_str(), data, size));
    }
    writer.notify_subscribers();
    const std::byte* raw = reader.next_frame();
    EXPECT_NE(raw, nullptr);
    return {shm::FrameView(raw), std::move(reader)};
}

std::pair<shm::FrameView, shm::Reader> write_and_read(DzFrameType type,
                                                      const std::string& inst,
                                                      const nlohmann::json& payload) {
    const std::string bytes = payload.dump();
    return write_bytes_and_read(type, inst, reinterpret_cast<const std::byte*>(bytes.data()),
                                static_cast<uint32_t>(bytes.size()));
}

TEST(FrameRouterTest, DispatchToRegisteredJsonHandler) {
    auto router = make_router();
    std::string got_source;
    nlohmann::json got_payload;
    router.register_json<nlohmann::json>(DZ_FRAME_RTN_LOG_CONFIG, true,
                                         [&](const std::string& source, const nlohmann::json& p) {
                                             got_source = source;
                                             got_payload = p;
                                         });
    auto [view, reader] =
        write_and_read(DZ_FRAME_RTN_LOG_CONFIG, "dzmd_ctp",
                       nlohmann::json{{"level", "info"}, {"flush_on", "warning"}});
    (void)reader;
    router.dispatch(view);
    EXPECT_EQ(got_source, "dzmd_ctp");
    EXPECT_EQ(got_payload["level"], "info");
}

TEST(FrameRouterTest, DispatchNonInstJsonHandler) {
    auto router = make_router();
    std::string got_source = "unset";
    router.register_json<nlohmann::json>(
        DZ_FRAME_RTN_PROCESS_CONFIG, false,
        [&](const std::string& source, const nlohmann::json& /*p*/) { got_source = source; });
    auto [view, reader] = write_and_read(DZ_FRAME_RTN_PROCESS_CONFIG, "", nlohmann::json::object());
    (void)reader;
    router.dispatch(view);
    EXPECT_EQ(got_source, "");  // 无 instance_id 帧 source 为空串
}

TEST(FrameRouterTest, InvalidJsonPayloadDroppedWithoutCallingHandler) {
    auto router = make_router();
    int calls = 0;
    router.register_json<nlohmann::json>(
        DZ_FRAME_RTN_LOG_CONFIG, true, [&](const std::string&, const nlohmann::json&) { ++calls; });
    const std::string truncated = R"({"level": "info")";  // 截断：缺右花括号
    auto [view, reader] = write_bytes_and_read(DZ_FRAME_RTN_LOG_CONFIG, "dzmd_ctp",
                                               reinterpret_cast<const std::byte*>(truncated.data()),
                                               static_cast<uint32_t>(truncated.size()));
    (void)reader;
    EXPECT_NO_THROW(router.dispatch(view));  // 解码失败只丢帧，不扩散异常
    EXPECT_EQ(calls, 0);                     // handler 不得被调用
}

TEST(FrameRouterTest, UnregisteredTypeIgnored) {
    auto router = make_router();
    int calls = 0;
    // 同为实例帧布局：若 dispatch 误落到已注册 handler，解码会成功并计数
    router.register_json<nlohmann::json>(
        DZ_FRAME_RTN_LOG_CONFIG, true, [&](const std::string&, const nlohmann::json&) { ++calls; });
    auto [view, reader] =
        write_and_read(DZ_FRAME_RTN_MD_STATUS, "dzmd_ctp", nlohmann::json::object());
    (void)reader;
    EXPECT_NO_THROW(router.dispatch(view));  // 未注册：忽略不崩
    EXPECT_EQ(calls, 0);                     // 也不得触发任何已注册 handler
}

TEST(FrameRouterTest, RawHandlerReceivesFrame) {
    auto router = make_router();
    int calls = 0;
    router.register_raw(DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER,
                        [&](const shm::FrameView&) { ++calls; });
    auto [view, reader] =
        write_and_read(DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER, "", nlohmann::json::object());
    (void)reader;
    router.dispatch(view);
    EXPECT_EQ(calls, 1);
}

TEST(FrameRouterTest, HandlerExceptionIsolated) {
    // 自定义 poster：记录投递 callable 是否把异常抛了出来。
    // 正确实现第二层 catch 兜底 → f() 不抛（posted_exceptions 恒 0）；若删掉该 catch，
    // 异常会冒到 poster 这里（同步 poster 下即可观察到）。
    int posted_exceptions = 0;
    FrameRouter router([&](std::function<void()> f) {
        try {
            f();
        } catch (...) {
            ++posted_exceptions;
        }
    });
    router.register_json<nlohmann::json>(
        DZ_FRAME_RTN_LOG_CONFIG, true,
        [&](const std::string&, const nlohmann::json&) { throw std::runtime_error("boom"); });
    int md_calls = 0;
    router.register_json<nlohmann::json>(
        DZ_FRAME_RTN_MD_STATUS, true,
        [&](const std::string&, const nlohmann::json&) { ++md_calls; });
    auto [v1, r1] = write_and_read(DZ_FRAME_RTN_LOG_CONFIG, "a", nlohmann::json{{"level", "info"}});
    (void)r1;
    auto [v2, r2] = write_and_read(DZ_FRAME_RTN_MD_STATUS, "b", nlohmann::json::object());
    (void)r2;
    EXPECT_NO_THROW(router.dispatch(v1));  // handler 抛异常被第二层兜底
    EXPECT_EQ(posted_exceptions, 0);       // 投递的 callable 本身不抛
    EXPECT_NO_THROW(router.dispatch(v2));  // 后续帧不受影响
    EXPECT_EQ(md_calls, 1);                // 后续帧确实被派发
}

}  // namespace
}  // namespace dztrader::webui
