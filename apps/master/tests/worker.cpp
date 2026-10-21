#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

static volatile bool g_running = true;

int main(int argc, char* argv[]) {
    std::string name = (argc > 1) ? argv[1] : "worker";
    int runtime = 0;

    // 崩溃一次模式: crash_once <marker_path> <exit_code>
    // 首次调用写 marker 并以 exit_code 退出; master 自动重启后的实例读到 marker
    // 落回常驻模式, 用于验证重启策略 (首次崩溃 -> 退避重启 -> 重启后确实在运行)。
    if (argc > 3 && name == "crash_once") {
        const std::filesystem::path marker = argv[2];
        const int crash_code = std::stoi(argv[3]);
        if (!std::filesystem::exists(marker)) {
            std::ofstream(marker) << "crashed\n";
            std::cout << "worker crash_once | marker=" << marker.string() << std::endl;
            return crash_code;
        }
        std::cout << "worker recovered | marker=" << marker.string() << std::endl;
        runtime = 60;
    } else if (argc > 2) {
        runtime = std::stoi(argv[2]);
    }

    std::signal(SIGINT, [](int) { g_running = false; });
    std::signal(SIGTERM, [](int) { g_running = false; });

    std::cout << "worker started | name=" << name << std::endl;

    // 一次性输出模式: argc > 3 时第三个参数为退出码, 打印一行 stdout + 一行 stderr
    // 后立即退出 (用于验证 master 的转发内容与非零退出码透传)
    // crash_once 的恢复实例同样 argc > 3, 必须排除 (否则会再次以退出码退出)。
    if (argc > 3 && name != "crash_once") {
        const int exit_code = std::stoi(argv[3]);
        std::cout << "worker stdout | name=" << name << " note=\"a\\b\"c\"" << std::endl;
        std::cerr << "worker stderr | name=" << name << " note=\"a\\b\"c\"" << std::endl;
        return exit_code;
    }

    auto start = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "heartbeat | name=" << name << std::endl;

        if (runtime > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= runtime) {
                std::cout << "runtime expired | name=" << name << std::endl;
                break;
            }
        }
    }

    std::cout << "worker exiting | name=" << name << std::endl;
    return 0;
}
