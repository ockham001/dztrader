#ifndef DZTRADER_WEBUI_TESTS_READY_SHM_FIXTURE_H_
#define DZTRADER_WEBUI_TESTS_READY_SHM_FIXTURE_H_

/**
 * @file ready_shm_fixture.h
 * @brief 真 SHM 事件通道 fixture 的**唯一实现**：捕获 dzweb 写出的定向帧（帧断言用）。
 *
 * 为什么必须只有一份（`docs/unit-test-standard.md` §5.2/§5.4）：`td_account_controller_test`
 * 与 `td_control_domain_service_test` 原先各复制了一份逐行同款 fixture（通道名/配置/cleanup/
 * 帧解码全同）。这类夹具一旦分叉，帧解码或清理逻辑的修复只会落在其中一份，另一份仍绿。
 *
 * **header-only**：`apps/webui/CMakeLists.txt` 为 `tests/` 下每个测试源文件建一个 target，
 * 没有共享测试库；与 `libs/` 各模块的共享夹具一样用 inline 头。
 *
 * 通道名按 pid + 随机 + 实例序号唯一化（ctest -j 下不同测试进程互不干扰）。
 */
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "test_temp_dir.h"
#include "test_util.h"

namespace dztrader::test {

/// 真 SHM 通道 fixture：写入端由被测代码持有（ShmWriter 等），本类只负责建通道/读帧/清理。
struct ReadyShmFixture {
    std::string channel_name;
    std::filesystem::path shm_dir;
    std::shared_ptr<shm::ChannelMeta> meta;
    std::shared_ptr<shm::MultiWriter> writer;
    std::optional<shm::Reader> reader;
    static constexpr uint64_t MB = 1024ull * 1024;

    explicit ReadyShmFixture(const std::string& channel_prefix = "dz_test_webui_ready_shm") {
        static std::atomic<uint64_t> instance_seq{0};
        channel_name = channel_prefix + "_" + unique_suffix() + "_" +
                       std::to_string(instance_seq.fetch_add(1, std::memory_order_relaxed));
        shm_dir = std::filesystem::temp_directory_path() / channel_name;
        std::filesystem::remove_all(shm_dir);
        std::filesystem::create_directories(shm_dir);
        const shm::ChannelConfig cfg{.channel_name = channel_name,
                                     .shm_dir = shm_dir,
                                     .meta_file_size = 4 * MB,
                                     .page_size = 1 * MB,
                                     .lock_memory = false,
                                     .prefetch_memory = false};
        meta = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer = std::make_shared<shm::MultiWriter>(shm::MultiWriter::create(meta, "test_writer"));
        reader = shm::Reader::create(meta, "test_reader");
    }
    ~ReadyShmFixture() {
        reader.reset();
        writer.reset();
        meta.reset();
        shm::test::cleanup_channel(channel_name, shm_dir);
    }
    ReadyShmFixture(const ReadyShmFixture&) = delete;
    ReadyShmFixture& operator=(const ReadyShmFixture&) = delete;

    struct Captured {
        bool found = false;
        std::string instance_id;
        nlohmann::json payload;
    };

    /// 排空 reader，返回第一个匹配 type + instance_id 的定向帧 payload。
    Captured capture_inst_frame(DzFrameType type, const std::string& instance) {
        Captured out;
        while (const auto* raw = reader->next_frame()) {
            const shm::FrameView view(raw);
            if (view.type() != type) {
                continue;
            }
            if (std::string(view.ext_inst_id()) != instance) {
                continue;
            }
            const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
            out.payload = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            out.instance_id = instance;
            out.found = true;
            break;
        }
        return out;
    }

    /// 排空 reader，返回第一个匹配 type 的无 instance_id（ext）帧 payload（空 payload → {}）。
    /// 用于 `REQUEST_PROCESS_CONTROL` 这类无 instance_id 的定向到 master 的帧。
    Captured capture_ext_frame(DzFrameType type) {
        Captured out;
        while (const auto* raw = reader->next_frame()) {
            const shm::FrameView view(raw);
            if (view.type() != type) {
                continue;
            }
            const auto size = view.ext_payload_size();
            if (size == 0) {
                out.payload = nlohmann::json::object();
            } else {
                const auto* data = reinterpret_cast<const char*>(view.ext_payload());
                out.payload = nlohmann::json::parse(data, data + size);
            }
            out.found = true;
            break;
        }
        return out;
    }

    /// 是否写入了任何帧（守卫失败路径必须一帧不写）；会排空 reader。
    bool has_any_frame() {
        while (const auto* raw = reader->next_frame()) {
            (void)raw;
            return true;
        }
        return false;
    }
};

}  // namespace dztrader::test

#endif  // DZTRADER_WEBUI_TESTS_READY_SHM_FIXTURE_H_
