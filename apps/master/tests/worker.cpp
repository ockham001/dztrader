#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

static volatile bool g_running = true;

int main(int argc, char* argv[]) {
    std::string name = (argc > 1) ? argv[1] : "worker";
    int runtime = (argc > 2) ? std::stoi(argv[2]) : 0;

    std::signal(SIGINT, [](int) { g_running = false; });
    std::signal(SIGTERM, [](int) { g_running = false; });

    std::cout << "worker started | name=" << name << std::endl;

    // 一次性输出模式: argc > 3 时第三个参数为退出码, 打印一行 stdout + 一行 stderr
    // 后立即退出 (用于验证 master 的转发内容与非零退出码透传)
    if (argc > 3) {
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
