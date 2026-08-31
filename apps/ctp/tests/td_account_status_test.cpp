// td_account_status_test: 2115 查询路由纯函数 account_query_matches 单元测试。
// account_query_matches 实现在 td_api_pure.cpp, 不依赖 TdApi 类和 CTP 头文件,
// 因此测试目标无需链接 dztd_ctp_api (与 td_api_pure_test.cpp 同款 extern 声明模式)。

#include <gtest/gtest.h>

#include <string>
#include <vector>

// 与既有 td_api_pure_test.cpp 同款 extern 声明模式 (该文件在 namespace dztrader::ctp
// 内 extern 声明, 不 include td_api.h——它携带 TdApi 类定义, 测试目标未链接完整依赖)。
// 本文件同做法: 在 namespace dztrader::ctp {} 内 extern 声明, 签名与 td_api.h 纯函数区一致
namespace dztrader::ctp {
bool account_query_matches(const std::vector<std::string>& configured,
                           const std::string& queried);
}  // namespace dztrader::ctp

TEST(AccountQueryRoute, EmptyQueryMeansAll) {
    EXPECT_TRUE(dztrader::ctp::account_query_matches({"A", "B"}, ""));
}
TEST(AccountQueryRoute, ConfiguredAccountMatches) {
    EXPECT_TRUE(dztrader::ctp::account_query_matches({"A", "B"}, "A"));
}
TEST(AccountQueryRoute, UnconfiguredNotMatched) {
    EXPECT_FALSE(dztrader::ctp::account_query_matches({"A", "B"}, "C"));
}
