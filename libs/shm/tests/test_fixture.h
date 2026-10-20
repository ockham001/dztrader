#ifndef DZTRADER_SHM_TEST_FIXTURE_H_
#define DZTRADER_SHM_TEST_FIXTURE_H_

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "test_util.h"

namespace dztrader::shm::test {

/// shm 测试共用的唯一通道名 / 临时目录 / 内核对象清理。
/// 单独成头而非并入 test_util.h：PerfBench 也包含 test_util.h 但不链 GTest。
class ShmTestBase : public ::testing::Test {
protected:
    void SetUp() override {
        channel_name_ = unique_channel_name(channel_prefix());
        shm_dir_ = test_shm_dir(channel_name_);
    }

    void TearDown() override { cleanup_channel(channel_name_, shm_dir_); }

    [[nodiscard]] virtual std::string channel_prefix() const = 0;

    std::string channel_name_;
    std::filesystem::path shm_dir_;
};

}  // namespace dztrader::shm::test

#endif  // DZTRADER_SHM_TEST_FIXTURE_H_
