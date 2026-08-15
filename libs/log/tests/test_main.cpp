#include <boost/filesystem.hpp>
#include <filesystem>
#include <gtest/gtest.h>

namespace {

std::filesystem::path g_test_root;

void ensure_test_root()
{
    if (!g_test_root.empty()) {
        return;
    }
    auto model = boost::filesystem::path((std::filesystem::temp_directory_path() / "dz-test-%%%%-%%%%").string());
    auto unique = boost::filesystem::unique_path(model);
    std::filesystem::create_directories(unique.string());
    g_test_root = std::filesystem::path(unique.string());
}

void cleanup_test_root()
{
    if (!g_test_root.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(g_test_root, ec);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    ensure_test_root();
    std::atexit(cleanup_test_root);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

const std::filesystem::path& test_root()
{
    return g_test_root;
}
