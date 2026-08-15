#ifndef DZTRADER_SHM_TEST_IPC_UTIL_H_
#define DZTRADER_SHM_TEST_IPC_UTIL_H_

#include <boost/asio/io_context.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/process/v2/process.hpp>
#ifdef _WIN32
#include <boost/process/v2/windows/creation_flags.hpp>
#endif
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/common.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/process_mutex.h>
#include <dztrader/struct.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace bpv = boost::process::v2;

namespace dztrader::shm::test {

inline bpv::process spawn_helper(boost::asio::io_context& ctx, std::vector<std::string> args) {
    auto self = boost::dll::program_location();
    args.insert(args.begin(), "--dz-test-helper");
#ifdef _WIN32
    return bpv::process(ctx, self.string(), args, bpv::windows::create_new_process_group);
#else
    return bpv::process(ctx, self.string(), args);
#endif
}

inline int wait_with_timeout(bpv::process& proc, std::chrono::steady_clock::duration timeout) {
    auto fut = std::async(std::launch::async, [&] { return proc.wait(); });
    auto status = fut.wait_for(timeout);
    if (status != std::future_status::ready) {
        try {
            proc.terminate();
        } catch (...) {
        }
        return -1;
    }
    return fut.get();
}

inline bool wait_for_file(const std::filesystem::path& path,
                          std::chrono::steady_clock::duration timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

namespace helper {

inline int mutex_lock_and_hold(const std::string& name,
                               int hold_ms,
                               const std::string& signal_file = "") {
    try {
        dztrader::shm::ProcessMutex mtx(name);
        mtx.lock();
        if (!signal_file.empty()) {
            std::ofstream ofs(signal_file);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        mtx.unlock();
        return 0;
    } catch (...) {
        return 1;
    }
}

inline int sem_notify(const std::string& name) {
    try {
        dztrader::shm::NamedSemaphore sem(name);
        sem.notify();
        return 0;
    } catch (...) {
        return 1;
    }
}

inline int sem_wait(const std::string& name) {
    try {
        dztrader::shm::NamedSemaphore sem(name);
        sem.wait();
        return 0;
    } catch (...) {
        return 1;
    }
}

inline int sem_wait_for_and_notify(const std::string& name, int timeout_ms) {
    if (timeout_ms < 0) return 1;
    try {
        dztrader::shm::NamedSemaphore sem(name);
        if (sem.wait_for(static_cast<uint32_t>(timeout_ms))) {
            sem.notify();
            return 0;
        }
        return 2;
    } catch (...) {
        return 1;
    }
}

// SingleWriter: single-producer scenario (normal write path)
inline int channel_writer_write(const std::string& channel_name, const std::string& shm_dir_str) {
    try {
        std::filesystem::path shm_dir(shm_dir_str);
        ChannelConfig config{
            .channel_name = channel_name,
            .shm_dir = shm_dir,
            .meta_file_size = 4 * 1024 * 1024,
            .page_size = 1 * 1024 * 1024,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
        auto writer = SingleWriter::create(meta, "gw.helper");

        constexpr uint32_t data_size = 56;
        auto* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        if (!payload) {
            return 1;
        }
        std::memset(payload, 0xAB, data_size);
        writer.close_frame();
        writer.notify_subscribers();
        return 0;
    } catch (...) {
        return 1;
    }
}

// MultiWriter: multi-producer scenario (crash/restart uses MultiWriter for recovery)
inline int channel_writer_open_frame(const std::string& channel_name,
                                     const std::string& shm_dir_str) {
    try {
        std::filesystem::path shm_dir(shm_dir_str);
        ChannelConfig config{
            .channel_name = channel_name,
            .shm_dir = shm_dir,
            .meta_file_size = 4 * 1024 * 1024,
            .page_size = 1 * 1024 * 1024,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
        auto writer = MultiWriter::create(meta, "gw.helper");

        constexpr uint32_t data_size = 56;
        auto* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
        if (!payload) {
            return 1;
        }
        std::memset(payload, 0xAB, data_size);

        std::this_thread::sleep_for(std::chrono::hours(1));
        writer.close_frame();
        return 0;
    } catch (...) {
        return 1;
    }
}

// MultiWriter: continuous writes crossing multiple pages. Used by the deadlock
// regression test: while this helper runs (page-cross acquires write→meta), the
// main process concurrently invokes the open_or_create ctor (also write→meta).
// With the correct lock order both serialize on `write` and make progress; an
// AB-BA inversion would hang forever (caught by wait_with_timeout's 5s guard).
// Writing 80000 frames at ~64B/frame on a 1MB page crosses ~5 pages, exercising
// the meta-lock handoff on each page-cross. `signal_file` is created once the
// writer has started, so the main process can begin racing ctor calls against
// the page-cross path.
inline int channel_writer_continuous_cross(const std::string& channel_name,
                                           const std::string& shm_dir_str,
                                           const std::string& signal_file) {
    try {
        std::filesystem::path shm_dir(shm_dir_str);
        ChannelConfig config{
            .channel_name = channel_name,
            .shm_dir = shm_dir,
            .meta_file_size = 4 * 1024 * 1024,
            .page_size = 1 * 1024 * 1024,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
        auto writer = MultiWriter::create(meta, "gw.helper");

        if (!signal_file.empty()) {
            std::ofstream ofs(signal_file);
        }

        constexpr uint32_t data_size = 56;
        constexpr int num_frames = 80000;  // ~5 page-crosses on a 1MB page
        for (int i = 0; i < num_frames; ++i) {
            auto* payload = writer.open_frame(DZ_FRAME_RTN_MD_TICK, data_size);
            if (!payload) {
                return 1;
            }
            std::memset(payload, 0xAB, data_size);
            writer.close_frame();
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

// Holds a raw file_mapping on a page file WITHOUT registering as a Reader
// (therefore invisible to the cleaner's reader-map). Used by the cleaner
// multi-process test to exercise the remove-failure skip path:
//  - Linux: std::filesystem::remove succeeds (unlink) even while mapped; the
//    helper's mapping stays valid via inode refcount until it unmaps.
//  - Windows: remove fails on a mapped file → the cleaner skips with a warning
//    and leaves the file in place; the helper's mapping stays valid.
// In both cases the cleaner must not throw and the helper's mapping must remain
// readable. `signal_file` is created once the mapping is established so the
// main process runs the cleaner only while the mapping is held.
inline int file_mapping_hold(const std::string& channel_name,
                             const std::string& shm_dir_str,
                             uint64_t page_id,
                             int hold_ms,
                             const std::string& signal_file) {
    try {
        namespace bip = boost::interprocess;
        std::filesystem::path shm_dir(shm_dir_str);
        ChannelConfig config{
            .channel_name = channel_name,
            .shm_dir = shm_dir,
            .meta_file_size = 4 * 1024 * 1024,
            .page_size = 1 * 1024 * 1024,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        // open_or_create on an existing channel (matching page_size) does NOT
        // reset state; we use it only to resolve page_dir()/page_size() without
        // duplicating the layout convention. Crucially we do NOT call
        // add_reader, so the cleaner's reader-map stays empty.
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(config));
        auto file_path = meta->page_dir() / std::format("{:08d}.dat", page_id);

        bip::file_mapping mfile(file_path.string().c_str(), bip::read_write);
        bip::mapped_region region(mfile, bip::read_write, 0,
                                  static_cast<std::size_t>(meta->page_size()));
        auto* addr = static_cast<std::byte*>(region.get_address());
        if (addr == nullptr) {
            return 1;
        }
        // Write a sentinel so we can verify the mapping stays readable after
        // the cleaner runs (the cleaner only removes files, never writes to
        // them, so the byte must be unchanged).
        addr[0] = static_cast<std::byte>(0xCD);

        if (!signal_file.empty()) {
            std::ofstream ofs(signal_file);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

        if (addr[0] != static_cast<std::byte>(0xCD)) {
            return 2;
        }
        return 0;
    } catch (...) {
        return 1;
    }
}

inline int run(int argc, char* argv[]) {
    if (argc < 1) {
        std::cerr << "usage: --dz-test-helper <command> [args...]\n";
        return 3;
    }

    std::string cmd = argv[0];

    if (cmd == "mutex_lock_and_hold" && argc >= 3) {
        std::string sig = (argc >= 4) ? argv[3] : "";
        return mutex_lock_and_hold(argv[1], std::atoi(argv[2]), sig);
    }
    if (cmd == "sem_notify" && argc >= 2) {
        return sem_notify(argv[1]);
    }
    if (cmd == "sem_wait" && argc >= 2) {
        return sem_wait(argv[1]);
    }
    if (cmd == "sem_wait_for_and_notify" && argc >= 3) {
        return sem_wait_for_and_notify(argv[1], std::atoi(argv[2]));
    }
    if (cmd == "channel_writer_write" && argc >= 3) {
        return channel_writer_write(argv[1], argv[2]);
    }
    if (cmd == "channel_writer_open_frame" && argc >= 3) {
        return channel_writer_open_frame(argv[1], argv[2]);
    }
    if (cmd == "channel_writer_continuous_cross" && argc >= 3) {
        std::string sig = (argc >= 4) ? argv[3] : "";
        return channel_writer_continuous_cross(argv[1], argv[2], sig);
    }
    if (cmd == "file_mapping_hold" && argc >= 5) {
        std::string sig = (argc >= 6) ? argv[5] : "";
        return file_mapping_hold(argv[1], argv[2], std::stoull(argv[3]),
                                 std::atoi(argv[4]), sig);
    }

    std::cerr << "unknown command or missing args: " << cmd << "\n";
    return 3;
}

}  // namespace helper

}  // namespace dztrader::shm::test

#endif  // DZTRADER_SHM_TEST_IPC_UTIL_H_
