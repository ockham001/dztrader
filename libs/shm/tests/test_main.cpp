#include <gtest/gtest.h>
#include <string>
#include "test_ipc_util.h"

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--dz-test-helper") {
        return dztrader::shm::test::helper::run(argc - 2, argv + 2);
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
