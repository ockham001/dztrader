#include <dztrader/log/log.h>
#include <gtest/gtest.h>

// 独立测试二进制：进程内没有任何 set_default_logger() 调用，
// 保证 forward_logger() 的「未初始化」分支真实执行，不依赖其他用例的执行顺序。
TEST(ForwardLoggerTest, ReturnsNullptrBeforeSetDefaultLogger) {
    EXPECT_EQ(dztrader::log::forward_logger(), nullptr);
}
